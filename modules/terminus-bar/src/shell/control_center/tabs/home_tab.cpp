#include "shell/control_center/tabs/home_tab.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "scripting/plugin_id.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/control_center/term_frame.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "idle/idle_inhibitor.h"
#include "system/gamma_service.h"
#include "system/health.h"
#include "calendar/calendar_service.h"
#include "calendar/calendar_types.h"
#include "system/weather_service.h"
#include "tasks/task_service.h"
#include "wayland/clipboard_service.h"
#include "render/core/async_texture_cache.h"
#include "shell/profile/avatar_path.h"
#include "system/distro_info.h"
#include "system/hardware_info.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "ui/controls/glyph.h"
#include "util/string_utils.h"

#include "util/secret_shape.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <wayland-client-protocol.h>
#include <sys/statvfs.h>

using namespace control_center;

namespace {

  using term_frame::Tone;

  constexpr float kTimerFontSize = 28.0F;
  // Height the big remaining time asks for.
  constexpr float kTimerTimeHeight = 44.0F;
  // The dashboard's row height at 14px text.
  constexpr float kHomeRowHeight = 20.0F;
  constexpr std::chrono::seconds kArmTimeout{5};
  constexpr std::chrono::seconds kPendingGiveUp{6 * 3600};

  void openControlCenterTab(std::string_view tab) {
    PanelManager::instance().togglePanel("control-center", PanelOpenRequest{.context = tab});
  }

  constexpr std::chrono::seconds kTimerMin{60};
  constexpr std::chrono::seconds kTimerMax{99 * 60};
  constexpr std::chrono::milliseconds kTimerTickInterval{250};

  std::chrono::seconds ceilSeconds(std::chrono::milliseconds ms) {
    return std::chrono::ceil<std::chrono::seconds>(ms);
  }

  std::string formatTimer(std::chrono::seconds total) {
    return std::format("{:02}:{:02}", total.count() / 60, total.count() % 60);
  }

  std::filesystem::path stateDir() {
    const char* state = std::getenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path base = state != nullptr && state[0] != '\0'
        ? std::filesystem::path(state)
        : std::filesystem::path(home != nullptr ? home : "") / ".local" / "state";
    return base / "terminus-est";
  }

  // Where today's timer sessions are kept (a private file of ours, not the bar's state).
  std::filesystem::path timerStatsPath() { return stateDir() / "dashboard-timer-today"; }

  // The moment the journal errors were last looked at: only newer ones count as new.
  std::filesystem::path journalSeenPath() { return stateDir() / "dashboard-journal-seen"; }

  // The last fleet answers, kept so the section shows something the moment the dashboard opens.
  std::filesystem::path fleetPath() { return stateDir() / "dashboard-fleet"; }

  void writeJournalSeen() {
    const std::filesystem::path path = journalSeenPath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream(path, std::ios::trunc) << std::time(nullptr) << '\n';
  }

  // Exit codes of actions that run in a terminal are written here by the terminal's own shell.
  std::filesystem::path runtimeDir() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    return runtime != nullptr && runtime[0] != '\0' ? std::filesystem::path(runtime) : std::filesystem::path("/tmp");
  }

  // Local date as YYYY-MM-DD.
  std::string localDateKey() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char buffer[16];
    return std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local) > 0 ? std::string(buffer) : std::string{};
  }

  // "07:42" in local time.
  std::string clockText(std::chrono::system_clock::time_point at) {
    const std::time_t t = std::chrono::system_clock::to_time_t(at);
    std::tm local{};
    localtime_r(&t, &local);
    char buffer[8];
    return std::strftime(buffer, sizeof(buffer), "%H:%M", &local) > 0 ? std::string(buffer) : std::string{};
  }

  // Cheap and fast facts, refreshed about once a minute while the dashboard is open: `key=value` lines. The kernel
  // is compared by package base: the running release against the newest installed one of the same flavour.
  constexpr const char* kLiveScript = R"SH(
sh=$(getent passwd "$(id -un)" | cut -d: -f7)
v=$("$sh" --version 2>/dev/null | grep -oE '[0-9]+(\.[0-9]+)+' | head -1)
echo "shell=$(basename "$sh") $v"
echo "wm=Hyprland $(hyprctl version -j 2>/dev/null | jq -r '.version // empty')"
echo "outputs=$(hyprctl monitors -j 2>/dev/null | jq length)"
echo "packages=$(pacman -Qq 2>/dev/null | wc -l) · $(flatpak list --app 2>/dev/null | wc -l) flatpak"
hyprctl monitors -j 2>/dev/null | jq -r 'sort_by(.id)|.[0]|"display=\(.width)x\(.height) @ \(.refreshRate|round)Hz"'
run=$(uname -r)
case $run in *-zen*) pk=linux-zen;; *-lts*) pk=linux-lts;; *-hardened*) pk=linux-hardened;; *-rt*) pk=linux-rt;; *) pk=linux;; esac
inst=
for d in /usr/lib/modules/*/; do
  d=${d%/}
  [ "$(cat "$d/pkgbase" 2>/dev/null)" = "$pk" ] && inst=$(basename "$d")
done
echo "kernel_run=$run"
echo "kernel_inst=${inst:-$run}"
if [ -d "/usr/lib/modules/$run" ]; then echo "reboot=0"; else echo "reboot=1"; fi
echo "failed=$({ systemctl --failed --no-legend --plain; systemctl --user --failed --no-legend --plain; } 2>/dev/null | awk 'NF{print $1}' | sed 's/\.service$//' | paste -sd, -)"
state="${XDG_STATE_HOME:-$HOME/.local/state}/terminus-est/dashboard-journal-seen"
seen=$(cat "$state" 2>/dev/null)
echo "journal_total=$(journalctl -b -p err -q --no-pager -o json 2>/dev/null | wc -l)"
if [ -n "$seen" ]; then
  echo "journal_new=$(journalctl -b -p err -q --no-pager -o json --since "@$seen" 2>/dev/null | wc -l)"
else
  echo "journal_new=0"
fi
)SH";

  // The slow package check (checkupdates and yay -Qua take seconds), refreshed every 30 minutes and on open.
  // `last` is the last full system upgrade in pacman.log.
  constexpr const char* kUpkeepScript = R"SH(
echo "repo=$(checkupdates 2>/dev/null | wc -l)"
echo "aur=$(yay -Qua 2>/dev/null | grep -c .)"
echo "cache=$(du -sb /var/cache/pacman/pkg 2>/dev/null | cut -f1)"
echo "orphans=$(pacman -Qdtq 2>/dev/null | wc -l)"
l=$(grep 'starting full system upgrade' /var/log/pacman.log 2>/dev/null | tail -1 | sed -E 's/^\[([^]]+)\].*/\1/')
echo "last=$(date -d "$l" +%s 2>/dev/null || echo 0)"
)SH";

  // The notifications still on screen in mako, which owns org.freedesktop.Notifications on this desktop, so the
  // bar's own history never sees them: `count=N` and up to three `note_N=id<TAB>summary`, newest first. Mako has no
  // read state; a notification that is not dismissed counts as unread.
  constexpr const char* kNotesScript = R"SH(
makoctl list -j 2>/dev/null | jq -r '[.[]|select((.summary // "") != "" or (.body // "") != "")]|sort_by(-.id)|"count=\(length)", (.[:8]|to_entries[]|(.value.app_name // "") as $a|((.value.summary // .value.body // "")|gsub("[\\n\\r\\t]";" ")) as $s|"note_\(.key+1)=\(.value.id)\t\(if $a == "" then $s else "\($a): \($s)" end)")' 2>/dev/null
)SH";

  // The fleet checks, each with its own short timeout; a check that fails prints nothing for its key and the row
  // says "unknown". Tailscale comes from `tailscale status --json`, as the network panel reads it. Syncthing's REST
  // API key is read from its own config.xml inside this script and handed to curl on stdin, so it is never an
  // argument, never printed and never kept by the bar.
  constexpr const char* kFleetScript = R"SH(
if command -v tailscale >/dev/null 2>&1; then
  ts=$(timeout 3 tailscale status --json 2>/dev/null)
  if [ -n "$ts" ]; then
    echo "ts_state=$(printf '%s' "$ts" | jq -r '.BackendState // "unknown"')"
    echo "ts_total=$(printf '%s' "$ts" | jq '[.Peer[]?]|length')"
    echo "ts_online=$(printf '%s' "$ts" | jq '[.Peer[]?|select(.Online)]|length')"
    printf '%s' "$ts" | jq -r '[.Peer[]?|select(.HostName!="server")]|sort_by(.HostName)|"peer_count=\(length)", (to_entries[]|"peer_\(.key+1)=\(.value.HostName)\t\(if .value.Online then "online" else "offline" end)\t\((.value.LastSeen // "")|sub("\\.[0-9]+Z$";"Z")|try fromdate catch 0)")'
    echo "server=$(printf '%s' "$ts" | jq -r '[.Peer[]?|select(.HostName=="server")][0]|if .==null then "none" elif .Online then "online" else "offline" end')"
    if [ "$(printf '%s' "$ts" | jq '[.Peer[]?|select(.HostName=="server" and .Online)]|length')" = 1 ]; then
      echo "server_ms=$(timeout 5 tailscale ping -c 1 --timeout 3s server 2>/dev/null | sed -n 's/.* in \([0-9.]*\)ms.*/\1/p' | head -1)"
    fi
  else
    echo "ts_state=unknown"
    echo "server=unknown"
  fi
else
  echo "tailscale=absent"
fi
cfg="${XDG_STATE_HOME:-$HOME/.local/state}/syncthing/config.xml"
if [ -r "$cfg" ] && command -v syncthing >/dev/null 2>&1; then
  key=$(sed -n 's:.*<apikey>\(.*\)</apikey>.*:\1:p' "$cfg" | head -1)
  addr=$(sed -n '/<gui /,/<\/gui>/s:.*<address>\([^<]*\)</address>.*:\1:p' "$cfg" | head -1)
  scheme=http
  grep -q '<gui [^>]*tls="true"' "$cfg" && scheme=https
  echo "syncthing_url=$scheme://$addr"
  api() { printf 'header = "X-API-Key: %s"\n' "$key" | curl -fsS -k -m 3 -K - "$scheme://$addr$1" 2>/dev/null; }
  st=unknown
  if [ -n "$key" ] && [ -n "$addr" ]; then
    folders=$(api /rest/config/folders)
    if [ -n "$folders" ]; then
      st=ok
      for id in $(printf '%s' "$folders" | jq -r '.[]|select(.paused|not)|.id'); do
        s=$(api "/rest/db/status?folder=$id")
        if [ -z "$s" ]; then st=unknown; break; fi
        one=$(printf '%s' "$s" | jq -r 'if (.state|test("sync|scan")) then "syncing:\(if .globalBytes > 0 then ((.inSyncBytes * 100 / .globalBytes) | floor) else 100 end)" elif .state == "error" or .needTotalItems > 0 then "out" else "ok" end')
        case "$one" in
          out) st=out ;;
          syncing:*) [ "$st" != out ] && st=$one ;;
        esac
      done
    fi
  fi
  echo "syncthing=$st"
else
  echo "syncthing=absent"
