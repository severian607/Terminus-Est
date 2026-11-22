// TaskService: Tasks.org / CalDAV VTODO sync over an encrypted offline journal, with local Trash and history.
// Cycle: tick -> storage loaded -> password -> REPORT (reconcile) -> replay one queued PUT/DELETE -> REPORT again.

#include "tasks/task_service.h"

#include "calendar/calendar_credential_store.h"
#include "config/config_service.h"
#include "net/http_client.h"
#include "security/encrypted_file_store.h"
#include "security/secret_store.h"
#include "security/storage_key_provider.h"
#include "tasks/task_codec.h"
#include "util/file_utils.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sodium.h>
#include <utility>

namespace {
  using Clock = std::chrono::steady_clock;
  using Json = nlohmann::json;

  constexpr std::size_t MaxStateBytes = 64 * 1024 * 1024;
  constexpr int StateVersion = 2;
  constexpr int LegacyStateVersion = 1;
  constexpr int MinSyncIntervalSeconds = 15;
  constexpr int MaxSyncIntervalSeconds = 24 * 60 * 60;
  constexpr int HttpMultiStatus = 207;
  constexpr int HttpUnauthorized = 401;
  constexpr int HttpForbidden = 403;
  constexpr int HttpNotFound = 404;
  constexpr int HttpGone = 410;
  constexpr int HttpPreconditionFailed = 412;
  constexpr const char* SourceDesktop = "Desktop";
  constexpr const char* SourceServer = "Server";
  constexpr const char* StateContextLabel = "tasks-state-v1";
  constexpr const char* TaskQuery = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                                    "<c:calendar-query xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
                                    "<d:prop><d:getetag/><c:calendar-data/></d:prop><c:filter>"
                                    "<c:comp-filter name=\"VCALENDAR\"><c:comp-filter name=\"VTODO\"/>"
                                    "</c:comp-filter></c:filter></c:calendar-query>";

  std::filesystem::path stateDirectory() {
    const std::filesystem::path base = FileUtils::stateDir();
    if (base.empty() || !base.is_absolute())
      return {};
    return base / "tasks";
  }
  std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  // One locally queued change for a task: an edit, a creation or a deletion.
  struct Pending {
    std::string baseEtag, desired;
    bool removed = false, conflict = false;
    // Persist before sending. A REPORT resolves uncertain responses after crashes and transport failures.
    std::string sent;
    bool sentDelete = false;
    // Once a DELETE was attempted, a stale REPORT is not sufficient evidence for safe undo.
    bool deleteAttempted = false;
    // An absent REPORT cannot rule out a delayed creating PUT after a timeout.
    bool createAttempted = false;
    bool operator==(const Pending&) const = default;

    // The server already holds exactly what we last sent.
    [[nodiscard]] bool sentMatches(const std::string& serverData) const {
      return !sent.empty() && tasks::equivalent(serverData, sent);
    }
    // A queued deletion that never reached the network, so undoing it is safe.
    [[nodiscard]] bool isUnsentDeletion() const {
      return removed && !conflict && sent.empty() && !sentDelete && !deleteAttempted && !createAttempted;
    }
  };
  using Journal = std::map<std::string, Pending>;

  struct Archived {
    std::string taskId, data, source;
    TaskItem item;
    std::int64_t deletedAt = 0;
    // A local delete retains its previous intent until its absence is confirmed.
    bool queued = false;
    std::optional<Pending> prior;
  };
  using Archive = std::map<std::string, std::shared_ptr<const Archived>>;
  using History = std::vector<TaskHistoryEntry>;

  // The mutable local records; edits stage a copy and commit it only after it has been persisted.
  struct Records {
    Journal journal;
    Archive archive;
    History history;
  };

  // Which parts of the published snapshot must be rebuilt; the rest is reused between publishes.
  struct ViewDirty {
    bool items = true, trash = true, history = true;
    void all() { items = trash = history = true; }
  };

  // Working copy of a REPORT reconciliation. Archive and history are copied only once something writes to them.
  class Reconciliation {
  public:
    Journal pending;
    bool resourcesChanged = false;
    bool deferRetry = false;

    Reconciliation(const Journal& journal, const Archive& archive, const History& history)
        : pending(journal), m_baseArchive(archive), m_baseHistory(history) {}

    [[nodiscard]] bool archiveTouched() const { return m_archive.has_value(); }
    [[nodiscard]] bool historyTouched() const { return m_history.has_value(); }
    [[nodiscard]] const Archive& currentArchive() const { return m_archive ? *m_archive : m_baseArchive; }
    [[nodiscard]] const History& currentHistory() const { return m_history ? *m_history : m_baseHistory; }
    Archive& archive() {
      if (!m_archive)
        m_archive = m_baseArchive;
      return *m_archive;
    }
    History& history() {
      if (!m_history)
        m_history = m_baseHistory;
      return *m_history;
    }
    Archive takeArchive() { return std::move(*m_archive); }
    History takeHistory() { return std::move(*m_history); }

  private:
    const Archive& m_baseArchive;
    const History& m_baseHistory;
    std::optional<Archive> m_archive;
    std::optional<History> m_history;
  };

  Json encodePending(const Pending& p) {
    return {
        {"base_etag", p.baseEtag},
        {"desired", p.desired},
        {"removed", p.removed},
        {"conflict", p.conflict},
        {"sent", p.sent},
        {"sent_delete", p.sentDelete},
        {"delete_attempted", p.deleteAttempted},
        {"create_attempted", p.createAttempted}
    };
  }
  // The attempt flags were added after version 1; absent keys are inferred from what was sent.
  Pending decodePending(const Json& p) {
    return {
        p.at("base_etag").get<std::string>(),
        p.at("desired").get<std::string>(),
        p.at("removed").get<bool>(),
        p.at("conflict").get<bool>(),
        p.at("sent").get<std::string>(),
        p.at("sent_delete").get<bool>(),
        p.value("delete_attempted", p.at("sent_delete").get<bool>()),
        p.value(
            "create_attempted", p.at("base_etag").get<std::string>().empty() && !p.at("sent").get<std::string>().empty()
        )
    };
  }
  bool validPending(const Pending& p) {
    return (p.baseEtag.empty() || tasks::strongEtag(p.baseEtag))
        && tasks::parseTask(p.desired)
        && (p.sent.empty() || tasks::parseTask(p.sent));
  }
  Json encodeArchived(const std::string& id, const Archived& a) {
    Json entry{{"id", id},           {"task_id", a.taskId},       {"data", a.data},
               {"source", a.source}, {"deleted_at", a.deletedAt}, {"queued", a.queued}};
    entry["prior"] = a.prior ? encodePending(*a.prior) : Json(nullptr);
    return entry;
  }
  Json encodeHistoryEntry(const TaskHistoryEntry& e) {
    return {{"id", e.id},         {"task_id", e.taskId},  {"title", e.title},           {"action", e.action},
            {"source", e.source}, {"details", e.details}, {"occurred_at", e.occurredAt}};
  }
  Json encodeState(
      const std::string& owner, const tasks::Resources& resources, const Journal& pending, const Archive& archived,
      const History& events
  ) {
    Json data{{"version", StateVersion},  {"owner", owner},         {"resources", Json::array()},
              {"journal", Json::array()}, {"trash", Json::array()}, {"history", Json::array()}};
    for (const auto& [id, r] : resources)
      data["resources"].push_back({{"id", id}, {"etag", r.etag}, {"data", r.data}});
    for (const auto& [id, p] : pending) {
      auto entry = encodePending(p);
      entry["id"] = id;
      data["journal"].push_back(std::move(entry));
    }
    for (const auto& [id, a] : archived)
      data["trash"].push_back(encodeArchived(id, *a));
    for (const auto& e : events)
      data["history"].push_back(encodeHistoryEntry(e));
    return data;
  }

