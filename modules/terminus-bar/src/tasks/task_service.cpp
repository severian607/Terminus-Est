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
  constexpr const char* Query = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
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
  std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
  }
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
    if (history.size() > 500)
      history.resize(500);
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
} // namespace

class TaskService::Impl {
public:
  ConfigService& config;
  HttpClient& http;
  security::SecretStore& secrets;
  security::StorageKeyProvider& keys;
  TasksConfig settings;
  TaskSnapshot view;
  tasks::Resources remote;
  Journal journal;
  Archive archive;
  History history;
  bool archiveViewDirty = true, historyViewDirty = true;
  std::string collection, owner;
  std::set<std::string> knownPendingAccounts;
  std::filesystem::path path;
  security::SecureBuffer password;
  security::SecretStoreOperation credentialOperation;
  std::shared_ptr<int> life = std::make_shared<int>(0);
  std::function<void()> changed;
  std::uint64_t generation = 0;
  bool initialized = false, loaded = false, storageWritable = false;
  bool busy = false, credentialBusy = false, immediate = false, fresh = false;
  Clock::time_point next = Clock::time_point::max();

  Impl(ConfigService& c, HttpClient& h, security::SecretStore& s, security::StorageKeyProvider& k)
      : config(c), http(h), secrets(s), keys(k) {}
  ~Impl() {
    life.reset();
    credentialOperation.cancel();
  }