fi
)SH";

  // The value of a `key=value` line in a job's output, empty when absent.
  std::string scriptValue(const ScriptJob& job, std::string_view key) {
    const std::string_view out = job.out;
    std::size_t pos = 0;
    while (pos < out.size()) {
      const std::size_t end = std::min(out.find('\n', pos), out.size());
      const std::string_view line = out.substr(pos, end - pos);
      if (line.size() > key.size() && line.substr(0, key.size()) == key && line[key.size()] == '=') {
        std::string value(line.substr(key.size() + 1));
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
          value.pop_back();
        }
        return value;
      }
      pos = end + 1;
    }
    return {};
  }

  long long numberValue(const ScriptJob& job, std::string_view key, long long fallback) {
    const std::string text = scriptValue(job, key);
    long long value = fallback;
    if (!text.empty()) {
      std::from_chars(text.data(), text.data() + text.size(), value);
    }
    return value;
  }

  // Runs `script` on a worker thread and applies its output on the main loop; the callback keeps only the
  // shared job, so it is safe if the panel or tab is gone. Repeated requests within a minute are ignored unless forced.
  void runScriptJob(
      const std::shared_ptr<ScriptJob>& job, const char* script, bool force,
      std::chrono::seconds minAge = std::chrono::seconds{60}
  ) {
    const auto now = std::chrono::steady_clock::now();
    if (job->running || (!force && job->loaded && now - job->startedAt < minAge)) {
      return;
    }
    job->running = true;
    job->startedAt = now;
    const bool started = process::runAsync(
        std::vector<std::string>{"sh", "-c", script},
        process::RunCallbacks{
            .onExit =
                [job](process::RunResult result) {
                  const bool ok = !result.timedOut;
                  DeferredCall::callLater([job, out = std::move(result.out), ok]() mutable {
                    job->running = false;
                    if (ok) {
                      job->out = std::move(out);
                      job->loaded = true;
                    }
                    PanelManager::instance().refresh();
                  });
                },
        },
        process::RunOptions{.timeout = std::chrono::seconds{180}}
    );
    if (!started) {
      job->running = false;
    }
  }

  // "2h 5m ago", "1d 7h ago": the time since `seconds` ago, in its two largest units.
  std::string formatAgo(long long seconds) {
    if (seconds < 60) {
      return "just now";
    }
    const long long minutes = seconds / 60;
    if (minutes < 60) {
      return std::format("{}m ago", minutes);
    }
    const long long hours = minutes / 60;
    if (hours < 24) {
      return std::format("{}h {}m ago", hours, minutes % 60);
    }
    return std::format("{}d {}h ago", hours / 24, hours % 24);
  }

  // "now", "5m", "3h", "2d" for the right edge of inbox rows.
  std::string formatAgeShort(long long seconds) {
    if (seconds < 60) {
      return "now";
    }
    if (seconds < 3600) {
      return std::format("{}m", seconds / 60);
    }
    if (seconds < 86400) {
      return std::format("{}h", seconds / 3600);
    }
    return std::format("{}d", seconds / 86400);
  }

  std::string formatUptime(std::chrono::seconds up) {
    const auto total = up.count();
    const auto days = total / 86400;
    const auto hours = (total % 86400) / 3600;
    const auto minutes = (total % 3600) / 60;
    if (days > 0) {
      return std::format("{}d {}h {}m", days, hours, minutes);
    }
    if (hours > 0) {
      return std::format("{}h {}m", hours, minutes);
    }
    return std::format("{}m", minutes);
  }

  std::string kernelDelta(const std::string& running, const std::string& installed) {
    const auto base = [](const std::string& release) { return release.substr(0, release.find('-')); };
    if (base(running) != base(installed)) {
      return base(running) + " → " + base(installed);
    }
    return running + " → " + installed;
  }

  // One line of free text: control characters and runs of white space become one space.
  std::string oneLine(std::string_view text) {
    std::string out;
    bool space = true;
    for (const char c : text) {
      if (std::isspace(static_cast<unsigned char>(c)) != 0 || static_cast<unsigned char>(c) < 0x20) {
        if (!space) {
          out += ' ';
        }
        space = true;
      } else {
        out += c;
        space = false;
      }
    }
    while (!out.empty() && out.back() == ' ') {
      out.pop_back();
    }
    return out;
  }

  std::string firstLine(std::string_view text) {
    const std::size_t end = text.find('\n');
    return oneLine(text.substr(0, end));
  }

  // Why a command failed, for a detail line: the first line of its stderr (and, when asked, of its stdout), else
  // its exit code.
  std::string failureReason(const process::RunResult& result, bool tryStdout = false) {
    std::string why = firstLine(result.err);
    if (why.empty() && tryStdout) {
      why = firstLine(result.out);
    }
    return why.empty() ? std::format("exit {}", result.exitCode) : why;
  }

  // "foo, bar +2": the first names that fit, then how many more.
  std::string unitList(std::string_view csv) {
    std::vector<std::string> names;
    std::size_t pos = 0;
    while (pos <= csv.size()) {
      const std::size_t end = std::min(csv.find(',', pos), csv.size());
      if (end > pos) {
        names.emplace_back(csv.substr(pos, end - pos));
      }
      pos = end + 1;
    }
    std::string out;
    std::size_t shown = 0;
    for (const std::string& name : names) {
      const std::size_t next = out.size() + (out.empty() ? 0 : 2) + name.size();
      const std::size_t reserve = shown + 1 < names.size() ? 5 : 0; // room for " +NN"
      if (shown > 0 && next + reserve > 22) {
        break;
      }
      out += (out.empty() ? "" : ", ") + name;
      ++shown;
    }
    if (shown < names.size()) {
      out += std::format(" +{}", names.size() - shown);
    }
    return out;
  }

  // The dashboard's names for the toggles; anything else falls back to the shortcut's own lower-cased label.
  std::string toggleName(const Shortcut& shortcut) {
    const std::string_view id = shortcut.id();
    if (id == "keyboard_sounds") {
      return "keyboard sounds";
    }
    if (id == "notification") {
      return "do not disturb";
    }
    if (id == "caffeine") {
      return "keep awake";
    }
    if (id == "nightlight") {
      return "night light";
    }
    if (id == "mic_mute") {
      return "mic mute";
    }
    if (id == "shutdown_timer") {
      return "auto-off";
    }
    return StringUtils::toLower(shortcut.displayLabel());
  }

  std::string networkOutputs(long long count) {
    return std::format("{} {}", count, count == 1 ? "output" : "outputs");
  }

  // Shows or hides a block that takes part in layout; true when its state changed.
  bool setShown(Node* node, bool shown) {
    if (node == nullptr || node->visible() == shown) {
      return false;
    }
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
    return true;
  }

  // One thing a tile may show: its node, the lines it takes and whether there is anything to show for it.
  struct FitItem {
    Node* node;
    int lines;
    bool wanted;
  };

  // Shows the wanted items in priority order for as long as they fit `capacity` lines and hides the rest; true when
  // any visibility changed. A tile keeps its size, so what does not fit is left out instead of being clipped.
  bool fitItems(const std::vector<FitItem>& byPriority, int capacity) {
    int used = 0;
    bool changed = false;
    for (const FitItem& item : byPriority) {
      const bool show = item.wanted && used + item.lines <= capacity;
      used += show ? item.lines : 0;
      changed |= setShown(item.node, show);
    }
    return changed;
  }

  std::unique_ptr<Label> dimCaption(Label** out, float scale, std::string text = {}) {
    return ui::label({
        .out = out,
        .text = std::move(text),
        .fontSize = term_frame::kBtopFontSize * scale,
        .fontFamily = std::string(term_frame::kFont),
        .color = term_frame::dimColor(),
        .maxLines = 1,
    });
  }

  // Sets a toggle row's name, only when it changed.
  void setRowName(term_frame::ToggleRow& row, const std::string& name) {
    if (row.button != nullptr && (row.button->label() == nullptr || row.button->label()->text() != name)) {
      row.button->setText(name);
    }
  }

  // Colours a toggle row's name and state alike.
  void setRowTone(term_frame::ToggleRow& row, Tone tone) {
    if (row.state != nullptr) {
      row.state->setColor(term_frame::toneColor(tone));
    }
    if (row.button != nullptr && row.button->label() != nullptr) {
      row.button->label()->setColor(term_frame::toneColor(tone));
    }
  }

  // Adds a pool of hidden, unnamed toggle rows; `click(i)` makes the handler of row i. The sync functions name and
  // show the rows that are wanted.
  template <std::size_t N, typename Click>
  void addHiddenRows(
      Flex& box, float scale, std::array<term_frame::ToggleRow, N>& rows, std::vector<Flex*>& fixedRows, Click click
  ) {
    for (std::size_t i = 0; i < N; ++i) {
      box.addChild(term_frame::toggleRow("", scale, rows[i], click(i), nullptr));
      fixedRows.push_back(rows[i].button);
      setShown(rows[i].button, false);
    }
  }

} // namespace

HomeTab::HomeTab(const ControlCenterServices& services)
    : m_config(services.config), m_services(services.shortcutServices()), m_notifications(services.notifications) {
  m_accounts = services.accounts;
  m_asyncTextures = services.asyncTextures;
  m_calendar = services.calendar;
  m_tasks = services.tasks;
  std::error_code error;
  if (!std::filesystem::exists(journalSeenPath(), error)) {
    writeJournalSeen(); // the first look: nothing is new yet
  }
  // Package upkeep is checked every 30 minutes (and when the dashboard opens), not continuously.
  m_upkeepTimer.startRepeating(std::chrono::minutes{30}, [this]() { refreshUpkeep(true); });
  loadTimerToday();
  // The last known fleet values, so the section is never empty while the first check runs.
  if (std::ifstream in(fleetPath()); in) {
    long long at = 0;
    in >> at;
    in.ignore(1, '\n');
    std::string rest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (at > 0 && !rest.empty()) {
      m_fleetAt = at;
      m_fleetJob->out = std::move(rest);
      m_fleetJob->loaded = true;
    }
  }
}

HomeTab::~HomeTab() = default;

float HomeTab::naturalHeight() const { return m_rootLayout != nullptr ? m_rootLayout->height() : 0.0F; }

// How many lines a visible wrapped detail label takes at the tile's width (0 when hidden), for the row budget.
int HomeTab::detailLines(const Label* label) const {
  if (label == nullptr || !label->visible()) {
    return 0;
  }
  const float charWidth = term_frame::monoWidth(1, term_frame::kBtopFontSize * contentScale());
  const int perLine = std::max(1, static_cast<int>(m_innerWidth / charWidth));
  const int chars = static_cast<int>(label->text().size());
  return std::clamp((chars + perLine - 1) / perLine, 1, 3);
}

void HomeTab::relayout() {
  if (m_relayoutQueued->exchange(true)) {
    return;
  }
  DeferredCall::callLater([queued = m_relayoutQueued]() {
    queued->store(false);
    PanelManager::instance().requestLayout();
    PanelManager::instance().requestUpdateOnly();
  });
}

std::unique_ptr<Flex> HomeTab::create() {
  const float scale = contentScale();
  const float gap = Style::spaceSm * scale;

  m_fixedRows.clear();
  m_detailLabels.clear();
  m_hotkeys.clear();

  // The grid: three rows of two tiles that share their top and bottom edges, then one strip for the session keys.
  // A margin on the left keeps the left tiles off the screen edge; it is as wide as the gutters between tiles.
  auto tab = ui::row({.out = &m_rootLayout, .align = FlexAlign::Start, .gap = 0.0F});
  tab->addChild(ui::column({.out = &m_margin}));
  auto grid = ui::column({.out = &m_grid, .align = FlexAlign::Stretch, .gap = gap});
  const auto addRow = [&](std::size_t index, std::unique_ptr<Flex> left, std::unique_ptr<Flex> right) {
    left->setClipChildren(true);
    right->setClipChildren(true);
    auto row = ui::row({.out = &m_gridRows[index], .align = FlexAlign::Stretch, .gap = gap});
    row->addChild(std::move(left));
    row->addChild(std::move(right));
    grid->addChild(std::move(row));
  };
  addRow(0, createHostBox(scale), createTogglesBox(scale));
  addRow(1, createUpkeepBox(scale), createTodayBox(scale));
  addRow(2, createInboxBox(scale), createFleetBox(scale));
  auto strip = createSystemBox(scale);
  strip->setClipChildren(true);
  grid->addChild(std::move(strip));
  tab->addChild(std::move(grid));

  // Every keyword, so the hotkey letters and the palette refresh reach them. Gates keep hidden ones quiet.
  const auto reg = [this](term_frame::Keyword& key, Flex* gate = nullptr) { m_hotkeys.push_back({&key, gate}); };
  reg(m_infoKey.key);
  reg(m_failedRow.key, m_failedRow.group);
  reg(m_journalRow.key, m_journalRow.group);
  reg(m_updateRow.key, m_updateRow.group);
  reg(m_orphansRow.key, m_orphansRow.group);
  reg(m_cacheRow.key, m_cacheRow.group);
  reg(m_timerStartKey, m_timerIdleRow);
  for (auto& key : m_timerPresetKeys) {
    reg(key, m_timerIdleRow);
  }
  for (auto& key : m_timerControlKeys) {
    reg(key, m_timerRunBox);
  }
  for (auto& key : m_powerKeys) {
    reg(key);
  }

  syncTimer();
  m_keywordPaletteConn = paletteChanged().connect([this]() {
    for (auto& entry : m_hotkeys) {
      term_frame::refreshKeyword(*entry.key);
    }
    syncTimer();
  });
  return tab;
}

// ---- 1 host -----------------------------------------------------------------------------------------------------

std::unique_ptr<Flex> HomeTab::createHostBox(float scale) {
  auto box = term_frame::btopBox("¹", "host", scale, &m_hostFrame);
  const float avatarSize = 2.0F * kHomeRowHeight * scale + Style::spaceXs * scale;
  const float avatarInner = std::max(1.0F, avatarSize - 2.0F * Style::borderWidth);
  m_avatarSize = avatarSize;

  auto avatarFrame = ui::column({
      .out = &m_avatarFrame,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .radius = 0.0F,
      .border = colorSpecFromRole(ColorRole::Outline),
      .borderWidth = Style::borderWidth,
      .clipChildren = true,
      .width = avatarSize,
      .height = avatarSize,
  });
  avatarFrame->addChild(
      ui::image({
          .out = &m_avatar,
          .fit = ImageFit::Cover,
          .radius = 0.0F,
          .width = avatarInner,
          .height = avatarInner,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  std::string initial = "?";
  if (const std::string name = sessionDisplayName(); !name.empty() && static_cast<unsigned char>(name[0]) < 0x80) {
    initial = std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(name[0]))));
  }
  avatarFrame->addChild(
      ui::label({
          .out = &m_avatarInitial,
          .text = initial,
          .fontSize = Style::fontSizeBody * scale,
          .fontFamily = std::string(term_frame::kFont),
          .color = colorSpecFromRole(ColorRole::Primary),
          .maxLines = 1,
      })
  );
  // The hint wraps onto two lines to fit the small square.
  avatarFrame->addChild(
      ui::label({
          .out = &m_avatarHint,
          .text = "click to set",
          .fontSize = Style::fontSizeCaption * scale * 0.85F,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
          .maxWidth = avatarInner,
          .maxLines = 2,
          .textAlign = TextAlign::Center,
      })
  );
  // Left click picks a new picture, right click clears it; hover shows an accent frame.
  auto avatarArea = ui::inputArea(
      {
          .out = &m_avatarArea,
          .acceptedButtons = InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}),
          .tooltip = std::string("choose picture"),
          .width = avatarSize,
          .height = avatarSize,
          .onEnter =
              [this](const InputArea::PointerData&) {
                if (m_avatarFrame != nullptr) {
                  m_avatarFrame->setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
                }
              },
          .onLeave =
              [this]() {
                if (m_avatarFrame != nullptr) {
                  m_avatarFrame->setBorder(term_frame::lineColor(), Style::borderWidth);
                }
              },
          .onClick =
              [this](const InputArea::PointerData& data) {
                if (data.button == BTN_RIGHT) {
                  clearAvatar();
                } else if (data.button == BTN_LEFT) {
                  chooseAvatar();
                }
              },
      },
      std::move(avatarFrame)
  );

  // Beside the avatar: the user name over the dim @host.
  auto text = ui::column(
      {
          .out = &m_hostText,
          .align = FlexAlign::Stretch,
          .justify = FlexJustify::Center,
          .gap = 0.0F,
          .flexGrow = 1.0F,
      },
      ui::label({
          .out = &m_hostUser,
          .text = sessionUserName(),
          .fontSize = term_frame::kBtopFontSize * scale,
          .fontWeight = FontWeight::Bold,
          .fontFamily = std::string(term_frame::kFont),
          .color = colorSpecFromRole(ColorRole::Primary),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      }),
      ui::label({
          .out = &m_hostName,
          .text = "@" + hostName(),
          .fontSize = term_frame::kBtopFontSize * scale,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      })
  );
  box->addChild(
      ui::row(
          {.out = &m_hostHeader, .align = FlexAlign::Center, .gap = Style::spaceSm * scale}, std::move(avatarArea),
          std::move(text)
      )
  );

  // The live rows and the static facts take turns in the same place, so opening `i` costs only the difference.
  auto liveBox = ui::column({.out = &m_hostLiveBox, .align = FlexAlign::Stretch, .gap = Style::spaceXs * scale});
  constexpr std::array<std::string_view, kHostLiveRows> live{"up", "kernel", "session"};
  for (std::size_t i = 0; i < live.size(); ++i) {
    liveBox->addChild(term_frame::infoRow(live[i], scale, m_hostLive[i]));
    m_fixedRows.push_back(m_hostLive[i].row);
  }
  box->addChild(std::move(liveBox));

  box->addChild(term_frame::actionRow("info", 0, scale, m_infoKey, [this]() {
    m_infoOpen = !m_infoOpen;
    syncHost();
    relayout();
  }));
  m_fixedRows.push_back(m_infoKey.row);

  auto info = ui::column({.out = &m_hostInfoBox, .align = FlexAlign::Stretch, .gap = Style::spaceXs * scale});
  constexpr std::array<std::string_view, kHostInfoRows> infoKeys{"os", "shell", "wm", "pkgs", "gpu", "display"};
  for (std::size_t i = 0; i < infoKeys.size(); ++i) {
    info->addChild(term_frame::infoRow(infoKeys[i], scale, m_hostInfoRows[i]));
    m_fixedRows.push_back(m_hostInfoRows[i].row);
  }
  info->setVisible(false);
  info->setParticipatesInLayout(false);
  box->addChild(std::move(info));

  syncHost();
  return box;
}