  void record(
      History& history, const std::string& id, const std::string& data, std::string action, std::string source,
      std::string details
  ) {
    const auto item = tasks::parseTask(data);
    history.insert(
        history.begin(),
        {tasks::randomId(), id, item ? item->title : "", std::move(action), std::move(source), std::move(details),
         nowSeconds()}
    );
    if (history.size() > TaskHistoryLimit)
      history.resize(TaskHistoryLimit);
  }
  void archiveTask(
      Archive& archive, const std::string& id, const std::string& data, const std::string& source, bool queued = false,
      std::optional<Pending> prior = {}, std::int64_t deletedAt = nowSeconds()
  ) {
    auto value = std::make_shared<Archived>();
    value->taskId = id;
    value->data = data;
    value->source = source;
    value->item = *tasks::parseTask(data);
    value->item.id = id;
    value->deletedAt = deletedAt;
    value->queued = queued;
    value->prior = std::move(prior);
    archive.emplace(tasks::randomId(), std::move(value));
  }
  // History line naming the fields that differ.
  std::string changes(const TaskItem& before, const TaskItem& after) {
    std::string result;
    auto add = [&](const char* field) {
      if (!result.empty())
        result += ", ";
      result += field;
    };
    if (before.title != after.title)
      add("title");
    if (before.description != after.description)
      add("notes");
    if (before.dueDate != after.dueDate)
      add("due date");
    if (before.completed != after.completed)
      add(after.completed ? "completed" : "reopened");
    return result.empty() ? "Calendar metadata changed." : "Changed " + result + ".";
  }
  const char* completionAction(const TaskItem& before, const TaskItem& after) {
    if (before.completed == after.completed)
      return "Edited";
    return after.completed ? "Completed" : "Reopened";
  }
  // Open tasks first, then dated before undated, by date, then title.
  bool taskPrecedes(const TaskItem& a, const TaskItem& b) {
    if (a.completed != b.completed)
      return !a.completed;
    if (a.dueDate.empty() != b.dueDate.empty())
      return !a.dueDate.empty();
    if (a.dueDate != b.dueDate)
      return a.dueDate < b.dueDate;
    return a.title < b.title;
  }
  std::optional<TaskItem> itemFor(const std::string& id, const std::string& data) {
    auto item = tasks::parseTask(data);
    if (item)
      item->id = id;
    return item;
  }
} // namespace

class TaskService::Impl {
public:
  ConfigService& config;
  HttpClient& http;
  security::SecretStore& secrets;
  security::StorageKeyProvider& keys;
  TasksConfig settings;
  TaskSnapshot view;
  ViewDirty dirty;

  // Local mirror of the server and the queued local changes.
  tasks::Resources remote;
  Journal journal;
  Archive archive;
  History history;

  std::string collection, owner;
  std::set<std::string> knownPendingAccounts;
  std::filesystem::path path;
  security::SecureBuffer password;
  security::SecretStoreOperation credentialOperation;
  std::shared_ptr<int> life = std::make_shared<int>(0);
  std::function<void()> changed;
  // Bumped on every account change; callbacks from an older generation are dropped.
  std::uint64_t generation = 0;
  bool initialized = false, loaded = false, storageWritable = false;
  // busy: one HTTP request in flight. fresh: `remote` was read since the last write. syncNow: skip the interval wait.
  bool busy = false, credentialBusy = false, syncNow = false, fresh = false;
  Clock::time_point nextSyncAt = Clock::time_point::max();

  Impl(ConfigService& c, HttpClient& h, security::SecretStore& s, security::StorageKeyProvider& k)
      : config(c), http(h), secrets(s), keys(k) {}
  ~Impl() {
    life.reset();
    credentialOperation.cancel();
  }

  // ---- Lifecycle and scheduling -------------------------------------------------------------------------------

  void initialize() {
    if (initialized)
      return;
    const std::weak_ptr<int> alive = life;
    config.addReloadCallback(
        [this, alive] {
          if (!alive.expired())
            configure();
        },
        "tasks"
    );
    configure();
  }
  // Runs from the poll loop whenever the poll timeout elapses.
  void tick() {
    configure();
    if (syncSuspended() || (!syncNow && Clock::now() < nextSyncAt))
      return;
    scheduleNextSync();
    if (!loaded || !storageWritable)
      syncPersistence();
    if (!loaded || !storageWritable)
      return;
    if (password.empty()) {
      authenticate();
      return;
    }
    refresh();
  }
  [[nodiscard]] int pollTimeoutMs() const {
    if (syncSuspended())
      return -1;
    if (syncNow)
      return 0;
    if (nextSyncAt == Clock::time_point::max())
      return -1;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(nextSyncAt - Clock::now()).count();
    return static_cast<int>(std::clamp<std::int64_t>(ms, 0, std::numeric_limits<int>::max()));
  }
  void requestRefresh() {
    configure();
    if (settings.enabled && !owner.empty())
      syncNow = true;
  }
  bool syncSuspended() const { return !settings.enabled || owner.empty() || busy || credentialBusy; }
  bool current(std::uint64_t gen) const { return gen == generation && settings == config.config().tasks; }
  void scheduleNextSync() {
    syncNow = false;
    nextSyncAt = Clock::now()
        + std::chrono::seconds(
                     std::clamp(settings.syncIntervalSeconds, MinSyncIntervalSeconds, MaxSyncIntervalSeconds)
        );
  }
  // Wraps an async callback so it runs only while this account configuration is still current.
  template <typename Fn> auto guarded(Fn fn) {
    return [this, alive = std::weak_ptr<int>(life), gen = generation, fn = std::move(fn)](auto&&... args) {
      if (alive.expired() || !current(gen))
        return;
      fn(std::forward<decltype(args)>(args)...);
    };
  }