  bool current(std::uint64_t gen) const { return gen == generation && settings == config.config().tasks; }
  security::EncryptionContext context() const { return {"tasks-state-v1", owner}; }
  security::SecretId secretId() const { return {"tasks", owner, "password"}; }
  void publish(std::string status = {}) {
    if (!status.empty())
      view.status = std::move(status);
    view.syncing = busy || credentialBusy;
    view.writable = settings.enabled
        && !collection.empty()
        && loaded
        && storageWritable
        && keys.state() == security::StorageKeyState::Ready;
    view.items.clear();
    if (!settings.enabled) {
      view.trash.clear();
      view.history.clear();
      archiveViewDirty = historyViewDirty = true;
    } else {
      if (archiveViewDirty) {
        view.trash.clear();
        for (const auto& [id, a] : archive)
          view.trash.push_back({id, a->item, a->deletedAt, a->source});
        std::stable_sort(view.trash.begin(), view.trash.end(), [](const auto& a, const auto& b) {
          return a.deletedAt > b.deletedAt;
        });
        archiveViewDirty = false;
      }
      for (auto& t : view.trash) {
        const auto& a = *archive.at(t.id);
        const auto p = journal.find(a.taskId);
        t.pending = p != journal.end();
        t.conflict = t.pending && p->second.conflict;
        t.restorable = !t.pending ? !a.queued
                                  : a.queued
                && p->second.removed
                && !p->second.conflict
                && p->second.sent.empty()
                && !p->second.sentDelete
                && !p->second.deleteAttempted
                && !p->second.createAttempted
                && !busy;
      }
      if (historyViewDirty) {
        view.history = history;
        historyViewDirty = false;
      }
    }
    if (settings.enabled) {
      for (const auto& [id, resource] : remote) {
        if (journal.contains(id))
          continue;
        auto item = tasks::parseTask(resource.data);
        if (item) {
          item->id = id;
          view.items.push_back(std::move(*item));
        }
      }
      for (const auto& [id, pending] : journal) {
        if (pending.removed && !pending.conflict)
          continue;
        auto item = tasks::parseTask(pending.desired);
        if (!item)
          continue;
        item->id = id;
        item->pending = true;
        item->conflict = pending.conflict;
        view.items.push_back(std::move(*item));
      }
      std::stable_sort(view.items.begin(), view.items.end(), [](const TaskItem& a, const TaskItem& b) {
        if (a.completed != b.completed)
          return !a.completed;
        if (a.dueDate.empty() != b.dueDate.empty())
          return !a.dueDate.empty();
        if (a.dueDate != b.dueDate)
          return a.dueDate < b.dueDate;
        return a.title < b.title;
      });
    }
    ++view.revision;
    if (changed)
      changed();
  }
  void later() {
    immediate = false;
    next = Clock::now() + std::chrono::seconds(std::clamp(settings.syncIntervalSeconds, 15, 86400));
  }
  void rememberPending() {
    if (owner.empty() || !loaded)
      return;
    if (journal.empty() && archive.empty() && history.empty())
      knownPendingAccounts.erase(owner);
    else
      knownPendingAccounts.insert(owner);
  }
  bool
  persist(const tasks::Resources& resources, const Journal& pending, const Archive& archived, const History& events) {
    auto key = keys.deriveKey(security::StorageKeyPurpose::Tasks);
    if (!key || path.empty()) {
      storageWritable = false;
      publish("Unlock encrypted storage before changing tasks; local changes have not been accepted.");
      return false;
    }
    try {
      Json data{
          {"version", 2},           {"owner", owner},          {"resources", Json::array()}, {"journal", Json::array()},
          {"trash", Json::array()}, {"history", Json::array()}
      };
      for (const auto& [id, r] : resources)
        data["resources"].push_back({{"id", id}, {"etag", r.etag}, {"data", r.data}});
      for (const auto& [id, p] : pending) {
        auto entry = encodePending(p);
        entry["id"] = id;
        data["journal"].push_back(std::move(entry));
      }
      for (const auto& [id, a] : archived) {
        Json entry{{"id", id},
                   {"task_id", a->taskId},
                   {"data", a->data},
                   {"source", a->source},
                   {"deleted_at", a->deletedAt},
                   {"queued", a->queued}};
        entry["prior"] = a->prior ? encodePending(*a->prior) : Json(nullptr);
        data["trash"].push_back(std::move(entry));
      }
      for (const auto& e : events)
        data["history"].push_back(
            {{"id", e.id},
             {"task_id", e.taskId},
             {"title", e.title},
             {"action", e.action},
             {"source", e.source},
             {"details", e.details},
             {"occurred_at", e.occurredAt}}
        );
      auto serialized = data.dump();
      const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(serialized.data()), serialized.size());
      const bool ok = serialized.size() <= MaxStateBytes && security::writeEncryptedFile(path, bytes, *key, context());
      sodium_memzero(serialized.data(), serialized.size());
      if (ok) {
        storageWritable = true;
        keys.noteEncryptedDataExists();
        return true;
      }
    } catch (const std::exception&) {
    }
    storageWritable = false;
    publish("Cannot save encrypted task journal. Check storage space and permissions; no new changes were accepted.");
    return false;
  }
  bool persist(const tasks::Resources& resources, const Journal& pending) {
    return persist(resources, pending, archive, history);
  }
  bool decode(
      const std::vector<std::uint8_t>& bytes, tasks::Resources& resources, Journal& pending, Archive& archived,
      History& events
  ) {
    try {
      const auto data = Json::parse(bytes);
      const auto version = data.at("version").get<int>();
      if ((version != 1 && version != 2) || data.at("owner").get<std::string>() != owner)
        return false;
      for (const auto& r : data.at("resources")) {
        const auto id = r.at("id").get<std::string>();
        tasks::Resource value{r.at("etag").get<std::string>(), r.at("data").get<std::string>()};
        if (tasks::resourceUrl(collection, id) != std::optional(id)
            || !tasks::strongEtag(value.etag)
            || !tasks::parseTask(value.data)
            || !resources.emplace(id, std::move(value)).second)
          return false;
      }
      for (const auto& p : data.at("journal")) {
        const auto id = p.at("id").get<std::string>();
        Pending value = decodePending(p);
        if (tasks::resourceUrl(collection, id) != std::optional(id)
            || !validPending(value)
            || !pending.emplace(id, std::move(value)).second)
          return false;
      }
      if (version == 1) {
        for (auto& [id, p] : pending) {
          if (!p.removed)
            continue;
          // Version 1 did not retain prior intent or whether earlier DELETE attempts occurred.
          // Preserve its calendar without inventing a deletion date or allowing unsafe undo.
          p.deleteAttempted = true;
          archiveTask(archived, id, p.desired, "Desktop", true, {}, 0);
        }
      }
      if (version == 2) {
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
          if (id.empty()
              || !item
              || tasks::resourceUrl(collection, value->taskId) != std::optional(value->taskId)
              || value->deletedAt < 0
              || (value->source != "Desktop" && value->source != "Server")
              || (value->prior && (!validPending(*value->prior) || value->prior->removed || value->prior->conflict))
              || (value->queued && (!pending.contains(value->taskId) || !pending.at(value->taskId).removed)))
            return false;
          value->item = *item;
          value->item.id = value->taskId;
          if (!archived.emplace(id, std::move(value)).second)
            return false;
        }
        for (const auto& e : data.at("history")) {
          TaskHistoryEntry value{e.at("id").get<std::string>(),          e.at("task_id").get<std::string>(),
                                 e.at("title").get<std::string>(),       e.at("action").get<std::string>(),
                                 e.at("source").get<std::string>(),      e.at("details").get<std::string>(),
                                 e.at("occurred_at").get<std::int64_t>()};
          if (value.id.empty()
              || value.occurredAt < 0
              || events.size() >= 500
              || tasks::resourceUrl(collection, value.taskId) != std::optional(value.taskId))
            return false;
          events.push_back(std::move(value));
        }
      }
      return true;
    } catch (const std::exception&) {
      return false;
    }
  }
  void syncPersistence() {
    if (owner.empty())
      return;
    if (keys.state() != security::StorageKeyState::Ready) {
      storageWritable = false;
      publish("Encrypted task storage is locked or unavailable. Unlock storage to edit or sync queued changes.");
      return;
    }
    if (!loaded) {
      auto key = keys.deriveKey(security::StorageKeyPurpose::Tasks);
      if (!key || path.empty()) {
        storageWritable = false;
        publish("No private task storage is available.");
        return;
      }
      auto result = security::readEncryptedFile(path, *key, context(), MaxStateBytes);
      if (result.succeeded()) {
        tasks::Resources resources;
        Journal pending;
        Archive archived;
        History events;
        const bool ok = decode(result.plaintext, resources, pending, archived, events);
        sodium_memzero(result.plaintext.data(), result.plaintext.size());
        if (!ok) {
          storageWritable = false;
          publish("Encrypted task journal is invalid. Preserve it before using storage recovery.");
          return;
        }
        remote = std::move(resources);
        journal = std::move(pending);
        archive = std::move(archived);
        history = std::move(events);
        archiveViewDirty = historyViewDirty = true;
        loaded = true;
        rememberPending();
      } else if (result.status == security::EncryptedReadStatus::NotFound)
        loaded = true;
      else {
        storageWritable = false;
        publish("Cannot unlock/read encrypted tasks. Check the storage key; cached edits were not replaced.");
        return;
      }
    }
    if (persist(remote, journal)) {
      publish(journal.empty() ? "Tasks ready." : "Saved local changes are waiting to sync.");
      immediate = true;
    }
  }
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
    remote.clear();
    journal.clear();
    archive.clear();
    history.clear();
    archiveViewDirty = historyViewDirty = true;
    owner.clear();
    collection.clear();
    path.clear();
    view.needsPassword = false;
    initialized = true;
    immediate = false;
    next = Clock::time_point::max();
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
      immediate = false;
      publish("Tasks are disabled in configuration.");
      return;
    }
    if (owner.empty()) {
      publish("Configure tasks.collection_url and tasks.username to connect Tasks.org.");
      return;
    }
    immediate = true;
    publish(loaded ? "Connecting to tasks collection…" : "Unlock encrypted storage to load tasks.");
  }
  void authenticate() {
    if (credentialBusy)
      return;
    if (!settings.passwordFile.empty()) {
      auto result = calendar::readCredentialFile(settings.passwordFile);
      if (result.status == calendar::CredentialFileStatus::Success && !result.value.empty()) {
        password = std::move(result.value);
        view.needsPassword = false;
        immediate = true;
      } else {
        later();
        publish("Cannot read task password_file. Use a private, user-owned credential file.");
      }
      return;
    }
    credentialBusy = true;
    publish("Looking up task credentials…");
    const auto gen = generation;
    const std::weak_ptr<int> alive = life;
    credentialOperation = secrets.lookup(
        secretId(), [this, alive, gen](security::SecretStoreStatus status, security::SecureBuffer value) {
          if (alive.expired() || !current(gen))
            return;
          credentialBusy = false;
          if (status == security::SecretStoreStatus::Success && !value.empty()) {
            password = std::move(value);
            view.needsPassword = false;
            immediate = true;
            publish("Task credentials ready.");
          } else {
            view.needsPassword = true;
            later();
            publish(
                status == security::SecretStoreStatus::NotFound
                    ? "Enter the tasks account password."
                    : "Unlock Secret Service, then enter the tasks password or configure password_file."
            );
          }
        }
    );
  }
  HttpRequest request(std::string method, const std::string& url) const {
    HttpRequest req;
    req.method = std::move(method);
    req.url = url;
    req.basicUsername = settings.username;
    const auto bytes = password.bytes();
    req.basicPassword.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return req;
  }
  void failed(const HttpResponse& response) {
    busy = false;
    fresh = false;
    later();
    if (response.transportOk && (response.status == 401 || response.status == 403)) {
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
  void refresh() {
    busy = true;
    immediate = false;
    fresh = false;
    publish("Refreshing tasks…");
    auto req = request("REPORT", collection);
    req.headers = {"Depth: 1", "Content-Type: application/xml; charset=utf-8"};
    req.body = Query;
    const auto gen = generation;
    const std::weak_ptr<int> alive = life;
    http.request(std::move(req), [this, alive, gen](HttpResponse response) {
      if (alive.expired() || !current(gen))
        return;
      if (!response.transportOk || response.status != 207) {
        failed(response);
        return;
      }
      tasks::Resources resources;
      if (!tasks::parseReport(response.body, collection, resources)) {
        busy = false;
        later();
        publish("Invalid or unsafe CalDAV task response. Cached tasks and queued changes are unchanged.");
        return;
      }
      auto pending = journal;
      std::optional<Archive> archived;
      std::optional<History> events;
      auto archives = [&]() -> Archive& {
        if (!archived)
          archived = archive;
        return *archived;
      };
      auto log = [&](const std::string& id, const std::string& data, const std::string& action,
                     const std::string& source, const std::string& details) {
        if (!events)
          events = history;
        record(*events, id, data, action, source, details);
      };
      bool resourcesChanged = resources.size() != remote.size();
      for (const auto& [id, r] : resources) {
        const auto before = remote.find(id);
        const auto p = journal.find(id);
        const bool echo = p != journal.end()
            && ((!p->second.sent.empty() && tasks::equivalent(r.data, p->second.sent))
                || ((!p->second.removed || p->second.createAttempted) && tasks::equivalent(r.data, p->second.desired)));
        if (before == remote.end()) {
          resourcesChanged = true;
          if (!echo)
            log(id, r.data, "Discovered", "Server", "First observed in this account's local history.");
        } else if (before->second.etag != r.etag || before->second.data != r.data) {
          resourcesChanged = true;
          if (!echo && !tasks::equivalent(before->second.data, r.data)) {
            const auto oldItem = *tasks::parseTask(before->second.data);
            const auto newItem = *tasks::parseTask(r.data);
            log(id, r.data,
                oldItem.completed != newItem.completed ? (newItem.completed ? "Completed" : "Reopened") : "Edited",
                "Server", changes(oldItem, newItem));
          }
        }
      }
      for (const auto& [id, r] : remote) {
        if (resources.contains(id))
          continue;
        resourcesChanged = true;
        const auto p = journal.find(id);
        if (p == journal.end() || !p->second.removed) {
          archiveTask(archives(), id, r.data, "Server");
          log(id, r.data, "Deleted", "Server",
              "Server task disappeared; its last observed calendar is in local Trash.");
        }
      }
      bool deferRetry = false;
      for (auto it = pending.begin(); it != pending.end();) {
        auto& p = it->second;
        const auto found = resources.find(it->first);
        const bool exists = found != resources.end();
        if (p.sentDelete && !exists) {
          it = pending.erase(it);
          continue;
        }
        if (exists
            && ((!p.sent.empty() && tasks::equivalent(found->second.data, p.sent))
                || (p.createAttempted && p.removed && tasks::equivalent(found->second.data, p.desired)))) {
          if (!p.removed && tasks::equivalent(p.desired, p.sent)) {
            it = pending.erase(it);
            continue;
          }
          p.baseEtag = found->second.etag;
          p.sent.clear();
          p.sentDelete = false;
          p.conflict = false;
          p.createAttempted = false;
        }
        if (p.removed && !exists) {
          if (p.createAttempted) {
            if (!p.conflict)
              log(it->first, p.desired, "Conflict", "Server",
                  "An interrupted creation is still uncertain; deletion and its recovery copy were retained.");
            p.conflict = true;
            ++it;
          } else {
            it = pending.erase(it);
          }
          continue;
        }
        if (!p.removed && exists && tasks::equivalent(found->second.data, p.desired)) {
          it = pending.erase(it);
          continue;
        }
        // Never replay a rejected precondition, even if a broken server repeats its ETag.
        if (p.conflict) {
          ++it;
          continue;
        }
        const bool unchanged = p.baseEtag.empty() ? !exists : exists && p.baseEtag == found->second.etag;
        if (!unchanged) {
          p.conflict = true;
          log(it->first, p.desired, "Conflict", "Server", "Server state changed; the local draft was retained.");
        } else {
          deferRetry = deferRetry || !p.sent.empty() || p.sentDelete;
          p.sent.clear();
          p.sentDelete = false;
        }
        ++it;
      }
      for (const auto& [id, a] : archive) {
        if (a->queued && !pending.contains(a->taskId)) {
          auto confirmed = std::make_shared<Archived>(*a);
          confirmed->queued = false;
          confirmed->prior.reset();
          archives().at(id) = std::move(confirmed);
        }
      }
      busy = false;
      const bool stateChanged = resourcesChanged || pending != journal || archived || events;
      if (!loaded
          || (stateChanged
              && !persist(resources, pending, archived ? *archived : archive, events ? *events : history))) {
        later();
        return;
      }
      remote = std::move(resources);
      journal = std::move(pending);
      if (archived) {
        archive = std::move(*archived);
        archiveViewDirty = true;
      }
      if (events) {
        history = std::move(*events);
        historyViewDirty = true;
      }
      rememberPending();
      fresh = true;
      const bool conflicts =
          std::any_of(journal.begin(), journal.end(), [](const auto& p) { return p.second.conflict; });
      publish(
          conflicts ? "Conflict: phone/server changes were preserved. Edit to apply only your newly changed fields, or "
                      "discard local changes. Deleted remote tasks must be added as new."
              : journal.empty() ? "Tasks synced."
                                : "Sending saved task changes…"
      );
      if (deferRetry)
        later();
      else
        replay();
    });
  }
  void replay() {
    if (busy || !fresh || !view.writable) {
      later();
      return;
    }
    auto it = std::find_if(journal.begin(), journal.end(), [](const auto& p) { return !p.second.conflict; });
    if (it == journal.end()) {
      later();
      return;
    }
    const auto id = it->first;
    if (tasks::resourceUrl(collection, id) != std::optional(id)) {
      fresh = false;
      later();
      publish("Refusing an unsafe task resource URL. No credentials or changes were sent.");
      return;
    }
    auto pending = journal;
    auto& p = pending.at(id);
    p.sentDelete = p.removed;
    p.deleteAttempted = p.deleteAttempted || p.removed;
    p.createAttempted = p.createAttempted || (!p.removed && p.baseEtag.empty());
    p.sent = p.removed ? std::string{} : p.desired;
    if (!persist(remote, pending)) {
      later();
      return;
    }
    journal = std::move(pending);
    const auto& sent = journal.at(id);
    auto req = request(sent.removed ? "DELETE" : "PUT", id);
    req.headers.push_back(sent.baseEtag.empty() ? "If-None-Match: *" : "If-Match: " + sent.baseEtag);
    if (!sent.removed) {
      req.headers.push_back("Content-Type: text/calendar; charset=utf-8");
      req.body = sent.sent;
    }
    busy = true;
    fresh = false;
    immediate = false;
    publish("Syncing saved task changes…");
    const auto gen = generation;
    const std::weak_ptr<int> alive = life;
    http.request(std::move(req), [this, alive, gen, id](HttpResponse response) {
      if (alive.expired() || !current(gen))
        return;
      busy = false;
      if (response.transportOk && response.status == 412) {
        auto conflicted = journal;
        auto events = history;
        if (auto found = conflicted.find(id); found != conflicted.end() && !found->second.conflict) {
          found->second.conflict = true;
          record(
              events, id, found->second.desired, "Conflict", "Server",
              "Conditional write rejected; the local draft was retained."
          );
        }
        if (!persist(remote, conflicted, archive, events)) {
          later();
          return;
        }
        journal = std::move(conflicted);
        history = std::move(events);
        historyViewDirty = true;
        rememberPending();
        immediate = true;
        publish("Task changed on the server; refreshing before resolving the conflict.");
        return;
      }
      if (response.transportOk
          && ((response.status >= 200 && response.status < 300) || response.status == 404 || response.status == 410)) {
        // HttpClient does not expose ETag headers. Read back the authoritative resource before acknowledging the
        // journal.
        immediate = true;
        publish("Task change sent; confirming server state…");
        return;
      }
      failed(response);
    });
  }
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
  bool commit(
      Journal pending, Archive archived, History events,
      const std::string& status = "Task changes saved locally; waiting to sync."
  ) {
    if (!persist(remote, pending, archived, events))
      return false;
    journal = std::move(pending);
    archive = std::move(archived);
    history = std::move(events);
    archiveViewDirty = historyViewDirty = true;
    rememberPending();
    fresh = false;
    immediate = true;
    publish(status);
    return true;
  }
  bool edit(const std::string& id, const TaskDraft* draft, std::optional<bool> completed) {
    if (!readyToEdit())
      return false;
    if (draft && !tasks::validDraft(*draft)) {
      publish("Enter a task title and a valid due date (YYYY-MM-DD), or leave the date empty.");
      return false;
    }
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
    auto pending = journal;
    auto old = pending.find(id);
    const auto server = remote.find(id);
    std::string source;
    if (old != pending.end()) {
      if (old->second.removed && !old->second.conflict) {
        publish("This task is awaiting deletion. Add a new task instead.");
        return false;
      }
      if (old->second.conflict) {
        if (server == remote.end()) {
          publish("This task was deleted remotely. Copy its text into a new task, or discard the local changes.");
          return false;
        }
        if (busy) {
          publish("Wait for the refresh to finish before resolving this conflict.");
          return false;
        }
        source = server->second.data;
        old->second = Pending{server->second.etag, source};
      } else
        source = old->second.desired;
    } else {
      if (server == remote.end()) {
        publish("Task disappeared. Refresh and try again.");
        return false;
      }
      source = server->second.data;
      old = pending.emplace(id, Pending{server->second.etag, source}).first;
    }
    const auto edited = tasks::editTask(source, *displayed, draft, completed);
    if (edited.empty()) {
      publish("This VTODO could not be edited safely. Its original data was preserved.");
      return false;
    }
    old->second.desired = edited;
    old->second.removed = false;
    auto events = history;
    const auto result = *tasks::parseTask(edited);
    record(
        events, id, edited, completionChanged ? (result.completed ? "Completed" : "Reopened") : "Edited", "Desktop",
        changes(*displayed, result)
    );
    auto archived = archive;
    // Resolving a rejected deletion keeps the server task; it is no longer a queued deletion.
    std::erase_if(archived, [&](const auto& a) { return a.second->taskId == id && a.second->queued; });
    return commit(std::move(pending), std::move(archived), std::move(events));
  }
};

TaskService::TaskService(ConfigService& c, HttpClient& h, security::SecretStore& s, security::StorageKeyProvider& k)
    : m(std::make_unique<Impl>(c, h, s, k)) {}
TaskService::~TaskService() = default;
void TaskService::initialize() {
  if (m->initialized)
    return;
  const std::weak_ptr<int> alive = m->life;
  auto* impl = m.get();
  m->config.addReloadCallback(
      [impl, alive] {
        if (!alive.expired())
          impl->configure();
      },
      "tasks"
  );
  m->configure();
}
void TaskService::tick() {
  m->configure();
  if (!m->settings.enabled || m->owner.empty() || m->busy || m->credentialBusy)
    return;
  if (!m->immediate && Clock::now() < m->next)
    return;
  m->later();
  if (!m->loaded || !m->storageWritable)
    m->syncPersistence();
  if (!m->loaded || !m->storageWritable) {
    m->later();
    return;
  }
  if (m->password.empty()) {
    m->authenticate();
    return;
  }
  m->refresh();
}
int TaskService::pollTimeoutMs() const {
  if (!m->settings.enabled || m->owner.empty() || m->busy || m->credentialBusy)
    return -1;
  if (m->immediate)
    return 0;
  if (m->next == Clock::time_point::max())
    return -1;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(m->next - Clock::now()).count();
  return static_cast<int>(std::clamp<std::int64_t>(ms, 0, std::numeric_limits<int>::max()));
}
const TaskSnapshot& TaskService::snapshot() const { return m->view; }
void TaskService::setChangeCallback(std::function<void()> callback) { m->changed = std::move(callback); }
void TaskService::requestRefresh() {
  m->configure();
  if (m->settings.enabled && !m->owner.empty())
    m->immediate = true;
}
bool TaskService::addTask(const TaskDraft& draft) {
  if (!m->readyToEdit())
    return false;
  if (!tasks::validDraft(draft)) {
    m->publish("Enter a task title and a valid due date (YYYY-MM-DD), or leave the date empty.");
    return false;
  }
  const auto uid = tasks::randomId();
  const auto id = tasks::resourceUrl(m->collection, uid + ".ics");
  if (!id) {
    m->publish("Invalid collection URL; cannot create tasks safely.");
    return false;
  }
  auto pending = m->journal;
  const auto data = tasks::newTask(draft, uid);
  if (data.empty() || m->remote.contains(*id) || !pending.emplace(*id, Pending{{}, data}).second) {
    m->publish("Could not allocate a new task identity. No change was saved; please retry.");
    return false;
  }
  auto events = m->history;
  record(events, *id, data, "Created", "Desktop", "New task saved locally.");
  return m->commit(std::move(pending), m->archive, std::move(events));
}
bool TaskService::updateTask(const std::string& id, const TaskDraft& draft) {
  return m->edit(id, &draft, std::nullopt);
}
bool TaskService::setCompleted(const std::string& id, bool completed) { return m->edit(id, nullptr, completed); }
bool TaskService::removeTask(const std::string& id) {
  if (!m->readyToEdit())
    return false;
  auto pending = m->journal;
  auto archived = m->archive;
  auto events = m->history;
  auto found = pending.find(id);
  if (found != pending.end() && found->second.conflict) {
    if (m->busy) {
      m->publish("Wait for synchronization to finish before discarding a conflict.");
      return false;
    }
    record(
        events, id, found->second.desired, "Discarded", "Desktop",
        "Local conflict draft discarded; no server task was deleted."
    );
    pending.erase(found);
    std::erase_if(archived, [&](const auto& a) { return a.second->taskId == id && a.second->queued; });
  } else if (found != pending.end()) {
    if (found->second.removed) {
      m->publish("This task is already in local Trash.");
      return true;
    }
    const auto prior = found->second;
    const bool unsentCreate =
        prior.baseEtag.empty() && prior.sent.empty() && !prior.sentDelete && !prior.createAttempted;
    archiveTask(
        archived, id, prior.desired, "Desktop", !unsentCreate, unsentCreate ? std::nullopt : std::optional(prior)
    );
    record(
        events, id, prior.desired, "Deleted", "Desktop",
        unsentCreate ? "Unsent new task moved to local Trash." : "Moved to local Trash; server deletion queued."
    );
    if (unsentCreate)
      pending.erase(found);
    else
      found->second.removed = true;
  } else {
    const auto server = m->remote.find(id);
    if (server == m->remote.end()) {
      m->publish("Task is already absent. Refresh the list.");
      return false;
    }
    archiveTask(archived, id, server->second.data, "Desktop", true);
    record(events, id, server->second.data, "Deleted", "Desktop", "Moved to local Trash; server deletion queued.");
    pending.emplace(id, Pending{server->second.etag, server->second.data, true});
  }
  return m->commit(std::move(pending), std::move(archived), std::move(events));
}
bool TaskService::restoreTask(const std::string& trashId) {
  if (!m->readyToEdit())
    return false;
  const auto found = m->archive.find(trashId);
  if (found == m->archive.end()) {
    m->publish("This local Trash entry is no longer available.");
    return false;
  }
  const auto& a = *found->second;
  auto pending = m->journal;
  auto p = pending.find(a.taskId);
  std::string restoredId = a.taskId, restoredData = a.data, details;
  if (a.queued) {
    if (p == pending.end()
        || !p->second.removed
        || p->second.conflict
        || p->second.sentDelete
        || p->second.deleteAttempted
        || p->second.createAttempted
        || !p->second.sent.empty()
        || m->busy) {
      m->publish("Deletion is in flight, uncertain, or conflicting. Refresh and reconcile it before restoring.");
      return false;
    }
    if (a.prior) {
      auto prior = *a.prior;
      // A preceding PUT may have been confirmed after the delete was queued.
      prior.baseEtag = p->second.baseEtag;
      prior.sent.clear();
      prior.sentDelete = false;
      prior.createAttempted = false;
      p->second = std::move(prior);
    } else
      pending.erase(p);
    details = "Unsent deletion undone; previous local task intent restored.";
  } else {
    if (p != pending.end()) {
      m->publish("The original task still has a pending or conflicting mutation. Reconcile it before restoring.");
      return false;
    }
    const auto uid = tasks::randomId();
    const auto id = tasks::resourceUrl(m->collection, uid + ".ics");
    restoredData = tasks::restoreTask(a.data, uid);
    if (!id
        || restoredData.empty()
        || m->remote.contains(*id)
        || !pending.emplace(*id, Pending{{}, restoredData}).second) {
      m->publish("Could not restore this calendar with a new identity. Local Trash was preserved.");
      return false;
    }
    restoredId = *id;
    details = "Restored a copy with a new UID/resource, preserving the complete archived calendar.";
  }
  auto archived = m->archive;
  archived.erase(trashId);
  auto events = m->history;
  record(events, restoredId, restoredData, "Restored", "Desktop", std::move(details));
  return m->commit(std::move(pending), std::move(archived), std::move(events));
}
bool TaskService::purgeTask(const std::string& trashId) {
  if (!m->readyToEdit())
    return false;
  const auto found = m->archive.find(trashId);
  if (found == m->archive.end()) {
    m->publish("This local Trash entry is no longer available.");
    return false;
  }
  if (found->second->queued || m->journal.contains(found->second->taskId)) {
    m->publish("Cannot purge local Trash while the original task has pending or conflicting changes.");
    return false;
  }
  auto events = m->history;
  record(
      events, found->second->taskId, found->second->data, "Discarded", "Desktop",
      "Local Trash entry permanently removed; no server request was made."
  );
  auto archived = m->archive;
  archived.erase(trashId);
  if (!m->persist(m->remote, m->journal, archived, events))
    return false;
  m->archive = std::move(archived);
  m->history = std::move(events);
  m->archiveViewDirty = m->historyViewDirty = true;
  m->rememberPending();
  m->publish("Local Trash entry purged. Server tasks were not changed.");
  return true;
}
bool TaskService::clearHistory() {
  if (!m->readyToEdit())
    return false;
  if (!m->history.empty()) {
    if (!m->persist(m->remote, m->journal, m->archive, {}))
      return false;
    m->history.clear();
    m->historyViewDirty = true;
    m->rememberPending();
  }
  m->publish("Local task history cleared. Tasks and Trash were not changed.");
  return true;
}
void TaskService::savePassword(std::string value) {
  m->configure();
  if (m->owner.empty() || !m->settings.passwordFile.empty() || value.empty()) {
    sodium_memzero(value.data(), value.size());
    m->publish("Configure the tasks account first; password_file credentials must be updated in that file.");
    return;
  }
  const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
  security::SecureBuffer secret(bytes);
  sodium_memzero(value.data(), value.size());
  m->credentialOperation.cancel();
  m->credentialBusy = true;
  m->publish("Saving task password in Secret Service…");
  const auto gen = m->generation;
  const std::weak_ptr<int> alive = m->life;
  auto* impl = m.get();
  m->credentialOperation = m->secrets.store(
      m->secretId(), std::move(secret), "TerminusBar Tasks account password",
      [impl, alive, gen](security::SecretStoreStatus status) {
        if (alive.expired() || !impl->current(gen))
          return;
        impl->credentialBusy = false;
        impl->password.clear();
        if (status == security::SecretStoreStatus::Success) {
          impl->view.needsPassword = false;
          impl->immediate = true;
          impl->publish("Task password saved securely.");
        } else {
          impl->view.needsPassword = true;
          impl->later();
          impl->publish(
              "Task password was not saved. Unlock Secret Service and try again; no plaintext fallback is used."
          );
        }
      }
  );
}
void TaskService::syncPersistence() { m->syncPersistence(); }
bool TaskService::hasEncryptedPersistence() const {
  const auto directory = stateDirectory();
  if (directory.empty())
    return false;
  std::error_code ec;
  if (!std::filesystem::exists(directory, ec))
    return bool(ec);
  for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() == ".enc")
      return true;
  }
  return bool(ec);
}
bool TaskService::clearEncryptedPersistenceForRecovery() {
  m->rememberPending();
  if (!m->knownPendingAccounts.empty()
      || !m->journal.empty()
      || !m->archive.empty()
      || !m->history.empty()
      || m->busy) {
    m->publish(
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
  std::vector<std::filesystem::path> paths;
  auto key = m->keys.deriveKey(security::StorageKeyPurpose::Tasks);
  for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() != ".enc")
      continue;
    // Other account outboxes, Trash and history must never be silently erased by recovery.
    if (it->path() != m->path) {
      m->publish(
          "Task recovery refused: encrypted data for another account exists. Preserve and resolve that account's "
          "journal, Trash and history first."
      );
      return false;
    }
    if (!key) {
      m->publish("Task recovery refused: unlock and preserve encrypted task records before resetting storage.");
      return false;
    }
    auto read = security::readEncryptedFile(it->path(), *key, m->context(), MaxStateBytes);
    if (!read.succeeded()) {
      m->publish("Task recovery refused: unreadable task records may contain local Trash, history or pending changes.");
      return false;
    }
    tasks::Resources resources;
    Journal pending;
    Archive archived;
    History events;
    const bool ok = m->decode(read.plaintext, resources, pending, archived, events);
    sodium_memzero(read.plaintext.data(), read.plaintext.size());
    if (!ok || !pending.empty() || !archived.empty() || !events.empty()) {
      m->publish("Task recovery refused: preserve local changes, Trash and history before recovery.");
      return false;
    }
    paths.push_back(it->path());
  }
  if (ec)
    return false;
  for (const auto& path : paths)
    if (!std::filesystem::remove(path, ec) || ec)
      return false;
  ++m->generation;
  m->credentialOperation.cancel();
  m->credentialBusy = false;
  m->remote.clear();
  m->journal.clear();
  m->archive.clear();
  m->history.clear();
  m->archiveViewDirty = m->historyViewDirty = true;
  m->loaded = m->storageWritable = m->fresh = false;
  m->publish("Encrypted task cache cleared for explicitly requested key recovery.");
  return true;
}