void HomeTab::syncHost() {
  if (m_hostFrame == nullptr) {
    return;
  }
  if (!m_systemInfoLoaded) {
    m_osLabel = distroLabel();
    m_gpuLabel = gpuLabel();
    m_systemInfoLoaded = true;
  }
  const ScriptJob& job = *m_liveJob;
  const auto fact = [&job](std::string_view key) {
    std::string value = scriptValue(job, key);
    return value.empty() ? std::string{"--"} : value;
  };

  const auto up = systemUptime();
  term_frame::setLabelTone(m_hostLive[0].value, up.has_value() ? formatUptime(*up) : "n/a", Tone::Normal);

  const std::string running = scriptValue(job, "kernel_run");
  const std::string installed = scriptValue(job, "kernel_inst");
  const bool kernelDiffers = !running.empty() && installed != running;
  // A leading "!" says "warning" without relying on the colour.
  if (m_hostLive[1].key != nullptr && m_hostLive[1].key->text() != (kernelDiffers ? "! kernel" : "kernel")) {
    m_hostLive[1].key->setText(kernelDiffers ? "! kernel" : "kernel");
  }
  if (running.empty()) {
    term_frame::setLabelTone(m_hostLive[1].value, kernelLabel(), Tone::Normal);
  } else if (kernelDiffers) {
    term_frame::setLabelTone(m_hostLive[1].value, kernelDelta(running, installed), Tone::Warning);
  } else {
    term_frame::setLabelTone(m_hostLive[1].value, running, Tone::Normal);
  }

  const std::string wm = scriptValue(job, "wm");
  const long long outputs = numberValue(job, "outputs", -1);
  term_frame::setLabelTone(
      m_hostLive[2].value, wm.empty() || outputs < 0 ? std::string{"--"} : std::format("{} · {}", wm, networkOutputs(outputs)),
      Tone::Normal
  );

  const std::array<std::string, kHostInfoRows> values{
      m_osLabel, fact("shell"), fact("wm"), fact("packages"), m_gpuLabel, fact("display")};
  for (std::size_t i = 0; i < kHostInfoRows; ++i) {
    term_frame::setLabelTone(m_hostInfoRows[i].value, values[i], Tone::Normal);
  }

  // The tile never changes height: the rows that fit are shown, by priority. Open, the static facts replace the
  // live rows and the avatar; the key row keeps its line.
  bool shapeChanged = setShown(m_hostInfoBox, m_infoOpen);
  shapeChanged |= setShown(m_hostLiveBox, !m_infoOpen);
  std::vector<FitItem> items;
  if (m_infoOpen) {
    for (const std::size_t i : std::array<std::size_t, 6>{0, 4, 5, 3, 1, 2}) { // os, gpu, display, pkgs, shell, wm
      items.push_back({m_hostInfoRows[i].row, 1, true});
    }
    items.push_back({m_hostHeader, 2, false});
  } else {
    items.push_back({m_hostHeader, 2, true});
    for (const std::size_t i : std::array<std::size_t, 3>{1, 2, 0}) { // kernel, session, up
      items.push_back({m_hostLive[i].row, 1, true});
    }
  }
  shapeChanged |= fitItems(items, m_capacity[TileHost] - 1); // the key row takes one line
  if (shapeChanged) {
    relayout();
  }
  if (m_infoKey.key.block != m_infoOpen) {
    m_infoKey.key.block = m_infoOpen;
    m_infoKey.key.blockText = m_infoKey.key.word;
    term_frame::refreshKeyword(m_infoKey.key);
  }
}

// ---- 3 upkeep ---------------------------------------------------------------------------------------------------

std::unique_ptr<Flex> HomeTab::createUpkeepBox(float scale) {
  auto box = term_frame::btopBox("³", "upkeep", scale, &m_upkeepFrame);
  const auto hidden = [](Flex* node) {
    node->setVisible(false);
    node->setParticipatesInLayout(false);
  };
  const auto leave = [this](term_frame::Keyword& key) {
    return [this, &key]() {
      if (m_armed == &key) {
        clearArmed();
      }
    };
  };

  // Warnings first.
  box->addChild(term_frame::infoRow("! reboot", scale, m_rebootRow));
  box->addChild(term_frame::infoRow("! root", scale, m_rootRow));
  for (term_frame::InfoRow* row : {&m_rebootRow, &m_rootRow}) {
    row->key->setColor(term_frame::toneColor(Tone::Warning));
    hidden(row->row);
    m_fixedRows.push_back(row->row);
  }
  box->addChild(term_frame::actionRow(
      "! failed", 2, scale, m_failedRow,
      [this]() {
        launchInTerminal("failed", "systemctl", "systemctl --failed --no-pager; systemctl --user --failed --no-pager");
      },
      nullptr
  ));
  box->addChild(term_frame::actionRow(
      "! journal", 2, scale, m_journalRow,
      [this]() {
        markJournalSeen();
        launchInTerminal("journal", "journalctl", "journalctl -b -p err");
      },
      nullptr
  ));
  for (term_frame::ActionRow* row : {&m_failedRow, &m_journalRow}) {
    hidden(row->group);
    m_fixedRows.push_back(row->row);
    m_detailLabels.push_back(row->detail);
  }

  box->addChild(dimCaption(&m_upkeepSummary, scale));

  // Housekeeping: each key shows what it will do and asks before it runs.
  box->addChild(term_frame::actionRow(
      "update", 0, scale, m_updateRow,
      [this]() {
        pressConfirmed(m_updateRow.key, &m_updateRow, "runs yay -Syu in a terminal", "", [this]() {
          launchInTerminal("update", "yay", "yay -Syu");
        });
      },
      leave(m_updateRow.key)
  ));
  box->addChild(term_frame::actionRow(
      "orphans", 0, scale, m_orphansRow,
      [this]() {
        const int count = static_cast<int>(numberValue(*m_upkeepJob, "orphans", -1));
        if (count <= 0) {
          clearArmed();
          if (term_frame::setActionDetail(m_orphansRow, count == 0 ? "nothing to remove" : "not checked yet", Tone::Dim)) {
            relayout();
          }
          return;
        }
        pressConfirmed(
            m_orphansRow.key, &m_orphansRow,
            std::format("pacman -Rns removes {} orphans; it lists them and asks first", count), "",
            [this]() { launchInTerminal("orphans", "pacman -Rns", "sudo pacman -Rns $(pacman -Qdtq)"); }
        );
      },
      leave(m_orphansRow.key)
  ));
  box->addChild(term_frame::actionRow(
      "cache", 0, scale, m_cacheRow,
      [this]() {
        pressConfirmed(
            m_cacheRow.key, &m_cacheRow, "paccache -rv keeps three versions of each package and deletes the rest", "",
            [this]() { launchInTerminal("cache", "paccache", "sudo paccache -rv"); }
        );
      },
      leave(m_cacheRow.key)
  ));
  for (term_frame::ActionRow* row : {&m_updateRow, &m_orphansRow, &m_cacheRow}) {
    m_fixedRows.push_back(row->row);
    m_detailLabels.push_back(row->detail);
  }
  box->addChild(dimCaption(&m_lastUpdate, scale));
  return box;
}

void HomeTab::syncUpkeep() {
  if (m_upkeepFrame == nullptr) {
    return;
  }
  const ScriptJob& live = *m_liveJob;
  const ScriptJob& slow = *m_upkeepJob;
  bool changed = false;

  const std::string running = scriptValue(live, "kernel_run");
  const std::string installed = scriptValue(live, "kernel_inst");
  const bool rebootNeeded = live.loaded && (scriptValue(live, "reboot") == "1" || (!running.empty() && running != installed));
  const health::Level rootLevel = health::disk("root", m_rootUsed * 100.0);
  const std::string failed = scriptValue(live, "failed");
  const int failedCount = failed.empty() ? 0 : static_cast<int>(std::count(failed.begin(), failed.end(), ',')) + 1;
  const long long journalTotal = numberValue(live, "journal_total", 0);
  // A check that started before the journal was opened still counts those errors as new; ignore it.
  const bool stale = live.startedAt < m_journalMarkedAt;
  const long long journalNew = stale ? 0 : numberValue(live, "journal_new", 0);

  // A warning row stays while it shows why its last action failed, even when the warning itself has cleared.
  const bool failedWarn = live.loaded && failedCount > 0;
  const bool journalWarn = live.loaded && journalNew > 0;
  const bool failedNote = m_failedRow.detail != nullptr && m_failedRow.detail->visible();
  const bool journalNote = m_journalRow.detail != nullptr && m_journalRow.detail->visible();
  const bool showReboot = rebootNeeded;
  const bool showRoot = rootLevel != health::Level::Ok;
  const bool showFailed = failedWarn || failedNote;
  const bool showJournal = journalWarn || journalNote;

  if (showReboot) {
    term_frame::setLabelTone(m_rebootRow.value, "needed", Tone::Warning);
  }
  if (showRoot) {
    term_frame::setLabelTone(
        m_rootRow.value, std::format("{:.0f}% used", m_rootUsed * 100.0),
        rootLevel == health::Level::Error ? Tone::Error : Tone::Warning
    );
  }
  if (showFailed) {
    term_frame::setActionValue(m_failedRow, failedWarn ? unitList(failed) : "none now", failedWarn ? Tone::Warning : Tone::Dim);
  }
  if (showJournal) {
    term_frame::setActionValue(
        m_journalRow, std::format("{} new · {} total", journalNew, journalTotal), journalWarn ? Tone::Warning : Tone::Dim
    );
  }

  // What is fine collapses into one dim line.
  std::vector<std::string> fine;
  if (live.loaded && failedCount == 0) {
    fine.emplace_back("failed 0");
  }
  if (live.loaded && !journalWarn) {
    fine.push_back(std::format("errors {} seen", journalTotal));
  }
  std::string summary;
  for (const std::string& part : fine) {
    summary += (summary.empty() ? "" : " · ") + part;
  }
  term_frame::setLabelTone(m_upkeepSummary, summary, Tone::Dim);

  // Housekeeping values.
  std::string updates = m_upkeepJob->running ? "checking…" : "--";
  Tone updateTone = Tone::Dim;
  if (slow.loaded) {
    const long long repo = std::max(0LL, numberValue(slow, "repo", 0));
    const long long aur = std::max(0LL, numberValue(slow, "aur", 0));
    if (repo + aur == 0) {
      updates = "up to date";
    } else {
      updates = repo > 0 ? std::format("{} repo", repo) : std::string{};
      if (aur > 0) {
        updates += (updates.empty() ? "" : " · ") + std::format("{} aur", aur);
      }
      updateTone = Tone::Normal;
    }
  }
  term_frame::setActionValue(m_updateRow, updates, updateTone);

  const long long orphans = numberValue(slow, "orphans", -1);
  term_frame::setActionValue(
      m_orphansRow, !slow.loaded || orphans < 0 ? "--" : (orphans == 0 ? "none" : std::to_string(orphans)),
      orphans > 0 ? Tone::Normal : Tone::Dim
  );

  std::string cache = "--";
  const long long cacheBytes = numberValue(slow, "cache", -1);
  if (slow.loaded && cacheBytes >= 0) {
    const double gib = static_cast<double>(cacheBytes) / (1024.0 * 1024.0 * 1024.0);
    cache = gib >= 1.0 ? std::format("{:.1f} GiB", gib) : std::format("{:.0f} MiB", gib * 1024.0);
  }
  term_frame::setActionValue(m_cacheRow, cache, Tone::Normal);

  const long long last = numberValue(slow, "last", 0);
  const std::string lastText = last > 0 ? "updated " + formatAgo(std::time(nullptr) - static_cast<std::time_t>(last)) : "";
  term_frame::setLabelTone(m_lastUpdate, lastText, Tone::Dim);

  // Header slot: how many of the warnings need action.
  const int actions = (showReboot ? 1 : 0) + (showRoot ? 1 : 0) + (failedWarn ? 1 : 0) + (journalWarn ? 1 : 0);
  if (!live.loaded) {
    m_upkeepFrame->setInfo("checking…");
    m_upkeepFrame->setInfoColor(colorSpecFromRole(ColorRole::OnSurface));
  } else if (actions == 0) {
    m_upkeepFrame->setInfo("all clear");
    m_upkeepFrame->setInfoColor(colorSpecFromRole(ColorRole::OnSurface));
  } else {
    m_upkeepFrame->setInfo(std::format("! {} {} action", actions, actions == 1 ? "needs" : "need"));
    m_upkeepFrame->setInfoColor(colorSpecFromRole(ColorRole::Warning));
  }
  // Warnings first, then the housekeeping keys, then the dim lines; what does not fit is left out. An armed or failed
  // row takes its detail lines with it.
  const auto group = [this](const term_frame::ActionRow& row) { return 1 + detailLines(row.detail); };
  const std::vector<FitItem> items{
      {m_rebootRow.row, 1, showReboot},
      {m_rootRow.row, 1, showRoot},
      {m_failedRow.group, group(m_failedRow), showFailed},
      {m_journalRow.group, group(m_journalRow), showJournal},
      {m_updateRow.group, group(m_updateRow), true},
      {m_orphansRow.group, group(m_orphansRow), true},
      {m_cacheRow.group, group(m_cacheRow), true},
      {m_upkeepSummary, 1, !summary.empty()},
      {m_lastUpdate, 1, !lastText.empty()},
  };
  changed |= fitItems(items, m_capacity[TileUpkeep]);
  if (changed) {
    relayout();
  }
}