  // Rebuilds all per-account state from configuration. A no-op when the tasks configuration is unchanged.
  void configure() {
    const auto incoming = config.config().tasks;
    if (initialized && incoming == settings)
      return;
    rememberPending();
    settings = incoming;
    ++generation;
    credentialOperation.cancel();
    password.clear();
    credentialBusy = busy = fresh = loaded = storageWritable = false;
    clearRecords();
    owner.clear();
    collection.clear();
    path.clear();
    view.needsPassword = false;
    initialized = true;
    syncNow = false;
    nextSyncAt = Clock::time_point::max();
    auto url = tasks::collectionUrl(settings.collectionUrl);
    if (url && !settings.username.empty()) {
      collection = *url;
      owner = tasks::accountId(collection, settings.username);
      const auto directory = stateDirectory();
      if (!directory.empty())
        path = directory / (owner + ".enc");
      syncPersistence();
    }
    if (!settings.enabled) {
      syncNow = false;
      publish("Tasks are disabled in configuration.");
      return;
    }
    if (owner.empty()) {
      publish("Configure tasks.collection_url and tasks.username to connect Tasks.org.");
      return;
    }
    syncNow = true;
    publish(loaded ? "Connecting to tasks collection…" : "Unlock encrypted storage to load tasks.");
  }
  void clearRecords() {
    remote.clear();
    journal.clear();
    archive.clear();
    history.clear();
    dirty.all();
  }
  // Remembers accounts that hold local records, so storage recovery refuses to erase them.
  void rememberPending() {
    if (owner.empty() || !loaded)
      return;
    if (journal.empty() && archive.empty() && history.empty())
      knownPendingAccounts.erase(owner);
    else
      knownPendingAccounts.insert(owner);
  }

  // ---- Published snapshot -------------------------------------------------------------------------------------

  // Hot path: runs on every state or status change (several times per sync). Parsing and sorting are skipped unless the
  // underlying records changed, so a status-only publish costs a scan of the Trash list.
  void publish(std::string status = {}) {
    if (!status.empty())
      view.status = std::move(status);
    view.syncing = busy || credentialBusy;
    view.writable = settings.enabled
        && !collection.empty()
        && loaded
        && storageWritable
        && keys.state() == security::StorageKeyState::Ready;
    if (settings.enabled) {
      rebuildTrash();
      rebuildHistory();
      rebuildItems();
    } else {
      view.items.clear();
      view.trash.clear();
      view.history.clear();
      dirty.all();
    }
    ++view.revision;
    if (changed)
      changed();
  }
  void rebuildTrash() {
    if (dirty.trash) {
      view.trash.clear();
      for (const auto& [id, a] : archive)
        view.trash.push_back({id, a->item, a->deletedAt, a->source});
      std::stable_sort(view.trash.begin(), view.trash.end(), [](const auto& a, const auto& b) {
        return a.deletedAt > b.deletedAt;
      });
      dirty.trash = false;
    }
    // Journal-dependent flags change independently of the archive, so they are refreshed every time.
    for (auto& t : view.trash) {
      const auto& a = *archive.at(t.id);
      const auto p = journal.find(a.taskId);
      t.pending = p != journal.end();
      t.conflict = t.pending && p->second.conflict;
      t.restorable = !t.pending ? !a.queued : a.queued && p->second.isUnsentDeletion() && !busy;
    }
  }
  void rebuildHistory() {
    if (!dirty.history)
      return;
    view.history = history;
    dirty.history = false;
  }
  // Synced tasks plus journal entries (which shadow their server copy); queued deletions are hidden.
  void rebuildItems() {
    if (!dirty.items)
      return;
    view.items.clear();
    for (const auto& [id, resource] : remote) {
      if (!journal.contains(id))
        if (auto item = itemFor(id, resource.data))
          view.items.push_back(std::move(*item));
    }
    for (const auto& [id, pending] : journal) {
      if (pending.removed && !pending.conflict)
        continue;
      auto item = itemFor(id, pending.desired);
      if (!item)
        continue;
      item->pending = true;
      item->conflict = pending.conflict;
      view.items.push_back(std::move(*item));
    }
    std::stable_sort(view.items.begin(), view.items.end(), taskPrecedes);
    dirty.items = false;
  }

  // ---- Encrypted persistence ----------------------------------------------------------------------------------

  security::EncryptionContext context() const { return {StateContextLabel, owner}; }
  security::SecretId secretId() const { return {"tasks", owner, "password"}; }