void HomeTab::markJournalSeen() {
  writeJournalSeen();
  m_journalMarkedAt = std::chrono::steady_clock::now();
  refreshLive(true);
}

// Both checks run in one shell command each on a worker thread and are applied on the main loop. Opening the
// dashboard repeatedly within a minute does not start another check.
void HomeTab::refreshLive(bool force) {
  struct statvfs root{};
  if (statvfs("/", &root) == 0 && root.f_blocks > 0) {
    m_rootUsed = 1.0 - static_cast<double>(root.f_bavail) / static_cast<double>(root.f_blocks);
  }
  runScriptJob(m_liveJob, kLiveScript, force);
}

void HomeTab::refreshUpkeep(bool force) {
  refreshLive(force);
  runScriptJob(m_upkeepJob, kUpkeepScript, force);
}

// ---- 5 inbox ----------------------------------------------------------------------------------------------------

std::unique_ptr<Flex> HomeTab::createInboxBox(float scale) {
  auto box = term_frame::btopBox("⁵", "inbox", scale, &m_inboxFrame);
  addHiddenRows(*box, scale, m_noteRows, m_fixedRows, [this](std::size_t i) {
    return [this, i]() { activateNote(i); };
  });
  addHiddenRows(*box, scale, m_clipRows, m_fixedRows, [this](std::size_t i) {
    return [this, i]() { copyClipboardEntry(i); };
  });
  box->addChild(ui::label({
      .out = &m_clipFull,
      .fontSize = term_frame::kBtopFontSize * scale,
      .fontFamily = std::string(term_frame::kFont),
      .color = term_frame::dimColor(),
      .maxLines = 3,
      .visible = false,
      .participatesInLayout = false,
  }));
  m_detailLabels.push_back(m_clipFull);
  box->addChild(ui::label({
      .out = &m_inboxDetail,
      .fontSize = term_frame::kBtopFontSize * scale,
      .fontFamily = std::string(term_frame::kFont),
      .color = term_frame::toneColor(Tone::Error),
      .maxLines = 3,
      .visible = false,
      .participatesInLayout = false,
  }));
  m_detailLabels.push_back(m_inboxDetail);
  syncInbox();
  return box;
}

void HomeTab::syncInbox() {
  if (m_inboxFrame == nullptr) {
    return;
  }
  bool changed = false;
  const auto now = std::chrono::system_clock::now();

  // Newest unread first: the bar's own history (its newest entry is at the back), then what mako still shows.
  struct NoteLine {
    std::string title;
    std::string age;
    std::string makoId; // empty for the bar's own notifications
  };
  std::vector<NoteLine> lines;
  std::size_t unread = 0;
  if (m_notifications != nullptr) {
    const auto& history = m_notifications->history();
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
      if (it->seen) {
        continue;
      }
      ++unread;
      if (lines.size() < kInboxNotes) {
        const Notification& note = it->notification;
        NoteLine line;
        const std::string headline = note.summary.empty() ? note.body : note.summary;
        line.title = oneLine(note.appName.empty() ? headline : note.appName + ": " + headline);
        if (note.receivedWallClock.has_value()) {
          line.age =
              formatAgeShort(std::chrono::duration_cast<std::chrono::seconds>(now - *note.receivedWallClock).count());
        }
        lines.push_back(std::move(line));
      }
    }
  }
  const long long makoCount = numberValue(*m_notesJob, "count", 0);
  unread += static_cast<std::size_t>(std::max(0LL, makoCount));
  for (int n = 1; n <= static_cast<int>(kInboxNotes) && lines.size() < kInboxNotes && n <= makoCount; ++n) {
    const std::string entry = scriptValue(*m_notesJob, std::format("note_{}", n));
    const std::size_t tab = entry.find('\t');
    if (tab == std::string::npos) {
      continue;
    }
    lines.push_back({.title = oneLine(entry.substr(tab + 1)), .age = "dismiss", .makoId = entry.substr(0, tab)});
  }
  std::array<bool, kInboxNotes> noteWanted{};
  for (std::size_t i = 0; i < kInboxNotes; ++i) {
    term_frame::ToggleRow& row = m_noteRows[i];
    noteWanted[i] = i < lines.size();
    if (!noteWanted[i] || row.button == nullptr) {
      m_noteMako[i].clear();
      continue;
    }
    m_noteMako[i] = lines[i].makoId;
    const std::string shown = "● " + lines[i].title; // ● marks a notification
    setRowName(row, shown);
    term_frame::setToggleRow(row, true, false, lines[i].age);
  }
  m_inboxFrame->setInfo(std::format("{} unread", unread));

  // The last clipboard entries, newest first.
  const InputArea* focused = inputDispatcher() != nullptr ? inputDispatcher()->focusedArea() : nullptr;
  std::string fullText; // the whole text of the focused clipboard row
  std::array<bool, kInboxClips> clipWanted{};
  if (m_services.clipboard != nullptr) {
    const auto& history = m_services.clipboard->history();
    for (std::size_t i = 0; i < kInboxClips; ++i) {
      term_frame::ToggleRow& row = m_clipRows[i];
      clipWanted[i] = i < history.size();
      if (!clipWanted[i] || row.button == nullptr) {
        continue;
      }
      const ClipboardEntry& entry = history[i];
      // Entries that look like secrets are masked; their text shows only while their row has the focus.
      const bool secret = !entry.isImage() && secret_shape::looksSensitive(entry.textPreview);
      std::string title = entry.isImage() ? std::string{"[image]"} : oneLine(entry.textPreview);
      if (secret) {
        title = std::string(secret_shape::kMaskedPreview);
      } else if (title.empty()) {
        title = "[data]";
      }
      const std::string shown = "▸ " + title; // ▸ marks a clipboard entry
      setRowName(row, shown);
      std::string age;
      if (entry.capturedAt.time_since_epoch().count() > 0) {
        age = formatAgeShort(std::chrono::duration_cast<std::chrono::seconds>(now - entry.capturedAt).count());
      }
      term_frame::setToggleRow(row, true, false, age);
      if (focused != nullptr && row.button->inputArea() == focused && !entry.isImage()) {
        fullText = oneLine(entry.textPreview).substr(0, 240);
      }
    }
  }
  term_frame::setLabelTone(m_clipFull, fullText, Tone::Dim);

  // Notifications first, then clipboard entries, as many as the tile's height holds; an error line takes its two
  // lines first.
  const int capacity = m_capacity[TileInbox] - (m_inboxDetail != nullptr && m_inboxDetail->visible() ? 2 : 0);
  std::vector<FitItem> items;
  for (std::size_t i = 0; i < kInboxNotes; ++i) {
    items.push_back({m_noteRows[i].button, 1, noteWanted[i]});
  }
  for (std::size_t i = 0; i < kInboxClips; ++i) {
    items.push_back({m_clipRows[i].button, 1, clipWanted[i]});
  }
  items.push_back({m_clipFull, 2, !fullText.empty()});
  changed |= fitItems(items, capacity);
  if (changed) {
    relayout();
  }
}

// Enter on an unread row: the bar's own notifications open the notifications tab; one of mako's is dismissed.
void HomeTab::activateNote(std::size_t slot) {
  if (slot >= kInboxNotes || m_noteMako[slot].empty()) {
    openControlCenterTab("notifications");
    return;
  }
  const bool started = process::runAsync(
      std::vector<std::string>{"makoctl", "dismiss", "-n", m_noteMako[slot]},
      process::RunCallbacks{
          .onExit =
              [mailbox = m_mailbox](process::RunResult result) {
                const std::string why = result.exitCode != 0 ? failureReason(result) : std::string{};
                DeferredCall::callLater([mailbox, why = std::move(why)]() {
                  mailbox->done.push_back({.id = "inbox", .exitCode = why.empty() ? 0 : 1, .error = why});
                  PanelManager::instance().refresh();
                });
              },
      }
  );
  if (!started) {
    m_mailbox->done.push_back({.id = "inbox", .exitCode = -1, .error = "could not run makoctl"});
  }
}

void HomeTab::copyClipboardEntry(std::size_t slot) {
  const auto fail = [this](std::string_view text) {
    term_frame::setLabelTone(m_inboxDetail, text, Tone::Error);
    if (setShown(m_inboxDetail, true)) {
      relayout();
    }
  };
  ClipboardService* clipboard = m_services.clipboard;
  if (clipboard == nullptr) {
    fail("copy failed: no clipboard service");
    return;
  }
  if (slot >= clipboard->history().size()) {
    fail("copy failed: the entry is gone");
    return;
  }
  if (!clipboard->ensureEntryLoaded(slot)) {
    fail("copy failed: its data could not be read");
    return;
  }
  const ClipboardEntry entry = clipboard->history()[slot];
  if (!clipboard->copyEntry(entry)) {
    fail("copy failed: the compositor refused the selection");
    return;
  }
  if (setShown(m_inboxDetail, false)) {
    relayout();
  }
}

// ---- 2 toggles --------------------------------------------------------------------------------------------------

// The configured control-center shortcuts as btop rows, name on the left and state on the right (an accent block
// when on; auto-off shows the time left).
std::unique_ptr<Flex> HomeTab::createTogglesBox(float scale) {
  auto box = term_frame::btopBox("²", "toggles", scale, &m_togglesFrame);
  const auto& shortcuts =
      m_config != nullptr ? m_config->config().controlCenter.shortcuts : std::vector<ShortcutConfig>{};
  const std::size_t count = std::min(shortcuts.size(), std::size_t{6});

  auto list = ui::column({
      .out = &m_togglesList,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
  });
  // Keep Shortcut instances across open/close so plugin file watches and runtimes
  // are not recreated on every Control Center open. Match by id/type from config.
  std::vector<std::unique_ptr<Shortcut>> previous;
  previous.reserve(m_shortcutPads.size());
  for (auto& pad : m_shortcutPads) {
    previous.push_back(std::move(pad.shortcut));
  }
  m_shortcutPads.clear();

  auto takeReusable = [&previous](std::string_view type) -> std::unique_ptr<Shortcut> {
    for (auto it = previous.begin(); it != previous.end(); ++it) {
      if (*it != nullptr && (*it)->id() == type) {
        auto found = std::move(*it);
        previous.erase(it);
        return found;
      }
    }
    return nullptr;
  };

  // A plugin shortcut seeds its Luau runtime once, at construction. Plugin settings changed
  // since the last build means every reusable plugin instance is stale, so drop it and let
  // ShortcutRegistry::create() re-seed from the current config.
  const bool pluginsChanged = m_config != nullptr && !(m_config->config().plugins == m_lastPlugins);
  if (m_config != nullptr) {
    m_lastPlugins = m_config->config().plugins;
  }

  for (std::size_t i = 0; i < count; ++i) {
    const auto& sc = shortcuts[i];
    auto shortcut = takeReusable(sc.type);
    // Match on the id syntax, not the registry: a disabled plugin has already left the registry.
    if (shortcut != nullptr && pluginsChanged && scripting::isValidPluginEntryId(sc.type)) {
      shortcut.reset();
    }
    const bool reused = shortcut != nullptr;
    if (!reused) {
      shortcut = ShortcutRegistry::create(sc.type, m_services);
    }
    if (shortcut == nullptr) {
      continue;
    }
    if (reused) {
      shortcut->onPanelOpen();
    }

    const std::size_t padIdx = m_shortcutPads.size();
    ShortcutPad pad;
    auto row = term_frame::toggleRow(
        toggleName(*shortcut), scale, pad.row,
        [this, padIdx]() {
          if (padIdx < m_shortcutPads.size()) {
            m_shortcutPads[padIdx].shortcut->onClick();
          }
        },
        [this, padIdx]() {
          if (padIdx < m_shortcutPads.size()) {
            m_shortcutPads[padIdx].shortcut->onRightClick();
          }
        }
    );
    if (auto* ia = pad.row.button->inputArea(); ia != nullptr) {
      ia->setOnAxisHandler([this, padIdx](const InputArea::PointerData& data) -> bool {
        if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL || padIdx >= m_shortcutPads.size()) {
          return false;
        }
        const float steps = data.scrollSteps();
        if (steps == 0.0F) {
          return false;
        }
        // Scroll up moves forward (toward performance); Wayland reports up as a negative delta.
        m_shortcutPads[padIdx].shortcut->onScroll(steps > 0.0F ? -1 : 1);
        return true;
      });
    }
    m_fixedRows.push_back(pad.row.button);
    pad.shortcut = std::move(shortcut);
    m_shortcutPads.push_back(std::move(pad));
    list->addChild(std::move(row));
  }

  if (!m_shortcutPads.empty()) {
    box->addChild(std::move(list));
  } else {
    m_togglesList = nullptr;
  }
  return box;
}

void HomeTab::syncShortcuts() {
  for (auto& pad : m_shortcutPads) {
    syncShortcutPad(pad);
  }
  syncToggleCount();
}

// The header slot counts the toggles that are on, out of all the rows.
void HomeTab::syncToggleCount() {
  if (m_togglesFrame == nullptr) {
    return;
  }
  std::size_t active = 0;
  for (const auto& pad : m_shortcutPads) {
    if (pad.shortcut->isToggle()) {
      active += pad.shortcut->enabled() && pad.shortcut->active() ? 1 : 0;
    }
  }
  m_togglesFrame->setInfo(m_shortcutPads.empty() ? std::string{} : std::format("{}/{}", active, m_shortcutPads.size()));
}

// A short dim note after a toggle's state. Keep awake shows how long the idle inhibitor has been on. Night light
// shows its real schedule: "schedule off", or the next transition, which is always still ahead.
std::string HomeTab::toggleDetail(const Shortcut& shortcut, bool on) {
  const std::string_view id = shortcut.id();
  if (id == "caffeine") {
    if (!on || m_services.idleInhibitor == nullptr) {
      return {};
    }
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(
                             std::chrono::steady_clock::now() - m_services.idleInhibitor->enabledSince()
    ).count();
    return minutes >= 60 ? std::format("· for {}h {}m", minutes / 60, minutes % 60) : std::format("· for {}m", minutes);
  }
  if (id == "nightlight") {
    GammaService* gamma = m_services.nightLight;
    if (gamma == nullptr) {
      return {};
    }
    if (gamma->forceEnabled()) {
      return "· manual";
    }
    if (const auto next = gamma->nextTransition(); next.has_value()) {
      return std::format("· {} {}", next->toNight ? "on at" : "until", clockText(next->at));
    }
    return {}; // schedule off and not forced: the state alone says it
  }
  return {};
}

void HomeTab::syncShortcutPad(ShortcutPad& pad) {
  const Shortcut& shortcut = *pad.shortcut;
  const bool enabled = shortcut.enabled();
  const bool toggle = shortcut.isToggle();
  const bool on = enabled && toggle && shortcut.active();
  std::string text = !enabled ? "n/a" : (!toggle ? "set" : (on ? "on" : "off"));
  if (on) {
    if (std::string detail = shortcut.stateText(); !detail.empty()) {
      text = std::move(detail);
    }
  }
  term_frame::setToggleRow(pad.row, enabled, on, text, enabled && toggle ? toggleDetail(shortcut, on) : std::string{});
}

// ---- the timer: the first line of today -------------------------------------------------------------------------