  // Writes the complete state; on failure nothing is accepted and the UI is told why.
  bool
  persist(const tasks::Resources& resources, const Journal& pending, const Archive& archived, const History& events) {
    auto key = keys.deriveKey(security::StorageKeyPurpose::Tasks);
    if (!key || path.empty()) {
      storageWritable = false;
      publish("Unlock encrypted storage before changing tasks; local changes have not been accepted.");
      return false;
    }
    try {
      auto serialized = encodeState(owner, resources, pending, archived, events).dump();
      const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(serialized.data()), serialized.size());
      const bool ok = serialized.size() <= MaxStateBytes && security::writeEncryptedFile(path, bytes, *key, context());
      sodium_memzero(serialized.data(), serialized.size());
      if (ok) {
        storageWritable = true;
        keys.noteEncryptedDataExists();
        return true;
      }
    } catch (const std::exception&) {
      // Falls through to the same refusal as a failed write: nothing was accepted.
    }
    storageWritable = false;
    publish("Cannot save encrypted task journal. Check storage space and permissions; no new changes were accepted.");
    return false;
  }
  bool persist(const tasks::Resources& resources, const Journal& pending) {
    return persist(resources, pending, archive, history);
  }
  bool isResourceId(const std::string& id) const { return tasks::resourceUrl(collection, id) == std::optional(id); }

  bool decodeResources(const Json& data, tasks::Resources& resources) const {
    for (const auto& r : data.at("resources")) {
      const auto id = r.at("id").get<std::string>();
      tasks::Resource value{r.at("etag").get<std::string>(), r.at("data").get<std::string>()};
      if (!isResourceId(id)
          || !tasks::strongEtag(value.etag)
          || !tasks::parseTask(value.data)
          || !resources.emplace(id, std::move(value)).second)
        return false;
    }
    return true;
  }
  bool decodeJournal(const Json& data, Journal& pending) const {
    for (const auto& p : data.at("journal")) {
      const auto id = p.at("id").get<std::string>();
      Pending value = decodePending(p);
      if (!isResourceId(id) || !validPending(value) || !pending.emplace(id, std::move(value)).second)
        return false;
    }
    return true;
  }
  // Version 1 kept no prior intent and did not record earlier DELETE attempts. Preserve its calendars without
  // inventing a deletion date or allowing an unsafe undo.
  static void upgradeLegacyDeletions(Journal& pending, Archive& archived) {
    for (auto& [id, p] : pending) {
      if (!p.removed)
        continue;
      p.deleteAttempted = true;
      archiveTask(archived, id, p.desired, SourceDesktop, true, {}, 0);
    }
  }
  bool decodeArchive(const Json& data, const Journal& pending, Archive& archived) const {
    for (const auto& a : data.at("trash")) {
      const auto id = a.at("id").get<std::string>();
      auto value = std::make_shared<Archived>();
      value->taskId = a.at("task_id").get<std::string>();
      value->data = a.at("data").get<std::string>();
      value->source = a.at("source").get<std::string>();
      value->deletedAt = a.at("deleted_at").get<std::int64_t>();
      value->queued = a.at("queued").get<bool>();
      if (!a.at("prior").is_null())
        value->prior = decodePending(a.at("prior"));
      const auto item = tasks::parseTask(value->data);
      const auto queuedEntry = pending.find(value->taskId);
      if (id.empty()
          || !item
          || !isResourceId(value->taskId)
          || value->deletedAt < 0
          || (value->source != SourceDesktop && value->source != SourceServer)
          || (value->prior && (!validPending(*value->prior) || value->prior->removed || value->prior->conflict))
          || (value->queued && (queuedEntry == pending.end() || !queuedEntry->second.removed)))
        return false;
      value->item = *item;
      value->item.id = value->taskId;
      if (!archived.emplace(id, std::move(value)).second)
        return false;
    }
    return true;
  }
  bool decodeHistory(const Json& data, History& events) const {
    for (const auto& e : data.at("history")) {
      TaskHistoryEntry value{e.at("id").get<std::string>(),          e.at("task_id").get<std::string>(),
                             e.at("title").get<std::string>(),       e.at("action").get<std::string>(),
                             e.at("source").get<std::string>(),      e.at("details").get<std::string>(),
                             e.at("occurred_at").get<std::int64_t>()};
      if (value.id.empty() || value.occurredAt < 0 || events.size() >= TaskHistoryLimit || !isResourceId(value.taskId))
        return false;
      events.push_back(std::move(value));
    }
    return true;
  }
  // False when the file is malformed, belongs to another account, or fails any validity rule.
  bool decode(
      const std::vector<std::uint8_t>& bytes, tasks::Resources& resources, Journal& pending, Archive& archived,
      History& events
  ) const {
    try {
      const auto data = Json::parse(bytes);
      const auto version = data.at("version").get<int>();
      if ((version != LegacyStateVersion && version != StateVersion) || data.at("owner").get<std::string>() != owner)
        return false;
      if (!decodeResources(data, resources) || !decodeJournal(data, pending))
        return false;
      if (version == LegacyStateVersion) {
        upgradeLegacyDeletions(pending, archived);
        return true;
      }
      return decodeArchive(data, pending, archived) && decodeHistory(data, events);
    } catch (const std::exception&) {
      // Any missing or mistyped field means the file is not a valid state document.
      return false;
    }
  }
  // Reads the state file into memory; a missing file is an empty account. Publishes the reason on failure.
  bool loadFromDisk() {
    auto key = keys.deriveKey(security::StorageKeyPurpose::Tasks);
    if (!key || path.empty()) {
      storageWritable = false;
      publish("No private task storage is available.");
      return false;
    }
    auto result = security::readEncryptedFile(path, *key, context(), MaxStateBytes);
    if (result.status == security::EncryptedReadStatus::NotFound) {
      loaded = true;
      return true;
    }
    if (!result.succeeded()) {
      storageWritable = false;
      publish("Cannot unlock/read encrypted tasks. Check the storage key; cached edits were not replaced.");
      return false;
    }
    tasks::Resources resources;
    Journal pending;
    Archive archived;
    History events;
    const bool ok = decode(result.plaintext, resources, pending, archived, events);
    sodium_memzero(result.plaintext.data(), result.plaintext.size());
    if (!ok) {
      storageWritable = false;
      publish("Encrypted task journal is invalid. Preserve it before using storage recovery.");
      return false;
    }
    remote = std::move(resources);
    journal = std::move(pending);
    archive = std::move(archived);
    history = std::move(events);
    dirty.all();
    loaded = true;
    rememberPending();
    return true;
  }
  // Loads (once) and re-saves the state so storage writability is known; requests a sync when it is.
  void syncPersistence() {
    if (owner.empty())
      return;
    if (keys.state() != security::StorageKeyState::Ready) {
      storageWritable = false;
      publish("Encrypted task storage is locked or unavailable. Unlock storage to edit or sync queued changes.");
      return;
    }
    if (!loaded && !loadFromDisk())
      return;
    if (persist(remote, journal)) {
      publish(journal.empty() ? "Tasks ready." : "Saved local changes are waiting to sync.");
      syncNow = true;
    }
  }

  // ---- Credentials --------------------------------------------------------------------------------------------

  void authenticate() {
    if (credentialBusy)
      return;
    if (!settings.passwordFile.empty()) {
      auto result = calendar::readCredentialFile(settings.passwordFile);
      if (result.status == calendar::CredentialFileStatus::Success && !result.value.empty()) {
        password = std::move(result.value);
        view.needsPassword = false;
        syncNow = true;
      } else {
        scheduleNextSync();
        publish("Cannot read task password_file. Use a private, user-owned credential file.");
      }
      return;
    }
    credentialBusy = true;
    publish("Looking up task credentials…");
    credentialOperation =
        secrets.lookup(secretId(), guarded([this](security::SecretStoreStatus status, security::SecureBuffer value) {
                         credentialBusy = false;
                         if (status == security::SecretStoreStatus::Success && !value.empty()) {
                           password = std::move(value);
                           view.needsPassword = false;
                           syncNow = true;
                           publish("Task credentials ready.");
                           return;
                         }
                         view.needsPassword = true;
                         scheduleNextSync();
                         publish(
                             status == security::SecretStoreStatus::NotFound
                                 ? "Enter the tasks account password."
                                 : "Unlock Secret Service, then enter the tasks password or configure password_file."
                         );
                       }));
  }
  void savePassword(std::string value) {
    configure();
    if (owner.empty() || !settings.passwordFile.empty() || value.empty()) {
      sodium_memzero(value.data(), value.size());
      publish("Configure the tasks account first; password_file credentials must be updated in that file.");
      return;
    }
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
    security::SecureBuffer secret(bytes);
    sodium_memzero(value.data(), value.size());
    credentialOperation.cancel();
    credentialBusy = true;
    publish("Saving task password in Secret Service…");
    credentialOperation = secrets.store(
        secretId(), std::move(secret), "TerminusBar Tasks account password",
        guarded([this](security::SecretStoreStatus status) {
          credentialBusy = false;
          password.clear();
          if (status == security::SecretStoreStatus::Success) {
            view.needsPassword = false;
            syncNow = true;
            publish("Task password saved securely.");
            return;
          }
          view.needsPassword = true;
          scheduleNextSync();
          publish("Task password was not saved. Unlock Secret Service and try again; no plaintext fallback is used.");
        })
    );
  }

  // ---- HTTP ---------------------------------------------------------------------------------------------------

  HttpRequest authorizedRequest(std::string method, const std::string& url) const {
    HttpRequest req;
    req.method = std::move(method);
    req.url = url;
    req.basicUsername = settings.username;
    const auto bytes = password.bytes();
    req.basicPassword.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return req;
  }
  // Transport error or unexpected status: keep every queued change and retry at the next interval.
  void onHttpFailure(const HttpResponse& response) {
    busy = false;
    fresh = false;
    scheduleNextSync();
    if (response.transportOk && (response.status == HttpUnauthorized || response.status == HttpForbidden)) {
      password.clear();
      view.needsPassword = settings.passwordFile.empty();
      publish(
          "Tasks authentication or permission denied. Check the password and collection write access; saved changes "
          "are retained."
      );
    } else {
      publish(
          response.transportOk
              ? "Task server returned HTTP " + std::to_string(response.status) + ". Saved changes will retry."
              : "Task server is unreachable. Encrypted offline changes are retained; retrying at the sync interval."
      );
    }
  }

  // ---- Refresh: REPORT, then reconcile the server state with the journal --------------------------------------

  void refresh() {
    busy = true;
    syncNow = false;
    fresh = false;
    // Due dates are shown as local days, so each refresh rebuilds the items once: a time zone change shows up
    // within one sync interval even when the server has nothing new.
    dirty.items = true;
    publish("Refreshing tasks…");
    auto req = authorizedRequest("REPORT", collection);
    req.headers = {"Depth: 1", "Content-Type: application/xml; charset=utf-8"};
    req.body = TaskQuery;
    http.request(std::move(req), guarded([this](HttpResponse response) { onReport(response); }));
  }
  void onReport(const HttpResponse& response) {
    if (!response.transportOk || response.status != HttpMultiStatus) {
      onHttpFailure(response);
      return;
    }
    tasks::Resources resources;
    if (!tasks::parseReport(response.body, collection, resources)) {
      busy = false;
      scheduleNextSync();
      publish("Invalid or unsafe CalDAV task response. Cached tasks and queued changes are unchanged.");
      return;
    }
    Reconciliation rec(journal, archive, history);
    observeServerChanges(rec, resources);
    for (auto it = rec.pending.begin(); it != rec.pending.end();) {
      if (reconcileEntry(rec, it->first, it->second, resources))
        ++it;
      else
        it = rec.pending.erase(it);
    }
    confirmQueuedDeletions(rec);
    busy = false;
    adoptReconciliation(std::move(resources), rec);
  }
  // True when the server copy is what we sent or intended, not a foreign change.
  bool isEchoOfLocalChange(const std::string& id, const std::string& serverData) const {
    const auto p = journal.find(id);
    return p != journal.end()
        && (p->second.sentMatches(serverData)
            || ((!p->second.removed || p->second.createAttempted) && tasks::equivalent(serverData, p->second.desired)));
  }
  // Records what changed on the server since the last REPORT: new, edited and vanished tasks.
  void observeServerChanges(Reconciliation& rec, const tasks::Resources& resources) {
    rec.resourcesChanged = resources.size() != remote.size();
    for (const auto& [id, r] : resources) {
      const auto before = remote.find(id);
      if (before == remote.end()) {
        rec.resourcesChanged = true;
        if (!isEchoOfLocalChange(id, r.data))
          record(
              rec.history(), id, r.data, "Discovered", SourceServer, "First observed in this account's local history."
          );
        continue;
      }
      if (before->second.etag == r.etag && before->second.data == r.data)
        continue;
      rec.resourcesChanged = true;
      if (isEchoOfLocalChange(id, r.data) || tasks::equivalent(before->second.data, r.data))
        continue;
      const auto oldItem = *tasks::parseTask(before->second.data);
      const auto newItem = *tasks::parseTask(r.data);
      record(rec.history(), id, r.data, completionAction(oldItem, newItem), SourceServer, changes(oldItem, newItem));
    }
    for (const auto& [id, r] : remote) {
      if (resources.contains(id))
        continue;
      rec.resourcesChanged = true;
      const auto p = journal.find(id);
      if (p != journal.end() && p->second.removed)
        continue;
      archiveTask(rec.archive(), id, r.data, SourceServer);
      record(
          rec.history(), id, r.data, "Deleted", SourceServer,
          "Server task disappeared; its last observed calendar is in local Trash."
      );
    }
  }
  // Decides one journal entry against the fresh server state. Returns false when the entry is settled and must be
  // dropped; otherwise it stays queued (possibly flagged as a conflict).
  bool reconcileEntry(Reconciliation& rec, const std::string& id, Pending& p, const tasks::Resources& resources) {
    const auto found = resources.find(id);
    const bool exists = found != resources.end();
    if (p.sentDelete && !exists)
      return false;

    const bool sentArrived = exists
        && (p.sentMatches(found->second.data)
            || (p.createAttempted && p.removed && tasks::equivalent(found->second.data, p.desired)));
    if (sentArrived) {
      if (!p.removed && tasks::equivalent(p.desired, p.sent))
        return false;
      p.baseEtag = found->second.etag;
      p.sent.clear();
      p.sentDelete = false;
      p.conflict = false;
      p.createAttempted = false;
    }
    if (p.removed && !exists) {
      if (!p.createAttempted)
        return false;
      if (!p.conflict)
        record(
            rec.history(), id, p.desired, "Conflict", SourceServer,
            "An interrupted creation is still uncertain; deletion and its recovery copy were retained."
        );
      p.conflict = true;
      return true;
    }
    if (!p.removed && exists && tasks::equivalent(found->second.data, p.desired))
      return false;
    // Never replay a rejected precondition, even if a broken server repeats its ETag.
    if (p.conflict)
      return true;

    const bool serverUnchanged = p.baseEtag.empty() ? !exists : exists && p.baseEtag == found->second.etag;
    if (!serverUnchanged) {
      p.conflict = true;
      record(
          rec.history(), id, p.desired, "Conflict", SourceServer, "Server state changed; the local draft was retained."
      );
      return true;
    }
    // A request of ours that left no trace is re-sent next cycle, not immediately.
    rec.deferRetry = rec.deferRetry || !p.sent.empty() || p.sentDelete;
    p.sent.clear();
    p.sentDelete = false;
    return true;
  }
  // A queued deletion whose journal entry is gone has been confirmed: Trash keeps the copy but forgets the queue.
  void confirmQueuedDeletions(Reconciliation& rec) {
    for (const auto& [id, a] : archive) {
      if (!a->queued || rec.pending.contains(a->taskId))
        continue;
      auto confirmed = std::make_shared<Archived>(*a);
      confirmed->queued = false;
      confirmed->prior.reset();
      rec.archive().at(id) = std::move(confirmed);
    }
  }
  // Persists the reconciled state and only then swaps it in; a failed write leaves the old state untouched.
  void adoptReconciliation(tasks::Resources resources, Reconciliation& rec) {
    const bool stateChanged =
        rec.resourcesChanged || rec.pending != journal || rec.archiveTouched() || rec.historyTouched();
    if (!loaded || (stateChanged && !persist(resources, rec.pending, rec.currentArchive(), rec.currentHistory()))) {
      scheduleNextSync();
      return;
    }
    remote = std::move(resources);
    journal = std::move(rec.pending);
    if (stateChanged)
      dirty.items = true;
    if (rec.archiveTouched()) {
      archive = rec.takeArchive();
      dirty.trash = true;
    }
    if (rec.historyTouched()) {
      history = rec.takeHistory();
      dirty.history = true;
    }
    rememberPending();
    fresh = true;
    const bool hasConflicts =
        std::any_of(journal.begin(), journal.end(), [](const auto& p) { return p.second.conflict; });
    publish(
        hasConflicts
            ? "Conflict: phone/server changes were preserved. Edit to apply only your newly changed fields, or "
              "discard local changes. Deleted remote tasks must be added as new."
            : journal.empty() ? "Tasks synced."
                              : "Sending saved task changes…"
    );
    if (rec.deferRetry)
      scheduleNextSync();
    else
      replay();
  }

  // ---- Replay: send one queued change, then read the result back ----------------------------------------------

  void replay() {
    if (busy || !fresh || !view.writable) {
      scheduleNextSync();
      return;
    }
    const auto next = std::find_if(journal.begin(), journal.end(), [](const auto& p) { return !p.second.conflict; });
    if (next == journal.end()) {
      scheduleNextSync();
      return;
    }
    const auto id = next->first;
    if (!isResourceId(id)) {
      fresh = false;
      scheduleNextSync();
      publish("Refusing an unsafe task resource URL. No credentials or changes were sent.");
      return;
    }
    if (!persistSendIntent(id)) {
      scheduleNextSync();
      return;
    }
    const auto& sent = journal.at(id);
    auto req = authorizedRequest(sent.removed ? "DELETE" : "PUT", id);
    req.headers.push_back(sent.baseEtag.empty() ? "If-None-Match: *" : "If-Match: " + sent.baseEtag);
    if (!sent.removed) {
      req.headers.push_back("Content-Type: text/calendar; charset=utf-8");
      req.body = sent.sent;
    }
    busy = true;
    fresh = false;
    syncNow = false;
    publish("Syncing saved task changes…");
    http.request(std::move(req), guarded([this, id](HttpResponse response) { onReplayResponse(id, response); }));
  }
  // Records on disk what is about to be sent, so a crash or timeout leaves evidence for the next REPORT.
  bool persistSendIntent(const std::string& id) {
    auto pending = journal;
    auto& p = pending.at(id);
    p.sentDelete = p.removed;
    p.deleteAttempted = p.deleteAttempted || p.removed;
    p.createAttempted = p.createAttempted || (!p.removed && p.baseEtag.empty());
    p.sent = p.removed ? std::string{} : p.desired;
    if (!persist(remote, pending))
      return false;
    journal = std::move(pending);
    return true;
  }
  void onReplayResponse(const std::string& id, const HttpResponse& response) {
    busy = false;
    if (response.transportOk && response.status == HttpPreconditionFailed) {
      markConflictAfterRejectedWrite(id);
      return;
    }
    const bool accepted = response.transportOk
        && ((response.status >= 200 && response.status < 300)
            || response.status == HttpNotFound
            || response.status == HttpGone);
    if (!accepted) {
      onHttpFailure(response);
      return;
    }
    // HttpClient does not expose ETag headers. Read back the authoritative resource before acknowledging the journal.
    syncNow = true;
    publish("Task change sent; confirming server state…");
  }
  void markConflictAfterRejectedWrite(const std::string& id) {
    auto conflicted = journal;
    auto events = history;
    if (auto found = conflicted.find(id); found != conflicted.end() && !found->second.conflict) {
      found->second.conflict = true;
      record(
          events, id, found->second.desired, "Conflict", SourceServer,
          "Conditional write rejected; the local draft was retained."
      );
    }
    if (!persist(remote, conflicted, archive, events)) {
      scheduleNextSync();
      return;
    }
    journal = std::move(conflicted);
    history = std::move(events);
    dirty.items = dirty.history = true;
    rememberPending();
    syncNow = true;
    publish("Task changed on the server; refreshing before resolving the conflict.");
  }

  // ---- Local edits: stage a copy of the records, persist it, then adopt it ------------------------------------

  Records stagedRecords() const { return {journal, archive, history}; }
  bool readyToEdit() {
    configure();
    if (!view.writable) {
      publish(
          "Task edits require enabled configuration and writable encrypted storage. Unlock storage or fix its "
          "permissions, then retry."
      );
      return false;
    }
    return true;
  }
  bool commit(Records staged, const std::string& status = "Task changes saved locally; waiting to sync.") {
    if (!persist(remote, staged.journal, staged.archive, staged.history))
      return false;
    journal = std::move(staged.journal);
    archive = std::move(staged.archive);
    history = std::move(staged.history);
    dirty.all();
    rememberPending();
    fresh = false;
    syncNow = true;
    publish(status);
    return true;
  }
  static void dropQueuedTrash(Archive& archived, const std::string& id) {
    std::erase_if(archived, [&](const auto& a) { return a.second->taskId == id && a.second->queued; });
  }
  bool publishInvalidDraft() {
    publish("Enter a task title and a valid due date (YYYY-MM-DD), or leave the date empty.");
    return false;
  }

  bool addTask(const TaskDraft& draft) {
    if (!readyToEdit())
      return false;
    if (!tasks::validDraft(draft))
      return publishInvalidDraft();
    const auto uid = tasks::randomId();
    const auto id = tasks::resourceUrl(collection, uid + ".ics");
    if (!id) {
      publish("Invalid collection URL; cannot create tasks safely.");
      return false;
    }
    auto staged = stagedRecords();
    const auto data = tasks::newTask(draft, uid);
    if (data.empty() || remote.contains(*id) || !staged.journal.emplace(*id, Pending{{}, data}).second) {
      publish("Could not allocate a new task identity. No change was saved; please retry.");
      return false;
    }
    record(staged.history, *id, data, "Created", SourceDesktop, "New task saved locally.");
    return commit(std::move(staged));
  }

  // Picks the calendar to edit and makes sure `pending` has an entry for it: the queued intent, or the server copy when
  // there is none or a conflict is being resolved. Publishes the reason and returns false when editing is not possible.
  bool stageForEdit(Journal& pending, const std::string& id, std::string& source) {
    const auto server = remote.find(id);
    const auto queued = pending.find(id);
    if (queued == pending.end()) {
      if (server == remote.end()) {
        publish("Task disappeared. Refresh and try again.");
        return false;
      }
      source = server->second.data;
      pending.emplace(id, Pending{server->second.etag, source});
      return true;
    }
    if (queued->second.removed && !queued->second.conflict) {
      publish("This task is awaiting deletion. Add a new task instead.");
      return false;
    }
    if (!queued->second.conflict) {
      source = queued->second.desired;
      return true;
    }
    if (server == remote.end()) {
      publish("This task was deleted remotely. Copy its text into a new task, or discard the local changes.");
      return false;
    }
    if (busy) {
      publish("Wait for the refresh to finish before resolving this conflict.");
      return false;
    }
    source = server->second.data;
    queued->second = Pending{server->second.etag, source};
    return true;
  }
  // Edits a task's fields and/or completion. Only fields that differ from the displayed task are touched.
  bool edit(const std::string& id, const TaskDraft* draft, std::optional<bool> completed) {
    if (!readyToEdit())
      return false;
    if (draft && !tasks::validDraft(*draft))
      return publishInvalidDraft();
    const auto displayed =
        std::find_if(view.items.begin(), view.items.end(), [&](const TaskItem& t) { return t.id == id; });
    if (displayed == view.items.end()) {
      publish("This task is no longer available. Refresh the list.");
      return false;
    }
    const bool draftChanged = draft
        && (draft->title != displayed->title
            || draft->description != displayed->description
            || draft->dueDate != displayed->dueDate);
    const bool completionChanged = completed && *completed != displayed->completed;
    if (!draftChanged && !completionChanged) {
      publish("No task fields changed.");
      return true;
    }
    auto staged = stagedRecords();
    std::string source;
    if (!stageForEdit(staged.journal, id, source))
      return false;
    const auto edited = tasks::editTask(source, *displayed, draft, completed);
    if (edited.empty()) {
      publish("This VTODO could not be edited safely. Its original data was preserved.");
      return false;
    }
    auto& entry = staged.journal.at(id);
    entry.desired = edited;
    entry.removed = false;
    const auto result = *tasks::parseTask(edited);
    record(
        staged.history, id, edited, completionChanged ? (result.completed ? "Completed" : "Reopened") : "Edited",
        SourceDesktop, changes(*displayed, result)
    );
    // Resolving a rejected deletion keeps the server task; it is no longer a queued deletion.
    dropQueuedTrash(staged.archive, id);
    return commit(std::move(staged));
  }

  bool removeTask(const std::string& id) {
    if (!readyToEdit())
      return false;
    auto staged = stagedRecords();
    const auto queued = staged.journal.find(id);
    if (queued != staged.journal.end() && queued->second.conflict) {
      if (busy) {
        publish("Wait for synchronization to finish before discarding a conflict.");
        return false;
      }
      record(
          staged.history, id, queued->second.desired, "Discarded", SourceDesktop,
          "Local conflict draft discarded; no server task was deleted."
      );
      staged.journal.erase(queued);
      dropQueuedTrash(staged.archive, id);
    } else if (queued != staged.journal.end()) {
      if (queued->second.removed) {
        publish("This task is already in local Trash.");
        return true;
      }
      trashQueuedTask(staged, queued);
    } else if (!trashServerTask(staged, id)) {
      return false;
    }
    return commit(std::move(staged));
  }
  // A task with a queued edit or creation. A creation that never reached the network simply disappears.
  static void trashQueuedTask(Records& staged, Journal::iterator queued) {
    const auto& [id, prior] = *queued;
    const bool unsentCreate =
        prior.baseEtag.empty() && prior.sent.empty() && !prior.sentDelete && !prior.createAttempted;
    archiveTask(
        staged.archive, id, prior.desired, SourceDesktop, !unsentCreate,
        unsentCreate ? std::nullopt : std::optional(prior)
    );
    record(
        staged.history, id, prior.desired, "Deleted", SourceDesktop,
        unsentCreate ? "Unsent new task moved to local Trash." : "Moved to local Trash; server deletion queued."
    );
    if (unsentCreate)
      staged.journal.erase(queued);
    else
      queued->second.removed = true;
  }
  bool trashServerTask(Records& staged, const std::string& id) {
    const auto server = remote.find(id);
    if (server == remote.end()) {
      publish("Task is already absent. Refresh the list.");
      return false;
    }
    archiveTask(staged.archive, id, server->second.data, SourceDesktop, true);
    record(
        staged.history, id, server->second.data, "Deleted", SourceDesktop,
        "Moved to local Trash; server deletion queued."
    );
    staged.journal.emplace(id, Pending{server->second.etag, server->second.data, true});
    return true;
  }

  bool restoreTask(const std::string& trashId) {
    if (!readyToEdit())
      return false;
    const auto found = archive.find(trashId);
    if (found == archive.end()) {
      publish("This local Trash entry is no longer available.");
      return false;
    }
    const auto& trashed = *found->second;
    auto staged = stagedRecords();
    std::string restoredId, restoredData, details;
    const bool restored = trashed.queued
        ? undoQueuedDeletion(staged.journal, trashed, restoredId, restoredData, details)
        : restoreAsNewTask(staged.journal, trashed, restoredId, restoredData, details);
    if (!restored)
      return false;
    staged.archive.erase(trashId);
    record(staged.history, restoredId, restoredData, "Restored", SourceDesktop, std::move(details));
    return commit(std::move(staged));
  }
  // The deletion was only queued: put the previous local intent (or nothing) back.
  bool undoQueuedDeletion(
      Journal& pending, const Archived& trashed, std::string& restoredId, std::string& restoredData,
      std::string& details
  ) {
    const auto p = pending.find(trashed.taskId);
    if (p == pending.end() || !p->second.isUnsentDeletion() || busy) {
      publish("Deletion is in flight, uncertain, or conflicting. Refresh and reconcile it before restoring.");
      return false;
    }
    if (trashed.prior) {
      auto prior = *trashed.prior;
      // A preceding PUT may have been confirmed after the delete was queued.
      prior.baseEtag = p->second.baseEtag;
      prior.sent.clear();
      prior.sentDelete = false;
      prior.createAttempted = false;
      p->second = std::move(prior);
    } else {
      pending.erase(p);
    }
    restoredId = trashed.taskId;
    restoredData = trashed.data;
    details = "Unsent deletion undone; previous local task intent restored.";
    return true;
  }
  // The deletion was confirmed: queue the archived calendar as a new task with a fresh identity.
  bool restoreAsNewTask(
      Journal& pending, const Archived& trashed, std::string& restoredId, std::string& restoredData,
      std::string& details
  ) {
    if (pending.contains(trashed.taskId)) {
      publish("The original task still has a pending or conflicting mutation. Reconcile it before restoring.");
      return false;
    }
    const auto uid = tasks::randomId();
    const auto id = tasks::resourceUrl(collection, uid + ".ics");
    restoredData = tasks::restoreTask(trashed.data, uid);
    if (!id
        || restoredData.empty()
        || remote.contains(*id)
        || !pending.emplace(*id, Pending{{}, restoredData}).second) {
      publish("Could not restore this calendar with a new identity. Local Trash was preserved.");
      return false;
    }
    restoredId = *id;
    details = "Restored a copy with a new UID/resource, preserving the complete archived calendar.";
    return true;
  }

  bool purgeTask(const std::string& trashId) {
    if (!readyToEdit())
      return false;
    const auto found = archive.find(trashId);
    if (found == archive.end()) {
      publish("This local Trash entry is no longer available.");
      return false;
    }
    if (found->second->queued || journal.contains(found->second->taskId)) {
      publish("Cannot purge local Trash while the original task has pending or conflicting changes.");
      return false;
    }
    auto events = history;
    record(
        events, found->second->taskId, found->second->data, "Discarded", SourceDesktop,
        "Local Trash entry permanently removed; no server request was made."
    );
    auto archived = archive;
    archived.erase(trashId);
    if (!persist(remote, journal, archived, events))
      return false;
    archive = std::move(archived);
    history = std::move(events);
    dirty.trash = dirty.history = true;
    rememberPending();
    publish("Local Trash entry purged. Server tasks were not changed.");
    return true;
  }
  bool clearHistory() {
    if (!readyToEdit())
      return false;
    if (!history.empty()) {
      if (!persist(remote, journal, archive, {}))
        return false;
      history.clear();
      dirty.history = true;
      rememberPending();
    }
    publish("Local task history cleared. Tasks and Trash were not changed.");
    return true;
  }

  // ---- Storage recovery ---------------------------------------------------------------------------------------

  // Lists the state files that may be deleted. Any file that could hold unsynced work, or that cannot be proven empty,
  // refuses the whole recovery (published); nullopt also covers directory read errors.
  std::optional<std::vector<std::filesystem::path>> recoverableStateFiles(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> paths;
    std::error_code ec;
    auto key = keys.deriveKey(security::StorageKeyPurpose::Tasks);
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
      if (it->path().extension() != ".enc")
        continue;
      // Other account outboxes, Trash and history must never be silently erased by recovery.
      if (it->path() != path) {
        publish(
            "Task recovery refused: encrypted data for another account exists. Preserve and resolve that account's "
            "journal, Trash and history first."
        );
        return std::nullopt;
      }
      if (!key) {
        publish("Task recovery refused: unlock and preserve encrypted task records before resetting storage.");
        return std::nullopt;
      }
      auto read = security::readEncryptedFile(it->path(), *key, context(), MaxStateBytes);
      if (!read.succeeded()) {
        publish("Task recovery refused: unreadable task records may contain local Trash, history or pending changes.");
        return std::nullopt;
      }
      tasks::Resources resources;
      Journal pending;
      Archive archived;
      History events;
      const bool ok = decode(read.plaintext, resources, pending, archived, events);
      sodium_memzero(read.plaintext.data(), read.plaintext.size());
      if (!ok || !pending.empty() || !archived.empty() || !events.empty()) {
        publish("Task recovery refused: preserve local changes, Trash and history before recovery.");
        return std::nullopt;
      }
      paths.push_back(it->path());
    }
    if (ec)
      return std::nullopt;
    return paths;
  }
  bool clearEncryptedPersistenceForRecovery() {
    rememberPending();
    if (!knownPendingAccounts.empty() || !journal.empty() || !archive.empty() || !history.empty() || busy) {
      publish(
          "Task recovery refused: local changes, Trash, history, or a sync request still exist. Preserve or explicitly "
          "clear local records and reconcile changes before resetting storage."
      );
      return false;
    }
    const auto directory = stateDirectory();
    if (directory.empty())
      return true;
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec))
      return !ec;
    const auto paths = recoverableStateFiles(directory);
    if (!paths)
      return false;
    for (const auto& file : *paths)
      if (!std::filesystem::remove(file, ec) || ec)
        return false;
    ++generation;
    credentialOperation.cancel();
    credentialBusy = false;
    clearRecords();
    loaded = storageWritable = fresh = false;
    publish("Encrypted task cache cleared for explicitly requested key recovery.");
    return true;
  }
};