// Idle it is one line, `05:00 ready start 5 10 25 45`. Running or paused, the large time sits beside the controls,
// two lines high. The session squares are in the tile's header slot.
void HomeTab::addTimerRows(Flex& box, float scale) {
  constexpr std::size_t kNoHotkey = std::string::npos;
  auto idle = ui::row({.out = &m_timerIdleRow, .align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  idle->addChild(
      ui::label({
          .out = &m_timerIdleTime,
          .text = "05:00",
          .fontSize = term_frame::kBtopFontSize * scale,
          .fontWeight = FontWeight::Bold,
          .fontFamily = std::string(term_frame::kFont),
          .color = colorSpecFromRole(ColorRole::Primary),
          .maxLines = 1,
      })
  );
  idle->addChild(dimCaption(&m_timerStatus, scale, "ready"));
  idle->addChild(term_frame::makeKeyword(m_timerStartKey, "start", 2, scale, [this]() { toggleTimer(); }));
  for (std::size_t i = 0; i < kTimerPresetMinutes.size(); ++i) {
    const int minutes = kTimerPresetMinutes[i];
    idle->addChild(
        term_frame::makeKeyword(
            m_timerPresetKeys[i], std::to_string(minutes), kNoHotkey, scale,
            [this, minutes]() { setTimerDuration(std::chrono::minutes{minutes}); }
        )
    );
  }
  m_fixedRows.push_back(m_timerIdleRow);
  box.addChild(std::move(idle));

  auto run = ui::row({.out = &m_timerRunBox, .align = FlexAlign::Center, .gap = Style::spaceMd * scale});
  run->addChild(
      ui::column(
          {.out = &m_timerTimeBlock,
           .align = FlexAlign::Center,
           .paddingV = Style::spaceXs * scale,
           .paddingH = Style::spaceSm * scale},
          ui::label({
              .out = &m_timerTime,
              .text = "00:00",
              .fontSize = kTimerFontSize * scale,
              .fontWeight = FontWeight::Bold,
              .fontFamily = std::string(term_frame::kFont),
              .color = colorSpecFromRole(ColorRole::Primary),
              .maxLines = 1,
          })
      )
  );
  auto controls = ui::column({.align = FlexAlign::Start, .gap = 0.0F});
  auto first = ui::row({.align = FlexAlign::Center, .gap = 0.0F});
  first->addChild(term_frame::makeKeyword(m_timerControlKeys[0], "pause", 1, scale, [this]() { toggleTimer(); }));
  first->addChild(term_frame::makeKeyword(m_timerControlKeys[1], "reset", 1, scale, [this]() { resetTimer(); }));
  auto second = ui::row({.align = FlexAlign::Center, .gap = 0.0F});
  second->addChild(
      term_frame::makeKeyword(
          m_timerControlKeys[2], "-1m", kNoHotkey, scale,
          [this]() { setTimerDuration(m_timerDuration - std::chrono::minutes{1}); }
      )
  );
  second->addChild(
      term_frame::makeKeyword(
          m_timerControlKeys[3], "+1m", kNoHotkey, scale,
          [this]() { setTimerDuration(m_timerDuration + std::chrono::minutes{1}); }
      )
  );
  controls->addChild(std::move(first));
  controls->addChild(std::move(second));
  run->addChild(std::move(controls));
  run->setVisible(false);
  run->setParticipatesInLayout(false);
  box.addChild(std::move(run));
  m_timerExpanded = false;
}

int HomeTab::timerGoal() const {
  const int goal = m_config != nullptr ? m_config->config().controlCenter.timerDailyGoal : 6;
  return std::clamp(goal, 1, 12);
}

std::chrono::milliseconds HomeTab::timerRemaining() const {
  if (m_timerState != TimerState::Running) {
    return m_timerRemaining;
  }
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(m_timerEnd - std::chrono::steady_clock::now());
  return std::max(left, std::chrono::milliseconds{0});
}

// Changing the length discards any run in progress.
void HomeTab::setTimerDuration(std::chrono::seconds duration) {
  m_timerDuration = std::clamp(duration, kTimerMin, kTimerMax);
  resetTimer();
}

void HomeTab::resetTimer() {
  m_timerTick.stop();
  m_timerState = TimerState::Idle;
  m_timerRemaining = std::chrono::duration_cast<std::chrono::milliseconds>(m_timerDuration);
  syncTimer();
  PanelManager::instance().requestRedraw();
}

void HomeTab::toggleTimer() {
  switch (m_timerState) {
  case TimerState::Running:
    m_timerRemaining = timerRemaining();
    m_timerTick.stop();
    m_timerState = TimerState::Paused;
    break;
  case TimerState::Done:
    m_timerRemaining = std::chrono::duration_cast<std::chrono::milliseconds>(m_timerDuration);
    [[fallthrough]];
  case TimerState::Idle:
  case TimerState::Paused:
    m_timerEnd = std::chrono::steady_clock::now() + m_timerRemaining;
    m_timerState = TimerState::Running;
    m_timerTick.startRepeating(kTimerTickInterval, [this]() { onTimerTick(); });
    break;
  }
  syncTimer();
  PanelManager::instance().requestRedraw();
}

void HomeTab::loadTimerToday() {
  m_timerDay = localDateKey();
  std::ifstream in(timerStatsPath());
  std::string day;
  int sessions = 0;
  long long seconds = 0;
  if (in >> day >> sessions >> seconds && day == m_timerDay) {
    m_timerSessions = sessions;
    m_timerSecondsToday = seconds;
  }
}

// A new local day starts today's sessions from zero.
void HomeTab::rollTimerDay() {
  const std::string today = localDateKey();
  if (today != m_timerDay) {
    m_timerDay = today;
    m_timerSessions = 0;
    m_timerSecondsToday = 0;
  }
}

// A finished countdown of at least kTimerSessionMinutes is one session; shorter ones do not fill a square.
void HomeTab::recordTimerSession() {
  if (m_timerDuration < std::chrono::minutes{ControlCenterConfig::kTimerSessionMinutes}) {
    return;
  }
  rollTimerDay();
  ++m_timerSessions;
  m_timerSecondsToday += m_timerDuration.count();
  const std::filesystem::path path = timerStatsPath();
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream(path, std::ios::trunc) << m_timerDay << ' ' << m_timerSessions << ' ' << m_timerSecondsToday << '\n';
}

void HomeTab::finishTimer() {
  m_timerTick.stop();
  recordTimerSession();
  m_timerState = TimerState::Done;
  m_timerRemaining = std::chrono::milliseconds{0};
  if (m_notifications != nullptr) {
    m_notifications->addInternal(
        "Timer", "Timer finished", std::format("{} elapsed", formatTimer(m_timerDuration)), Urgency::Normal,
        kDefaultNotificationTimeout, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        NotificationDndPolicy::ShowToast
    );
  }
  syncTimer();
  if (m_active) {
    PanelManager::instance().requestRedraw();
  }
}

void HomeTab::onTimerTick() {
  if (m_timerState != TimerState::Running) {
    return;
  }
  const auto remaining = timerRemaining();
  if (remaining.count() == 0) {
    finishTimer();
    return;
  }
  if (m_active && ceilSeconds(remaining) != m_timerShown) {
    PanelManager::instance().requestUpdateOnly();
  }
}

void HomeTab::syncTimer() {
  if (m_timerTime == nullptr) {
    return;
  }
  const auto remaining = timerRemaining();
  m_timerShown = ceilSeconds(remaining);
  const std::string timeText = formatTimer(m_timerShown);
  if (m_timerTime->text() != timeText) {
    m_timerTime->setText(timeText);
  }
  // The idle line shows the length that will start.
  const std::string idleText = formatTimer(m_timerDuration);
  if (m_timerIdleTime != nullptr && m_timerIdleTime->text() != idleText) {
    m_timerIdleTime->setText(idleText);
  }

  const bool running = m_timerState == TimerState::Running;
  const bool expanded = running || m_timerState == TimerState::Paused;
  bool changed = false;
  changed |= setShown(m_timerRunBox, expanded);
  changed |= setShown(m_timerIdleRow, !expanded);
  m_timerExpanded = expanded;

  // The time is an accent block with dark text while the timer runs.
  if (m_timerLit != running && m_timerTimeBlock != nullptr) {
    m_timerLit = running;
    if (running) {
      m_timerTimeBlock->setFill(colorSpecFromRole(ColorRole::Primary));
      m_timerTime->setColor(colorSpecFromRole(ColorRole::OnPrimary));
    } else {
      m_timerTimeBlock->clearFill();
      m_timerTime->setColor(colorSpecFromRole(ColorRole::Primary));
    }
  }

  if (m_timerStatus != nullptr) {
    const std::string status = m_timerState == TimerState::Done ? "done" : "ready";
    if (m_timerStatus->text() != status) {
      m_timerStatus->setText(status);
    }
  }

  // Session squares in the tile's header slot: filled for each counted session, empty up to the goal.
  {
    rollTimerDay();
    const int goal = timerGoal();
    const int filled = std::min(m_timerSessions, goal);
    std::string squares;
    for (int i = 0; i < goal; ++i) {
      squares += i < filled ? "■" : "□";
    }
    squares += std::format(" {}/{}", filled, goal);
    if (m_timerSessions > goal) {
      squares += std::format(" +{}", m_timerSessions - goal);
    }
    m_squaresText = squares;
    updateTodayHeader();
  }

  const auto syncKey = [](term_frame::Keyword& key, std::string_view word, std::size_t hot, bool block) {
    if (key.word != word || key.block != block || key.hot != hot) {
      key.word = std::string(word);
      key.hot = hot;
      key.block = block;
      key.blockText = key.word;
      term_frame::refreshKeyword(key);
    }
  };
  // `start` and `pause` share the letter `a`.
  syncKey(m_timerControlKeys[0], running ? "pause" : "start", running ? 1 : 2, false);
  for (std::size_t i = 0; i < m_timerPresetKeys.size(); ++i) {
    syncKey(
        m_timerPresetKeys[i], m_timerPresetKeys[i].word, m_timerPresetKeys[i].hot,
        m_timerDuration == std::chrono::minutes{kTimerPresetMinutes[i]}
    );
  }
  if (changed) {
    relayout();
  }
}

// ---- 4 today ----------------------------------------------------------------------------------------------------

// The next calendar events, the open tasks and one weather line.
std::unique_ptr<Flex> HomeTab::createTodayBox(float scale) {
  auto box = term_frame::btopBox("⁴", "today", scale, &m_todayFrame);
  addTimerRows(*box, scale);
  addHiddenRows(*box, scale, m_eventRows, m_fixedRows, [](std::size_t) { return []() { openControlCenterTab("calendar"); }; });
  addHiddenRows(*box, scale, m_taskRows, m_fixedRows, [](std::size_t) { return []() { openControlCenterTab("tasks"); }; });
  box->addChild(ui::label({
      .out = &m_weatherLine,
      .fontSize = term_frame::kBtopFontSize * scale,
      .fontFamily = std::string(term_frame::kFont),
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .maxLines = 1,
  }));
  syncToday();
  return box;
}

void HomeTab::updateTodayHeader() {
  if (m_todayFrame == nullptr) {
    return;
  }
  const bool next = !m_nextText.empty();
  m_todayFrame->setInfo(next ? m_nextText : m_squaresText);
  m_todayFrame->setInfoColor(next ? colorSpecFromRole(ColorRole::OnSurface) : term_frame::dimColor());
}

void HomeTab::syncToday() {
  if (m_todayFrame == nullptr) {
    return;
  }
  bool changed = false;
  const auto now = std::chrono::system_clock::now();
  const auto localDay = [](std::chrono::system_clock::time_point at) {
    const std::time_t t = std::chrono::system_clock::to_time_t(at);
    std::tm local{};
    localtime_r(&t, &local);
    return local.tm_year * 400 + local.tm_yday;
  };
  const int today = localDay(now);

  // Events that have not ended yet, soonest first.
  std::vector<const CalendarEvent*> upcoming;
  const bool calendarOn = m_calendar != nullptr && m_calendar->enabled() && m_calendar->hasData();
  if (calendarOn) {
    for (const CalendarEvent& event : m_calendar->snapshot().events) {
      if (event.end > now) {
        upcoming.push_back(&event);
      }
    }
    std::ranges::sort(upcoming, [](const CalendarEvent* a, const CalendarEvent* b) { return a->start < b->start; });
  }
  std::array<bool, kTodayItems> eventWanted{};
  for (std::size_t i = 0; i < kTodayItems; ++i) {
    term_frame::ToggleRow& row = m_eventRows[i];
    eventWanted[i] = i < upcoming.size();
    if (!eventWanted[i] || row.button == nullptr) {
      continue;
    }
    const CalendarEvent& event = *upcoming[i];
    std::string when;
    if (event.allDay) {
      when = "all day";
    } else if (event.start <= now) {
      when = "now";
    } else {
      const std::string clock = clockText(event.start);
      const int days = localDay(event.start) - today;
      if (days == 0) {
        when = clock;
      } else if (days == 1) {
        when = "tmrw " + clock;
      } else {
        const std::time_t t = std::chrono::system_clock::to_time_t(event.start);
        std::tm local{};
        localtime_r(&t, &local);
        char weekday[8];
        std::strftime(weekday, sizeof(weekday), "%a", &local);
        when = std::string(weekday) + " " + clock;
      }
    }
    const std::string title = "◇ " + oneLine(event.title); // ◇ marks an event
    setRowName(row, title);
    term_frame::setToggleRow(row, true, false, when);
  }

  // Open tasks, marked with an empty box so they read apart from the events above them.
  std::vector<const TaskItem*> open;
  if (m_tasks != nullptr) {
    for (const TaskItem& item : m_tasks->snapshot().items) {
      if (!item.completed) {
        open.push_back(&item);
      }
    }
  }
  const std::string todayKey = localDateKey();
  std::array<bool, kTodayItems> taskWanted{};
  for (std::size_t i = 0; i < kTodayItems; ++i) {
    term_frame::ToggleRow& row = m_taskRows[i];
    taskWanted[i] = i < open.size();
    if (!taskWanted[i] || row.button == nullptr) {
      continue;
    }
    const TaskItem& item = *open[i];
    const std::string title = "□ " + oneLine(item.title);
    setRowName(row, title);
    std::string due;
    if (!item.dueDate.empty()) {
      due = item.dueDate == todayKey ? "today" : (item.dueDate < todayKey ? "late" : item.dueDate.substr(std::min<std::size_t>(5, item.dueDate.size())));
    }
    term_frame::setToggleRow(row, true, false, due);
  }

  // One weather line: now, the sky, today's range.
  std::string weather = "weather --";
  if (m_services.weather != nullptr && m_services.weather->snapshot().valid) {
    const WeatherSnapshot& snapshot = m_services.weather->snapshot();
    weather = std::format(
        "{:.0f}° {}", snapshot.current.temperatureC,
        StringUtils::toLower(WeatherService::shortDescriptionForCode(snapshot.current.weatherCode))
    );
    if (!snapshot.forecastDays.empty()) {
      weather += std::format(
          " · {:.0f}°–{:.0f}°", snapshot.forecastDays.front().temperatureMinC, snapshot.forecastDays.front().temperatureMaxC
      );
    }
  }
  term_frame::setLabelTone(m_weatherLine, weather, Tone::Normal);

  // Header slot: the countdown to the next timed event today; the session squares only when there is none.
  m_nextText.clear();
  for (const CalendarEvent* event : upcoming) {
    if (event->allDay || event->start <= now || localDay(event->start) != today) {
      continue;
    }
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(event->start - now).count();
    m_nextText = minutes < 60 ? std::format("in {}m", minutes + 1) : std::format("in {}h {}m", minutes / 60, minutes % 60);
    break;
  }
  updateTodayHeader();

  // The timer line comes first and keeps its lines (one idle, two running); then events, tasks and the weather, in
  // that order, as many as the tile's height holds.
  const int capacity = m_capacity[TileToday] - (m_timerExpanded ? 2 : 1);
  std::vector<FitItem> items;
  for (std::size_t i = 0; i < kTodayItems; ++i) {
    items.push_back({m_eventRows[i].button, 1, eventWanted[i]});
  }
  for (std::size_t i = 0; i < kTodayItems; ++i) {
    items.push_back({m_taskRows[i].button, 1, taskWanted[i]});
  }
  items.push_back({m_weatherLine, 1, true});
  changed |= fitItems(items, capacity);
  if (changed) {
    relayout();
  }
}

// ---- 6 fleet ----------------------------------------------------------------------------------------------------

// Whether the things this machine depends on are up. One line each, dim when healthy; a row that is down says so in
// the warning tone with a leading "!". The checks run in the background at most once a minute while the dashboard is
// open; until the first answer arrives the last known values show, dimmed with their age.
std::unique_ptr<Flex> HomeTab::createFleetBox(float scale) {
  auto box = term_frame::btopBox("⁶", "fleet", scale, &m_fleetFrame);
  for (std::size_t i = 0; i < kFleetRows; ++i) {
    box->addChild(term_frame::toggleRow(kFleetNames[i], scale, m_fleetRows[i], [this, i]() { activateFleet(i); }, nullptr));
    m_fixedRows.push_back(m_fleetRows[i].button);
  }
  // One row per tailscale peer; shown instead of the summary line when the tile has room.
  addHiddenRows(*box, scale, m_peerRows, m_fixedRows, [](std::size_t) { return []() { openControlCenterTab("network"); }; });
  box->addChild(ui::label({
      .out = &m_fleetDetail,
      .fontSize = term_frame::kBtopFontSize * scale,
      .fontFamily = std::string(term_frame::kFont),
      .color = term_frame::toneColor(Tone::Error),
      .maxLines = 3,
      .visible = false,
      .participatesInLayout = false,
  }));
  m_detailLabels.push_back(m_fleetDetail);
  syncFleet();
  return box;
}

void HomeTab::syncFleet() {
  if (m_fleetFrame == nullptr) {
    return;
  }
  const ScriptJob& job = *m_fleetJob;
  // A finished check stamps the data and keeps it for the next start.
  if (!job.running && job.loaded && job.startedAt != m_fleetSeenStart) {
    m_fleetSeenStart = job.startedAt;
    m_fleetAt = std::time(nullptr);
    std::error_code error;
    std::filesystem::create_directories(fleetPath().parent_path(), error);
    std::ofstream(fleetPath(), std::ios::trunc) << m_fleetAt << '\n' << job.out;
  }
  const long long age = m_fleetAt > 0 ? std::time(nullptr) - m_fleetAt : -1;
  const bool stale = job.running || age < 0 || age > 90;
  const std::string ageText = age >= 0 ? " · " + formatAgeShort(age) : std::string{};

  struct Line {
    bool show = true;
    bool down = false;
    bool live = false; // read straight from a service, never stale
    std::string value = "unknown";
  };
  std::array<Line, kFleetRows> lines;

  const std::string tailscale = scriptValue(job, "tailscale");
  const std::string server = scriptValue(job, "server");
  if (tailscale == "absent" || server == "none") {
    lines[0].show = false;
  } else if (server == "online") {
    const std::string ms = scriptValue(job, "server_ms");
    lines[0].value = ms.empty() ? std::string{"online"} : "online · " + ms + " ms";
  } else if (server == "offline") {
    lines[0].down = true;
    lines[0].value = "offline";
  }

  const std::string syncthing = scriptValue(job, "syncthing");
  if (syncthing == "absent") {
    lines[1].show = false;
  } else if (syncthing == "ok") {
    lines[1].value = "up to date";
  } else if (syncthing.starts_with("syncing:")) {
    lines[1].value = "syncing " + syncthing.substr(8) + "%";
  } else if (syncthing == "out") {
    lines[1].down = true;
    lines[1].value = "out of sync";
  }

  // Radicale: the calendar service's own connection; no check of ours.
  if (m_calendar == nullptr || !m_calendar->enabled()) {
    lines[2].show = false;
  } else {
    lines[2].live = true;
    switch (m_calendar->syncError()) {
    case CalendarService::SyncError::None:
      lines[2].value = m_calendar->hasData() ? "reachable" : "unknown";
      break;
    case CalendarService::SyncError::NoCalendars:
      lines[2].value = "reachable";
      break;
    case CalendarService::SyncError::Unreachable:
      lines[2].down = true;
      lines[2].value = "unreachable";
      break;
    case CalendarService::SyncError::AuthFailed:
      lines[2].down = true;
      lines[2].value = "auth failed";
      break;
    case CalendarService::SyncError::Failed:
      lines[2].down = true;
      lines[2].value = "sync failed";
      break;
    }
  }

  const std::string state = scriptValue(job, "ts_state");
  if (tailscale == "absent") {
    lines[3].show = false;
  } else if (state == "Running") {
    lines[3].value = std::format(
        "{} of {} peers online", numberValue(job, "ts_online", 0), numberValue(job, "ts_total", 0)
    );
  } else if (!state.empty() && state != "unknown") {
    lines[3].down = true;
    lines[3].value = StringUtils::toLower(state);
  }

  std::size_t down = 0;
  std::size_t unknown = 0;
  bool changed = false;
  for (std::size_t i = 0; i < kFleetRows; ++i) {
    term_frame::ToggleRow& row = m_fleetRows[i];
    changed |= setShown(row.button, lines[i].show);
    if (!lines[i].show || row.button == nullptr) {
      continue;
    }
    const Line& line = lines[i];
    const bool fresh = line.live || !stale;
    const bool warn = line.down && fresh;
    down += line.down ? 1 : 0;
    unknown += line.value == "unknown" ? 1 : 0;
    const std::string name = std::string(line.down ? "! " : "") + kFleetNames[i];
    setRowName(row, name);
    term_frame::setToggleRow(row, true, false, line.value + (fresh || line.value == "unknown" ? "" : ageText));
    setRowTone(row, warn ? Tone::Warning : Tone::Dim);
  }

  // Peers: with room, one row each (state, or when it was last seen) replaces the summary line; otherwise the
  // summary stays. Offline peers are not "down": only the server and the tailscale daemon are.
  const int fleetCapacity = m_capacity[TileFleet] - detailLines(m_fleetDetail);
  const int baseRows = (lines[0].show ? 1 : 0) + (lines[1].show ? 1 : 0) + (lines[2].show ? 1 : 0);
  const long long peerCount = tailscale == "absent" || state != "Running" ? 0 : numberValue(job, "peer_count", 0);
  const bool showPeers = peerCount > 0 && peerCount <= static_cast<long long>(kPeerRows)
      && baseRows + static_cast<int>(peerCount) <= fleetCapacity;
  changed |= setShown(m_fleetRows[3].button, lines[3].show && !showPeers && baseRows + 1 <= fleetCapacity);
  const long long nowSeconds = std::time(nullptr);
  for (std::size_t i = 0; i < kPeerRows; ++i) {
    term_frame::ToggleRow& row = m_peerRows[i];
    const bool show = showPeers && static_cast<long long>(i) < peerCount;
    changed |= setShown(row.button, show);
    if (!show || row.button == nullptr) {
      continue;
    }
    const std::string entry = scriptValue(job, std::format("peer_{}", i + 1));
    const std::size_t first = entry.find('\t');
    const std::size_t second = first == std::string::npos ? first : entry.find('\t', first + 1);
    std::string name = entry.substr(0, first);
    std::string status = "unknown";
    if (first != std::string::npos && second != std::string::npos) {
      long long lastSeen = 0;
      const std::string seen = entry.substr(second + 1);
      std::from_chars(seen.data(), seen.data() + seen.size(), lastSeen);
      if (entry.substr(first + 1, second - first - 1) == "online") {
        status = "online";
      } else if (lastSeen > 100000000) {
        const long long ago = std::max(0LL, nowSeconds - lastSeen);
        status = ago < 3600 ? std::format("last seen {}m", ago / 60)
            : ago < 86400   ? std::format("last seen {}h", ago / 3600)
                            : std::format("last seen {}d", ago / 86400);
      } else {
        status = "offline";
      }
    }
    name = "· " + name;
    setRowName(row, name);
    term_frame::setToggleRow(row, true, false, status);
    setRowTone(row, Tone::Dim);
  }
  const bool headerStale = stale && down == 0;
  if (down > 0) {
    m_fleetFrame->setInfo(
        unknown > 0 ? std::format("! {} down · {} unknown", down, unknown) : std::format("! {} down", down)
    );
    m_fleetFrame->setInfoColor(colorSpecFromRole(stale ? ColorRole::OnSurfaceVariant : ColorRole::Warning));
  } else if (unknown > 0 || headerStale) {
    m_fleetFrame->setInfo(unknown > 0 ? std::format("{} unknown", unknown) : std::format("all ok{}", ageText));
    m_fleetFrame->setInfoColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  } else {
    m_fleetFrame->setInfo("all ok");
    m_fleetFrame->setInfoColor(colorSpecFromRole(ColorRole::OnSurface));
  }
  if (changed) {
    relayout();
  }
}

// Enter: syncthing opens its web UI, server and tailscale the network panel, radicale the calendar tab.
void HomeTab::activateFleet(std::size_t row) {
  if (row == 2) {
    openControlCenterTab("calendar");
    return;
  }
  if (row != 1) {
    openControlCenterTab("network");
    return;
  }
  std::string url = scriptValue(*m_fleetJob, "syncthing_url");
  if (url.empty()) {
    url = "http://127.0.0.1:8384";
  }
  const bool started = process::runAsync(
      std::vector<std::string>{"xdg-open", url},
      process::RunCallbacks{
          .onExit =
              [mailbox = m_mailbox, url](process::RunResult result) {
                if (result.exitCode == 0) {
                  return;
                }
                const std::string why = failureReason(result);
                DeferredCall::callLater([mailbox, url, why = std::move(why)]() {
                  mailbox->done.push_back({.id = "fleet", .exitCode = 1, .error = "could not open " + url + ": " + why});
                  PanelManager::instance().refresh();
                });
              },
      }
  );
  if (!started) {
    m_mailbox->done.push_back({.id = "fleet", .exitCode = -1, .error = "could not run xdg-open"});
  }
}

// ---- 7 system ---------------------------------------------------------------------------------------------------

// One strip across the bottom: lock, sleep and logout on the left, reboot and shutdown on the right. Lock and sleep
// run at once. Logout, reboot and shutdown end the session, so the first press only arms the key (it turns into a
// block asking "confirm?") and a second press runs it. Leaving the key, any other key, or five seconds disarms it.
// A failed action says why in the middle of the strip.
std::unique_ptr<Flex> HomeTab::createSystemBox(float scale) {
  auto box = term_frame::btopBox("⁷", "system", scale, &m_systemFrame);
  struct KeySpec {
    const char* word;
    std::size_t hot;
    bool warning;
  };
  const auto addKey = [&](Flex& row, std::size_t index, const KeySpec& spec) {
    row.addChild(
        term_frame::makeKeyword(
            m_powerKeys[index], spec.word, spec.hot, scale, [this, index]() { pressPower(index); },
            [this, index]() {
              if (m_armed == &m_powerKeys[index]) {
                clearArmed();
              }
            },
            1.5F, 0.85F, spec.warning
        )
    );
  };
  auto strip = ui::row({.out = &m_stripRow, .align = FlexAlign::Center, .gap = Style::spaceMd * scale});
  auto plain = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale});
  addKey(*plain, 0, {"lock", 0, false});
  addKey(*plain, 1, {"sleep", 0, false});
  addKey(*plain, 2, {"logout", 2, false});
  strip->addChild(std::move(plain));

  auto middle = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .flexGrow = 1.0F});
  middle->addChild(ui::label({
      .out = &m_systemDetail,
      .fontSize = term_frame::kBtopFontSize * scale,
      .fontFamily = std::string(term_frame::kFont),
      .color = term_frame::toneColor(Tone::Error),
      .maxLines = 1,
      .visible = false,
      .participatesInLayout = false,
      .configure = [](Label& detail) { detail.setEllipsize(TextEllipsize::End); },
  }));
  strip->addChild(std::move(middle));

  // The two that cannot be undone, on capital letters (Shift held).
  auto danger = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale});
  addKey(*danger, 3, {"R reboot", 0, true});
  addKey(*danger, 4, {"D shutdown", 0, true});
  strip->addChild(std::move(danger));
  box->addChild(std::move(strip));
  return box;
}

void HomeTab::pressPower(std::size_t index) {
  constexpr std::size_t kFirstConfirmed = 2; // logout, reboot, shutdown
  if (index >= kPowerKeyCount) {
    return;
  }
  if (index >= kFirstConfirmed) {
    pressConfirmed(m_powerKeys[index], nullptr, "", "confirm?", [this, index]() { runPower(index); });
    return;
  }
  clearArmed();
  runPower(index);
}

void HomeTab::runPower(std::size_t index) {
  constexpr std::array<const char*, kPowerKeyCount> kActions{"lock", "suspend", "logout", "reboot", "shutdown"};
  const std::string action = kActions[index];
  const std::string id = "power:" + action;
  if (setShown(m_systemDetail, false)) {
    relayout();
  }
  const bool started = process::runAsync(
      std::vector<std::string>{"terminus-bar", "msg", "session", action},
      process::RunCallbacks{
          .onExit =
              [mailbox = m_mailbox, id](process::RunResult result) {
                if (result.exitCode == 0) {
                  return;
                }
                const std::string why = failureReason(result, true);
                DeferredCall::callLater([mailbox, id, code = result.exitCode, why = std::move(why)]() {
                  mailbox->done.push_back({.id = id, .exitCode = code, .error = why});
                  PanelManager::instance().refresh();
                });
              },
      }
  );
  if (!started) {
    m_mailbox->done.push_back({.id = id, .exitCode = -1, .error = "could not run terminus-bar"});
  }
}

// ---- actions: arming, terminal runs, outcomes -------------------------------------------------------------------

void HomeTab::pressConfirmed(
    term_frame::Keyword& keyword, term_frame::ActionRow* row, std::string_view detail, std::string_view blockText,
    const std::function<void()>& run
) {
  if (m_armed != &keyword) {
    clearArmed();
    m_armed = &keyword;
    m_armedRow = row;
    keyword.block = true;
    keyword.blockText = blockText.empty() ? keyword.word : std::string(blockText);
    term_frame::refreshKeyword(keyword);
    if (row != nullptr && !detail.empty() && term_frame::setActionDetail(*row, detail, Tone::Normal)) {
      relayout();
    }
    m_armTimer.start(kArmTimeout, [this]() { clearArmed(); });
    return;
  }
  clearArmed();
  run();
}

void HomeTab::clearArmed() {
  m_armTimer.stop();
  if (m_armed == nullptr) {
    return;
  }
  m_armed->block = false;
  term_frame::refreshKeyword(*m_armed);
  if (m_armedRow != nullptr && term_frame::setActionDetail(*m_armedRow, "", Tone::Dim)) {
    relayout();
  }
  m_armed = nullptr;
  m_armedRow = nullptr;
  PanelManager::instance().requestRedraw();
}

void HomeTab::noteKey(std::uint32_t sym, std::uint32_t /*modifiers*/, bool pressed, const InputArea* focused) {
  if (m_armed == nullptr || !pressed) {
    return;
  }
  // Shift and the other modifiers are part of the confirming press (Shift+R).
  if (sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R) {
    return;
  }
  if (KeySymbol::isEnterOrSpace(sym) && focused != nullptr && m_armed->button != nullptr
      && focused == m_armed->button->inputArea()) {
    return;
  }
  if (m_armed->hot < m_armed->word.size() && (sym | 0x20U) == static_cast<std::uint32_t>(m_armed->word[m_armed->hot] | 0x20)) {
    return;
  }
  clearArmed();
}

term_frame::ActionRow* HomeTab::actionRowFor(std::string_view id) {
  const std::array<std::pair<std::string_view, term_frame::ActionRow*>, 5> rows{{
      {"update", &m_updateRow},
      {"orphans", &m_orphansRow},
      {"cache", &m_cacheRow},
      {"failed", &m_failedRow},
      {"journal", &m_journalRow},
  }};
  for (const auto& [name, row] : rows) {
    if (id == name) {
      return row;
    }
  }
  return nullptr;
}

// Opens `command` in a terminal that stays until Enter. The terminal's shell writes the exit code to a file we poll,
// so a failure shows in the row; the launcher's own failure arrives through the mailbox.
void HomeTab::launchInTerminal(const std::string& id, const std::string& what, const std::string& command) {
  term_frame::ActionRow* row = actionRowFor(id);
  if (row != nullptr && term_frame::setActionDetail(*row, "", Tone::Dim)) {
    relayout();
  }
  const auto already = std::ranges::find(m_pending, id, &PendingAction::id);
  if (already != m_pending.end()) {
    if (row != nullptr && term_frame::setActionDetail(*row, "already open in a terminal", Tone::Dim)) {
      relayout();
    }
    return;
  }
  const std::string rc = (runtimeDir() / ("terminus-dashboard-" + id + ".rc")).string();
  std::error_code error;
  std::filesystem::remove(rc, error);
  const std::string script = std::format(
      "{}; rc=$?; echo $rc > '{}'; printf '\\n[ exit %s · press enter to close ]' \"$rc\"; read _; exit $rc", command, rc
  );
  const bool started = process::runAsync(
      std::vector<std::string>{"uwsm-app", "--", "ghostty", "-e", "sh", "-c", script},
      process::RunCallbacks{
          .onExit =
              [mailbox = m_mailbox, id, rc, launched = std::chrono::steady_clock::now()](process::RunResult result) {
                // Only a terminal that dies at once, before its shell wrote an exit code, never started; a window
                // closed later exits non-zero too, and that is not a failure of the action.
                std::error_code ec;
                const bool early = std::chrono::steady_clock::now() - launched < std::chrono::seconds{5};
                if (result.exitCode == 0 || !early || std::filesystem::exists(rc, ec)) {
                  return;
                }
                const std::string why = failureReason(result);
                DeferredCall::callLater([mailbox, id, code = result.exitCode, why = std::move(why)]() {
                  mailbox->done.push_back({.id = id, .exitCode = code, .error = "terminal did not start: " + why});
                  PanelManager::instance().refresh();
                });
              },
      }
  );
  if (!started) {
    setActionFailure(id, "could not start a terminal");
    return;
  }
  m_pending.push_back({.id = id, .what = what, .rcFile = rc, .startedAt = std::chrono::steady_clock::now()});
  if (!m_actionPoll.active()) {
    m_actionPoll.startRepeating(std::chrono::seconds{2}, [this]() { pollTerminalActions(); });
  }
}

void HomeTab::setActionFailure(const std::string& id, const std::string& text) {
  if (term_frame::ActionRow* row = actionRowFor(id);
      row != nullptr && term_frame::setActionDetail(*row, text, Tone::Error)) {
    relayout();
  }
}

// Reads the exit codes the terminals left behind.
void HomeTab::pollTerminalActions() {
  const auto now = std::chrono::steady_clock::now();
  bool refresh = false;
  for (auto it = m_pending.begin(); it != m_pending.end();) {
    std::error_code error;
    if (std::filesystem::exists(it->rcFile, error)) {
      int code = -1;
      std::ifstream(it->rcFile) >> code;
      std::filesystem::remove(it->rcFile, error);
      if (code == 0) {
        refresh = true;
      } else if (code == 127) {
        setActionFailure(it->id, it->what + ": command not found");
      } else {
        setActionFailure(it->id, std::format("{} failed (exit {})", it->what, code));
      }
      it = m_pending.erase(it);
    } else if (now - it->startedAt > kPendingGiveUp) {
      it = m_pending.erase(it);
    } else {
      ++it;
    }
  }
  if (m_pending.empty()) {
    m_actionPoll.stop();
  }
  if (refresh) {
    refreshUpkeep(true);
  }
  PanelManager::instance().requestUpdateOnly();
}

// Applies what the worker threads reported.
void HomeTab::collectOutcomes() {
  if (m_mailbox->done.empty()) {
    return;
  }
  std::vector<ActionOutcome> done;
  done.swap(m_mailbox->done);
  for (const ActionOutcome& outcome : done) {
    if (outcome.id == "fleet") {
      term_frame::setLabelTone(m_fleetDetail, outcome.error, Tone::Error);
      if (setShown(m_fleetDetail, !outcome.error.empty())) {
        relayout();
      }
    } else if (outcome.id == "inbox") {
      // A dismissed mako notification: refresh the list, and say why when it failed.
      runScriptJob(m_notesJob, kNotesScript, true, std::chrono::seconds{5});
      const std::string text = outcome.error.empty() ? std::string{} : "dismiss failed: " + outcome.error;
      term_frame::setLabelTone(m_inboxDetail, text, Tone::Error);
      if (setShown(m_inboxDetail, !text.empty())) {
        relayout();
      }
    } else if (outcome.id.starts_with("power:")) {
      const std::string text = std::format("{} failed: {}", outcome.id.substr(6), outcome.error);
      term_frame::setLabelTone(m_systemDetail, text, Tone::Error);
      if (setShown(m_systemDetail, true)) {
        relayout();
      }
    } else {
      m_pending.erase(
          std::remove_if(m_pending.begin(), m_pending.end(), [&](const PendingAction& p) { return p.id == outcome.id; }),
          m_pending.end()
      );
      setActionFailure(outcome.id, outcome.error);
    }
  }
}