// ---- Public facade --------------------------------------------------------------------------------------------

TaskService::TaskService(ConfigService& c, HttpClient& h, security::SecretStore& s, security::StorageKeyProvider& k)
    : m(std::make_unique<Impl>(c, h, s, k)) {}
TaskService::~TaskService() = default;
void TaskService::initialize() { m->initialize(); }
void TaskService::tick() { m->tick(); }
int TaskService::pollTimeoutMs() const { return m->pollTimeoutMs(); }
const TaskSnapshot& TaskService::snapshot() const { return m->view; }
void TaskService::setChangeCallback(std::function<void()> callback) { m->changed = std::move(callback); }
void TaskService::requestRefresh() { m->requestRefresh(); }
bool TaskService::addTask(const TaskDraft& draft) { return m->addTask(draft); }
bool TaskService::updateTask(const std::string& id, const TaskDraft& draft) {
  return m->edit(id, &draft, std::nullopt);
}
bool TaskService::setCompleted(const std::string& id, bool completed) { return m->edit(id, nullptr, completed); }
bool TaskService::removeTask(const std::string& id) { return m->removeTask(id); }
bool TaskService::restoreTask(const std::string& trashId) { return m->restoreTask(trashId); }
bool TaskService::purgeTask(const std::string& trashId) { return m->purgeTask(trashId); }
bool TaskService::clearHistory() { return m->clearHistory(); }
void TaskService::savePassword(std::string value) { m->savePassword(std::move(value)); }
void TaskService::syncPersistence() { m->syncPersistence(); }
bool TaskService::hasEncryptedPersistence() const {
  const auto directory = stateDirectory();
  if (directory.empty())
    return false;
  // A directory that cannot be inspected is assumed to hold data, so the storage key is kept.
  std::error_code ec;
  if (!std::filesystem::exists(directory, ec))
    return bool(ec);
  for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() == ".enc")
      return true;
  }
  return bool(ec);
}
bool TaskService::clearEncryptedPersistenceForRecovery() { return m->clearEncryptedPersistenceForRecovery(); }