// The highlighted letters run their keyword while the dashboard has focus. They are unique across the tiles, so no
// tile has to be selected first. Capital letters (R, D) need Shift. Key repeat arrives as more presses, so a held
// letter acts once.
bool HomeTab::handleHotkey(std::uint32_t sym, std::uint32_t modifiers, bool pressed) {
  // A release always frees the letter, even one that arrives with a modifier down.
  if (!pressed && m_hotkeyHeld != 0U && (sym | 0x20U) == m_hotkeyHeld) {
    m_hotkeyHeld = 0;
  }
  const bool shifted = modifiers == KeyMod::Shift;
  const bool upper = sym >= 'A' && sym <= 'Z'; // a capital keysym counts as Shift even when the modifier is absent
  const bool letter = (sym >= 'a' && sym <= 'z') || upper;
  if (!letter || (modifiers != 0U && !shifted)) {
    return false;
  }
  for (const HotKey& entry : m_hotkeys) {
    term_frame::Keyword& key = *entry.key;
    if (!key.activate || key.button == nullptr || key.hot >= key.word.size()) {
      continue;
    }
    const char hot = key.word[key.hot];
    const bool capital = hot >= 'A' && hot <= 'Z';
    if (static_cast<std::uint32_t>(hot | 0x20) != (sym | 0x20U) || (capital ? !(shifted || upper) : shifted)) {
      continue;
    }
    if (entry.gate != nullptr && !entry.gate->visible()) {
      continue;
    }
    if (!pressed) {
      if (m_hotkeyHeld == (sym | 0x20U)) {
        m_hotkeyHeld = 0;
      }
      return true;
    }
    if (m_hotkeyHeld == (sym | 0x20U)) {
      return true;
    }
    m_hotkeyHeld = sym | 0x20U;
    const auto activate = key.activate;
    activate();
    return true;
  }
  return false;
}

// ---- layout and lifecycle ---------------------------------------------------------------------------------------

std::unique_ptr<Flex> HomeTab::createHeaderActions() {
  const float scale = contentScale();
  auto row = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .glyph = "settings",
          .onClick = []() { PanelManager::instance().openSettingsWindow(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      }),
      ui::button({
          .out = &m_sessionButton,
          .glyph = "shutdown",
          .onClick = []() { PanelManager::instance().togglePanel("session"); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  syncHeaderActions();
  return row;
}

void HomeTab::syncHeaderActions() {
  if (m_sessionButton != nullptr) {
    m_sessionButton->setVisible(m_config == nullptr || m_config->config().controlCenter.showSessionButton);
  }
}

// The grid: three rows of two tiles share the height left by the strip, so nothing is left over; both tiles of a row
// have the same top and bottom edge. Each tile's capacity in lines is worked out here and the sync functions show as
// many rows as fit it.
void HomeTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }
  const float scale = contentScale();
  const float gap = Style::spaceSm * scale;
  const float width = std::max(1.0F, contentWidth);
  const float height = std::max(1.0F, bodyHeight > 0.0F ? bodyHeight : 600.0F * scale);
  const float inset = term_frame::boxInset(scale);
  const float chrome = term_frame::btopChrome(scale);
  const float rowHeight = kHomeRowHeight * scale;
  const float rowGap = Style::spaceXs * scale;

  const float margin = gap;
  const float gridWidth = std::max(1.0F, width - margin);
  const float leftWidth = std::max(1.0F, std::floor((gridWidth - gap) * 0.5F));
  const float rightWidth = std::max(1.0F, gridWidth - gap - leftWidth);
  const float stripHeight = chrome + rowHeight;
  const float available = std::max(3.0F, height - stripHeight - 3.0F * gap);
  const float baseHeight = std::floor(available / 3.0F);
  const std::array<float, 3> rowHeights{baseHeight, baseHeight, available - 2.0F * baseHeight};
  const float inner = std::max(1.0F, std::min(leftWidth, rightWidth) - 2.0F * inset);
  const float stripInner = std::max(1.0F, gridWidth - 2.0F * inset);
  m_innerWidth = inner;

  for (Flex* row : m_fixedRows) {
    if (row != nullptr) {
      term_frame::pinSize(*row, inner, rowHeight);
    }
  }
  for (Label* detail : m_detailLabels) {
    if (detail != nullptr) {
      detail->setMaxWidth(inner);
    }
  }

  // How many one-line rows each tile holds (the toggle rows sit without a gap between them).
  const auto capacityOf = [&](float tileHeight) {
    return std::max(1, static_cast<int>(std::floor((tileHeight - chrome + rowGap) / (rowHeight + rowGap))));
  };
  const std::array<int, TileCount> capacity{
      capacityOf(rowHeights[0]),
      std::max(1, static_cast<int>(std::floor((rowHeights[0] - chrome) / rowHeight))),
      capacityOf(rowHeights[1]),
      capacityOf(rowHeights[1]),
      capacityOf(rowHeights[2]),
      capacityOf(rowHeights[2]),
  };
  if (capacity != m_capacity) {
    m_capacity = capacity;
    relayout();
  }

  // Host: the avatar is two rows tall, the user and host sit beside it.
  if (m_hostHeader != nullptr && m_avatarFrame != nullptr) {
    const float avatar = 2.0F * rowHeight + rowGap;
    m_avatarSize = avatar;
    m_avatarFrame->setSize(avatar, avatar);
    if (m_avatarArea != nullptr) {
      m_avatarArea->setSize(avatar, avatar);
    }
    const float avatarInner = std::max(1.0F, avatar - 2.0F * Style::borderWidth);
    if (m_avatar != nullptr) {
      m_avatar->setSize(avatarInner, avatarInner);
    }
    if (m_avatarHint != nullptr) {
      m_avatarHint->setMaxWidth(avatarInner);
    }
    const float textWidth = std::max(1.0F, inner - avatar - Style::spaceSm * scale);
    m_hostUser->setMaxWidth(textWidth);
    m_hostName->setMaxWidth(textWidth);
    term_frame::pinSize(*m_hostText, textWidth, avatar);
    term_frame::pinSize(*m_hostHeader, inner, avatar);
  }
  if (m_hostInfoBox != nullptr) {
    m_hostInfoBox->setMinWidth(inner);
    m_hostInfoBox->setMaxWidth(inner);
  }
  if (m_timerTime != nullptr) {
    m_timerTime->setFontSize(kTimerFontSize * scale);
  }
  if (m_systemDetail != nullptr) {
    m_systemDetail->setMaxWidth(stripInner * 0.4F);
  }

  const auto pin = [](Flex* node, float w, float h) {
    if (node != nullptr) {
      term_frame::pinSize(*node, w, h);
    }
  };
  if (m_margin != nullptr) {
    m_margin->setMinWidth(margin);
    m_margin->setMaxWidth(margin);
  }
  pin(m_grid, gridWidth, height);
  for (std::size_t i = 0; i < m_gridRows.size(); ++i) {
    pin(m_gridRows[i], gridWidth, rowHeights[i]);
  }
  pin(m_hostFrame, leftWidth, rowHeights[0]);
  pin(m_togglesFrame, rightWidth, rowHeights[0]);
  pin(m_upkeepFrame, leftWidth, rowHeights[1]);
  pin(m_todayFrame, rightWidth, rowHeights[1]);
  pin(m_inboxFrame, leftWidth, rowHeights[2]);
  pin(m_fleetFrame, rightWidth, rowHeights[2]);
  pin(m_systemFrame, gridWidth, stripHeight);
  pin(m_stripRow, stripInner, rowHeight);
  m_rootLayout->setMinWidth(width);
  m_rootLayout->setMaxWidth(width);
  m_rootLayout->layout(renderer);
}

void HomeTab::doUpdate(Renderer& renderer) {
  syncHeaderActions();
  if (m_active) {
    sync();
    syncAvatar(renderer);
  }
}

void HomeTab::onFrameTick(float /*deltaMs*/) {}

void HomeTab::setActive(bool active) {
  const bool becameActive = active && !m_active;
  m_active = active;
  if (becameActive) {
    refreshUpkeep(false);
    DeferredCall::callLater([]() {
      PanelManager::instance().requestLayout();
      PanelManager::instance().requestUpdateOnly();
    });
  }
}

void HomeTab::onClose() {
  m_active = false;
  clearArmed();
  m_armTimer.stop();
  m_rootLayout = nullptr;
  m_margin = nullptr;
  m_grid = nullptr;
  m_gridRows = {};
  m_stripRow = nullptr;
  m_fixedRows.clear();
  m_detailLabels.clear();
  m_hotkeys.clear();
  m_hotkeyHeld = 0;
  m_hostFrame = nullptr;
  m_hostHeader = nullptr;
  m_hostText = nullptr;
  m_hostUser = nullptr;
  m_hostName = nullptr;
  m_avatarArea = nullptr;
  m_avatarFrame = nullptr;
  m_avatar = nullptr;
  m_avatarInitial = nullptr;
  m_avatarHint = nullptr;
  m_avatarSize = 0.0F;
  m_avatarPath.clear();
  m_hostLive = {};
  m_hostInfoRows = {};
  m_hostInfoBox = nullptr;
  m_hostLiveBox = nullptr;
  m_infoKey = {};
  m_upkeepFrame = nullptr;
  m_rebootRow = {};
  m_rootRow = {};
  m_failedRow = {};
  m_journalRow = {};
  m_upkeepSummary = nullptr;
  m_updateRow = {};
  m_orphansRow = {};
  m_cacheRow = {};
  m_lastUpdate = nullptr;
  m_inboxFrame = nullptr;
  m_noteRows = {};
  m_clipRows = {};
  m_inboxDetail = nullptr;
  m_clipFull = nullptr;
  m_todayFrame = nullptr;
  m_eventRows = {};
  m_taskRows = {};
  m_weatherLine = nullptr;
  m_fleetFrame = nullptr;
  m_fleetRows = {};
  m_fleetDetail = nullptr;
  m_togglesFrame = nullptr;
  m_togglesList = nullptr;
  m_timerIdleRow = nullptr;
  m_timerIdleTime = nullptr;
  m_timerStatus = nullptr;
  m_timerStartKey = {};
  m_timerPresetKeys = {};
  m_timerRunBox = nullptr;
  m_timerTimeBlock = nullptr;
  m_timerTime = nullptr;
  m_timerLit = false;
  m_timerExpanded = false;
  m_timerControlKeys = {};
  m_systemFrame = nullptr;
  m_powerKeys = {};
  m_systemDetail = nullptr;
  m_sessionButton = nullptr;
  // Keep Shortcut instances alive; drop only the UI pointers destroyed with the scene.
  for (auto& pad : m_shortcutPads) {
    if (pad.shortcut != nullptr) {
      pad.shortcut->onPanelClose();
    }
    pad.row = {};
  }
}

void HomeTab::sync() {
  collectOutcomes();
  refreshLive(false);
  runScriptJob(m_notesJob, kNotesScript, false, std::chrono::seconds{5});
  syncHost();
  syncUpkeep();
  syncInbox();
  syncToday();
  runScriptJob(m_fleetJob, kFleetScript, false);
  syncFleet();
  syncShortcuts();
  syncTimer();
}

void HomeTab::syncAvatar(Renderer& renderer) {
  if (m_avatar == nullptr || m_avatarInitial == nullptr || m_config == nullptr) {
    return;
  }
  const auto showFallback = [this, &renderer](bool fallback) {
    if (fallback) {
      m_avatar->clear(renderer);
    }
    m_avatar->setVisible(!fallback);
    m_avatar->setParticipatesInLayout(!fallback);
    m_avatarInitial->setVisible(fallback);
    m_avatarInitial->setParticipatesInLayout(fallback);
    if (m_avatarHint != nullptr) {
      m_avatarHint->setVisible(fallback);
      m_avatarHint->setParticipatesInLayout(fallback);
    }
  };

  const std::string path = shell::avatarDisplayPath(m_accounts, m_config->config());
  if (path.empty()) {
    m_avatarPath.clear();
    showFallback(true);
    return;
  }
  m_avatarPath = path;
  const int targetSize = std::max(1, static_cast<int>(std::round(m_avatarSize)));
  const bool ready = m_asyncTextures != nullptr
      ? m_avatar->setSourceFileAsync(renderer, *m_asyncTextures, path, targetSize, true)
      : m_avatar->setSourceFile(renderer, path, targetSize, true);
  // An async load keeps the (still empty) image in place instead of the "click to set" fallback: it hid the image
  // until a later update pass, so a texture that finished after this call could stay invisible. A file that is gone
  // still gets the fallback.
  std::error_code fileError;
  showFallback(!ready && (m_asyncTextures == nullptr || !std::filesystem::exists(path, fileError)));
}

void HomeTab::chooseAvatar() {
  if (m_config == nullptr || !process::commandExists("zenity") || m_avatarPickerBusy->exchange(true)) {
    return;
  }
  std::vector<std::string> args{
      "zenity", "--file-selection", "--title=Choose profile picture",
      "--file-filter=Images | *.png *.jpg *.jpeg *.webp *.gif",
  };
  if (!m_avatarPath.empty()) {
    args.push_back("--filename=" + m_avatarPath);
  }
  // The callback runs on a worker thread and may outlive the panel (even this tab), so it captures only the
  // long-lived services and the shared busy flag, and applies the result on the main loop.
  ConfigService* config = m_config;
  AccountsService* accounts = m_accounts;
  auto busy = m_avatarPickerBusy;
  const bool started = process::runAsync(
      args,
      process::RunCallbacks{
          .onExit =
              [config, accounts, busy](process::RunResult result) {
                std::string picked = result.exitCode == 0 ? std::move(result.out) : std::string{};
                DeferredCall::callLater([config, accounts, busy, picked = std::move(picked)]() {
                  busy->store(false);
                  if (picked.empty()) {
                    return;
                  }
                  if (shell::applyAvatarPath(accounts, config, picked).success()) {
                    PanelManager::instance().refresh();
                  }
                });
              },
      }
  );
  if (!started) {
    m_avatarPickerBusy->store(false);
  }
}

void HomeTab::clearAvatar() {
  if (m_config == nullptr) {
    return;
  }
  if (shell::applyAvatarPath(m_accounts, m_config, "").success()) {
    PanelManager::instance().refresh();
  }
}
