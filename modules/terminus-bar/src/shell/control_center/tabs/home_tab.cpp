// Home dashboard tab: the host, upkeep, inbox, toggles, today and fleet tiles and the system strip, each a btop-style
// box fed by background shell scripts and the shell's services. See home_tab.h for the layout.

#include "shell/control_center/tabs/home_tab.h"

#include "calendar/calendar_service.h"
#include "calendar/calendar_types.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "idle/idle_inhibitor.h"
#include "notification/notification_manager.h"
#include "render/core/async_texture_cache.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "scripting/plugin_id.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/control_center/term_frame.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "shell/profile/avatar_path.h"
#include "system/distro_info.h"
#include "system/gamma_service.h"
#include "system/hardware_info.h"
#include "system/health.h"
#include "system/weather_service.h"
#include "tasks/task_service.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "util/secret_shape.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/statvfs.h>
#include <utility>
#include <wayland-client-protocol.h>

using namespace control_center;

namespace {

  using term_frame::Tone;

  constexpr float kTimerFontSize = 28.0F;
  // The dashboard's row height at 14px text.
  constexpr float kHomeRowHeight = 20.0F;
  constexpr float kFallbackBodyHeight = 600.0F;   // px at scale 1, until the panel reports its height
  constexpr float kSystemDetailWidthShare = 0.4F; // of the system strip, for the failure line in its middle
  constexpr float kAvatarHintScale = 0.85F;       // of the caption size: the hint wraps in the small square

  constexpr long long kSecondsPerMinute = 60;
  constexpr long long kSecondsPerHour = 3600;
  constexpr long long kSecondsPerDay = 86400;
  constexpr int kDaysPerYearBound = 400; // above 366, so year * bound + day-of-year is unique per day
  constexpr double kBytesPerGiB = 1024.0 * 1024.0 * 1024.0;

  constexpr std::chrono::seconds kArmTimeout{5};
  constexpr std::chrono::seconds kPendingGiveUp{6 * 3600};
  constexpr std::chrono::seconds kTerminalStartWindow{5};
  constexpr std::chrono::seconds kActionPollInterval{2};
  constexpr std::chrono::seconds kScriptTimeout{180};
  constexpr std::chrono::seconds kScriptRefreshInterval{60};
  constexpr std::chrono::seconds kNotesRefreshInterval{5};
  constexpr std::chrono::minutes kUpkeepInterval{30};
  constexpr std::chrono::seconds kTimerMin{60};
  constexpr std::chrono::seconds kTimerMax{99 * 60};
  constexpr std::chrono::milliseconds kTimerTickInterval{250};

  constexpr long long kFleetStaleSeconds = 90;
  constexpr long long kMinValidEpochSeconds = 100'000'000; // anything earlier is tailscale's "never seen"
  constexpr std::size_t kMaxToggleRows = 6;
  constexpr std::size_t kClipFullChars = 240;
  constexpr std::size_t kUnitListChars = 22;    // the failed-units list fits this many characters
  constexpr std::size_t kUnitListMoreChars = 5; // room kept for " +NN"
  constexpr int kDefaultTimerGoal = 6;
  constexpr int kMinTimerGoal = 1;
  constexpr int kMaxTimerGoal = 12;
  constexpr const char* kSyncthingDefaultUrl = "http://127.0.0.1:8384";

  void openControlCenterTab(std::string_view tab) {
    PanelManager::instance().togglePanel("control-center", PanelOpenRequest{.context = tab});
  }

  std::chrono::seconds ceilSeconds(std::chrono::milliseconds ms) { return std::chrono::ceil<std::chrono::seconds>(ms); }

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

  std::tm localTime(std::chrono::system_clock::time_point at) {
    const std::time_t t = std::chrono::system_clock::to_time_t(at);
    std::tm local{};
    localtime_r(&t, &local);
    return local;
  }

  // "07:42" in local time.
  std::string clockText(std::chrono::system_clock::time_point at) {
    const std::tm local = localTime(at);
    char buffer[8];
    return std::strftime(buffer, sizeof(buffer), "%H:%M", &local) > 0 ? std::string(buffer) : std::string{};
  }

  // Cheap and fast facts, refreshed about once a minute while the dashboard is open: `key=value` lines. The kernel
  // is compared by package base: the running release against the newest installed one of the same flavour.
  constexpr const char* kLiveScript = R"SH(
sh=$(getent passwd "$(id -un)" | cut -d: -f7)
v=$("$sh" --version 2>/dev/null | grep -oE '[0-9]+(\.[0-9]+)+' | head -1)
echo "shell=${sh##*/} $v"
echo "wm=Hyprland $(hyprctl version -j 2>/dev/null | jq -r '.version // empty')"
echo "packages=$(pacman -Qq 2>/dev/null | wc -l) · $(flatpak list --app 2>/dev/null | wc -l) flatpak"
hyprctl monitors -j 2>/dev/null | jq -r '"outputs=\(length)", (sort_by(.id)|.[0]|"display=\(.width)x\(.height) @ \(.refreshRate|round)Hz")'
run=$(uname -r)
case $run in *-zen*) pk=linux-zen;; *-lts*) pk=linux-lts;; *-hardened*) pk=linux-hardened;; *-rt*) pk=linux-rt;; *) pk=linux;; esac
inst=
for d in /usr/lib/modules/*/; do
  d=${d%/}
  base=
  read -r base 2>/dev/null < "$d/pkgbase"
  [ "$base" = "$pk" ] && inst=${d##*/}
done
echo "kernel_run=$run"
echo "kernel_inst=${inst:-$run}"
if [ -d "/usr/lib/modules/$run" ]; then echo "reboot=0"; else echo "reboot=1"; fi
echo "failed=$({ systemctl --failed --no-legend --plain; systemctl --user --failed --no-legend --plain; } 2>/dev/null | awk 'NF{sub(/\.service$/, "", $1); printf "%s%s", sep, $1; sep=","}')"
seen=
read -r seen 2>/dev/null < "${XDG_STATE_HOME:-$HOME/.local/state}/terminus-est/dashboard-journal-seen"
echo "journal_total=$(journalctl -b -p err -q --no-pager -o json 2>/dev/null | wc -l)"
if [ -n "$seen" ]; then
  echo "journal_new=$(journalctl -b -p err -q --no-pager -o json --since "@$seen" 2>/dev/null | wc -l)"
else
  echo "journal_new=0"
fi
)SH";

  // The slow package check (checkupdates and yay -Qua take seconds), refreshed every 30 minutes and on open.
  // `repo` and `aur` are printed only when the tool exists and ran: checkupdates exits 0 with updates, 2 with none
  // and anything else on failure (offline, no database); the AUR helper exits 0 or 1 when it ran. A missing count
  // reads as unknown, never as "up to date".
  // `last` is the last full system upgrade in pacman.log, 0 when it has none.
  constexpr const char* kUpkeepScript = R"SH(
if command -v checkupdates >/dev/null 2>&1; then
  out=$(checkupdates 2>/dev/null)
  rc=$?
  if [ "$rc" -eq 0 ]; then echo "repo=$(printf '%s\n' "$out" | grep -c .)"; elif [ "$rc" -eq 2 ]; then echo "repo=0"; fi
fi
for helper in yay paru; do
  if command -v "$helper" >/dev/null 2>&1; then
    out=$("$helper" -Qua 2>/dev/null)
    rc=$?
    if [ "$rc" -le 1 ]; then echo "aur=$(printf '%s\n' "$out" | grep -c .)"; fi
    break
  fi
done
cache=$(du -sb /var/cache/pacman/pkg 2>/dev/null)
echo "cache=${cache%%[[:space:]]*}"
echo "orphans=$(pacman -Qdtq 2>/dev/null | wc -l)"
l=$(grep 'starting full system upgrade' /var/log/pacman.log 2>/dev/null | tail -1)
l=${l#\[}
l=${l%%\]*}
if [ -n "$l" ]; then echo "last=$(date -d "$l" +%s 2>/dev/null || echo 0)"; else echo "last=0"; fi
)SH";

  // The notifications still on screen in mako, which owns org.freedesktop.Notifications on this desktop, so the
  // bar's own history never sees them: `count=N` and up to three `note_N=id<TAB>summary`, newest first. Mako has no
  // read state; a notification that is not dismissed counts as unread.
  constexpr const char* kNotesScript = R"SH(
makoctl list -j 2>/dev/null | jq -r '[.[]|select((.summary // "") != "" or (.body // "") != "")]|sort_by(-.id)|"count=\(length)", (.[:8]|to_entries[]|(.value.app_name // "") as $a|((.value.summary // .value.body // "")|gsub("[\\n\\r\\t]";" ")) as $s|"note_\(.key+1)=\(.value.id)\t\(if $a == "" then $s else "\($a): \($s)" end)")' 2>/dev/null
)SH";

  // The fleet checks, each with its own short timeout; a check that fails prints nothing for its key and the row
  // says "unknown". Tailscale comes from `tailscale status --json`, as the network panel reads it; one jq run turns it
  // into the lines below (`server_up` only tells the shell whether to ping). Syncthing's REST API key is read from its
  // own config.xml inside this script and handed to curl on stdin, so it is never an argument, never printed and never
  // kept by the bar.
  constexpr const char* kFleetScript = R"SH(
if command -v tailscale >/dev/null 2>&1; then
  ts=$(timeout 3 tailscale status --json 2>/dev/null)
  if [ -n "$ts" ]; then
    summary=$(printf '%s' "$ts" | jq -r '
      [.Peer[]?] as $peers
      | [$peers[] | select(.HostName == "server")] as $servers
      | "ts_state=\(.BackendState // "unknown")",
        "ts_total=\($peers | length)",
        "ts_online=\([$peers[] | select(.Online)] | length)",
        ([$peers[] | select(.HostName != "server")] | sort_by(.HostName)
          | "peer_count=\(length)",
            (to_entries[] | "peer_\(.key + 1)=\(.value.HostName)\t\(if .value.Online then "online" else "offline" end)\t\((.value.LastSeen // "") | try (sub("\\.[0-9]+Z$"; "Z") | fromdate) catch 0)")),
        "server=\(if ($servers | length) == 0 then "none" elif $servers[0].Online then "online" else "offline" end)",
        "server_up=\([$servers[] | select(.Online)] | length)"')
    printf '%s\n' "$summary"
    case $summary in
      *server_up=1) echo "server_ms=$(timeout 5 tailscale ping -c 1 --timeout 3s server 2>/dev/null | sed -n '/ in [0-9.]*ms/{s/.* in \([0-9.]*\)ms.*/\1/p;q;}')" ;;
    esac
  else
    echo "ts_state=unknown"
    echo "server=unknown"
  fi
else
  echo "tailscale=absent"
fi
cfg="${XDG_STATE_HOME:-$HOME/.local/state}/syncthing/config.xml"
if [ -r "$cfg" ] && command -v syncthing >/dev/null 2>&1; then
  key=$(sed -n 's:.*<apikey>\(.*\)</apikey>.*:\1:p;T;q' "$cfg")
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

  // The value of a `key=value` line in a job's output, empty when absent. The view lasts until the job's next output.
  std::string_view scriptValue(const ScriptJob& job, std::string_view key) {
    const auto found = job.values.find(key);
    return found == job.values.end() ? std::string_view{} : std::string_view{found->second};
  }

  long long numberValue(const ScriptJob& job, std::string_view key, long long fallback) {
    const std::string_view text = scriptValue(job, key);
    long long value = fallback; // left alone when the text is empty or not a number
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
  }

  // Runs `script` on a worker thread and applies its output on the main loop; the callback keeps only the
  // shared job, so it is safe if the panel or tab is gone. A repeated request within `minAge` is ignored unless forced.
  void runScriptJob(
      const std::shared_ptr<ScriptJob>& job, const char* script, bool force,
      std::chrono::seconds minAge = kScriptRefreshInterval
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
                      job->setOutput(std::move(out));
                      job->loaded = true;
                    }
                    PanelManager::instance().refresh();
                  });
                },
        },
        process::RunOptions{.timeout = kScriptTimeout}
    );
    if (!started) {
      job->running = false;
    }
  }

  // "2h 5m ago", "1d 7h ago": the time since `seconds` ago, in its two largest units.
  std::string formatAgo(long long seconds) {
    if (seconds < kSecondsPerMinute) {
      return "just now";
    }
    const long long minutes = seconds / kSecondsPerMinute;
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
    if (seconds < kSecondsPerMinute) {
      return "now";
    }
    if (seconds < kSecondsPerHour) {
      return std::format("{}m", seconds / kSecondsPerMinute);
    }
    if (seconds < kSecondsPerDay) {
      return std::format("{}h", seconds / kSecondsPerHour);
    }
    return std::format("{}d", seconds / kSecondsPerDay);
  }

  std::string formatUptime(std::chrono::seconds up) {
    const auto total = up.count();
    const auto days = total / kSecondsPerDay;
    const auto hours = (total % kSecondsPerDay) / kSecondsPerHour;
    const auto minutes = (total % kSecondsPerHour) / kSecondsPerMinute;
    if (days > 0) {
      return std::format("{}d {}h {}m", days, hours, minutes);
    }
    if (hours > 0) {
      return std::format("{}h {}m", hours, minutes);
    }
    return std::format("{}m", minutes);
  }

  // "7.3.5 → 7.3.6" when the versions differ before their flavour suffix, else the full releases.
  std::string kernelDelta(std::string_view running, std::string_view installed) {
    const auto base = [](std::string_view release) { return release.substr(0, release.find('-')); };
    if (base(running) != base(installed)) {
      return std::format("{} → {}", base(running), base(installed));
    }
    return std::format("{} → {}", running, installed);
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
      const std::size_t reserve = shown + 1 < names.size() ? kUnitListMoreChars : 0;
      if (shown > 0 && next + reserve > kUnitListChars) {
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
    constexpr std::array<std::pair<std::string_view, std::string_view>, 6> kNames{{
        {"keyboard_sounds", "keyboard sounds"},
        {"notification", "do not disturb"},
        {"caffeine", "keep awake"},
        {"nightlight", "night light"},
        {"mic_mute", "mic mute"},
        {"shutdown_timer", "auto-off"},
    }};
    for (const auto& [id, name] : kNames) {
      if (shortcut.id() == id) {
        return std::string(name);
      }
    }
    return StringUtils::toLower(shortcut.displayLabel());
  }

  std::string formatOutputs(long long count) { return std::format("{} {}", count, count == 1 ? "output" : "outputs"); }

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

  // ---- tables and small helpers of the tiles ----

  // Row order of the host tile; the priority tables say which rows go first when the tile is short.
  enum HostLive : std::size_t { LiveUptime, LiveKernel, LiveSession };
  enum HostInfo : std::size_t { InfoOs, InfoShell, InfoWm, InfoPkgs, InfoGpu, InfoDisplay };
  constexpr std::array<std::string_view, 3> kHostLiveKeys{"up", "kernel", "session"};
  constexpr std::array<std::string_view, 6> kHostInfoKeys{"os", "shell", "wm", "pkgs", "gpu", "display"};
  constexpr std::array<HostLive, 3> kLivePriority{LiveKernel, LiveSession, LiveUptime};
  constexpr std::array<HostInfo, 6> kInfoPriority{InfoOs, InfoGpu, InfoDisplay, InfoPkgs, InfoShell, InfoWm};

  // The session keys; `action` is the verb of `terminus-bar msg session`. Lock and sleep run at once, the rest ask
  // for a confirming second press; the last two sit apart on capital letters (Shift held) in the warning tone.
  struct PowerKey {
    const char* word;
    std::size_t hot;
    bool warning;
    const char* action;
  };
  constexpr std::array<PowerKey, 5> kPowerKeys{{
      {"lock", 0, false, "lock"},
      {"sleep", 0, false, "suspend"},
      {"logout", 2, false, "logout"},
      {"R reboot", 0, true, "reboot"},
      {"D shutdown", 0, true, "shutdown"},
  }};
  constexpr std::size_t kFirstConfirmedKey = 2;
  constexpr std::size_t kFirstDangerKey = 3;

  // Sets the case bit: folds ASCII letters (keysyms and characters alike) together.
  constexpr std::uint32_t lowerKeysym(std::uint32_t sym) noexcept { return sym | 0x20U; }

  // Whether `key`'s hotkey letter is `letter`; a capital hotkey needs Shift, a lower-case one must not have it.
  bool hotkeyMatches(const term_frame::Keyword& key, std::uint32_t letter, bool shifted, bool upper) {
    if (!key.activate || key.button == nullptr || key.hot >= key.word.size()) {
      return false;
    }
    const char hot = key.word[key.hot];
    const bool capital = hot >= 'A' && hot <= 'Z';
    return lowerKeysym(static_cast<std::uint32_t>(hot)) == letter && (capital ? shifted || upper : !shifted);
  }

  // Posts an action's outcome from a worker thread to the main loop, where collectOutcomes() shows it.
  void reportOutcome(const std::shared_ptr<ActionMailbox>& mailbox, ActionOutcome outcome) {
    DeferredCall::callLater([mailbox, outcome = std::move(outcome)]() mutable {
      mailbox->done.push_back(std::move(outcome));
      PanelManager::instance().refresh();
    });
  }

  // The tile grid's geometry in px: three rows of two tiles share the height the system strip leaves, so both tiles
  // of a row have the same top and bottom edge.
  struct GridMetrics {
    float width;
    float height;
    float margin;
    float gridWidth;
    float leftWidth;
    float rightWidth;
    float stripHeight;
    float inner;      // text width inside a tile
    float stripInner; // text width inside the system strip
    std::array<float, 3> rowHeights;
  };

  GridMetrics gridMetrics(float contentWidth, float bodyHeight, float scale) {
    const float gap = Style::spaceSm * scale;
    GridMetrics metrics{};
    metrics.width = std::max(1.0F, contentWidth);
    metrics.height = std::max(1.0F, bodyHeight > 0.0F ? bodyHeight : kFallbackBodyHeight * scale);
    metrics.margin = gap;
    metrics.gridWidth = std::max(1.0F, metrics.width - metrics.margin);
    metrics.leftWidth = std::max(1.0F, std::floor((metrics.gridWidth - gap) * 0.5F));
    metrics.rightWidth = std::max(1.0F, metrics.gridWidth - gap - metrics.leftWidth);
    metrics.stripHeight = term_frame::btopChrome(scale) + kHomeRowHeight * scale;
    const float available = std::max(3.0F, metrics.height - metrics.stripHeight - 3.0F * gap);
    const float baseHeight = std::floor(available / 3.0F);
    metrics.rowHeights = {baseHeight, baseHeight, available - 2.0F * baseHeight};
    const float inset = term_frame::boxInset(scale);
    metrics.inner = std::max(1.0F, std::min(metrics.leftWidth, metrics.rightWidth) - 2.0F * inset);
    metrics.stripInner = std::max(1.0F, metrics.gridWidth - 2.0F * inset);
    return metrics;
  }

  // Takes the instance for `type` out of `pool`; null when there is none.
  std::unique_ptr<Shortcut> takeShortcut(std::vector<std::unique_ptr<Shortcut>>& pool, std::string_view type) {
    const auto found = std::ranges::find_if(pool, [type](const auto& shortcut) {
      return shortcut != nullptr && shortcut->id() == type;
    });
    if (found == pool.end()) {
      return nullptr;
    }
    auto shortcut = std::move(*found);
    pool.erase(found);
    return shortcut;
  }

  // Writes `describe(item)` (a name and a state text) into the first `limit` rows of a pool and returns which rows have
  // an item. Rows past `limit` cannot fit the tile, so they keep their old text until the tile grows.
  template <std::size_t N, typename Items, typename Describe>
  std::array<bool, N>
  fillRows(std::array<term_frame::ToggleRow, N>& rows, const Items& items, std::size_t limit, Describe describe) {
    std::array<bool, N> wanted{};
    for (std::size_t i = 0; i < N; ++i) {
      wanted[i] = i < items.size();
      if (!wanted[i] || i >= limit || rows[i].button == nullptr) {
        continue;
      }
      const auto [name, state] = describe(items[i]);
      setRowName(rows[i], name);
      term_frame::setToggleRow(rows[i], true, false, state);
    }
    return wanted;
  }

  // "1.4 GiB", or "312 MiB" below one GiB.
  std::string formatCacheSize(long long bytes) {
    const double gib = static_cast<double>(bytes) / kBytesPerGiB;
    return gib >= 1.0 ? std::format("{:.1f} GiB", gib) : std::format("{:.0f} MiB", gib * 1024.0);
  }

  // Header slot of the upkeep tile: how many of the warnings need action.
  void setUpkeepHeader(term_frame::BtopFrame& frame, bool loaded, int actions) {
    if (!loaded || actions == 0) {
      frame.setInfo(loaded ? "all clear" : "checking…");
      frame.setInfoColor(colorSpecFromRole(ColorRole::OnSurface));
      return;
    }
    frame.setInfo(std::format("! {} {} action", actions, actions == 1 ? "needs" : "need"));
    frame.setInfoColor(colorSpecFromRole(ColorRole::Warning));
  }

  // ---- inbox ----

  std::string ageShort(std::chrono::system_clock::time_point now, std::chrono::system_clock::time_point then) {
    return formatAgeShort(std::chrono::duration_cast<std::chrono::seconds>(now - then).count());
  }

  struct NoteLine {
    std::string title;
    std::string age;
    std::string makoId; // empty for the bar's own notifications
  };

  // The newest `maxLines` unread notifications: the bar's own history (its newest entry is at the back), then what
  // mako still shows. `unread` receives the number of all of them.
  std::vector<NoteLine> collectNotes(
      NotificationManager* manager, const ScriptJob& makoJob, std::size_t maxLines,
      std::chrono::system_clock::time_point now, std::size_t& unread
  ) {
    std::vector<NoteLine> lines;
    if (manager != nullptr) {
      const auto& history = manager->history();
      for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (it->seen) {
          continue;
        }
        ++unread;
        if (lines.size() >= maxLines) {
          continue;
        }
        const Notification& note = it->notification;
        const std::string headline = note.summary.empty() ? note.body : note.summary;
        NoteLine line{.title = oneLine(note.appName.empty() ? headline : note.appName + ": " + headline)};
        if (note.receivedWallClock.has_value()) {
          line.age = ageShort(now, *note.receivedWallClock);
        }
        lines.push_back(std::move(line));
      }
    }
    const long long makoCount = numberValue(makoJob, "count", 0);
    unread += static_cast<std::size_t>(std::max(0LL, makoCount));
    for (std::size_t n = 1; n <= maxLines && lines.size() < maxLines && static_cast<long long>(n) <= makoCount; ++n) {
      const std::string_view entry = scriptValue(makoJob, std::format("note_{}", n));
      const std::size_t tab = entry.find('\t');
      if (tab == std::string_view::npos) {
        continue;
      }
      lines.push_back(
          {.title = oneLine(entry.substr(tab + 1)), .age = "dismiss", .makoId = std::string(entry.substr(0, tab))}
      );
    }
    return lines;
  }

  // The row text of a clipboard entry. Entries that look like secrets are masked; their text shows only while their
  // row has the focus, in the full-text line.
  std::string clipTitle(const ClipboardEntry& entry) {
    if (entry.isImage()) {
      return "[image]";
    }
    if (secret_shape::looksSensitive(entry.textPreview)) {
      return std::string(secret_shape::kMaskedPreview);
    }
    std::string title = oneLine(entry.textPreview);
    return title.empty() ? "[data]" : title;
  }

  // ---- today ----

  // A number that is equal for the instants of one local day and differs between days.
  int localDay(std::chrono::system_clock::time_point at) {
    const std::tm local = localTime(at);
    return local.tm_year * kDaysPerYearBound + local.tm_yday;
  }

  // Events that have not ended yet, soonest first.
  std::vector<const CalendarEvent*>
  upcomingEvents(CalendarService* calendar, std::chrono::system_clock::time_point now) {
    std::vector<const CalendarEvent*> upcoming;
    if (calendar == nullptr || !calendar->enabled() || !calendar->hasData()) {
      return upcoming;
    }
    for (const CalendarEvent& event : calendar->snapshot().events) {
      if (event.end > now) {
        upcoming.push_back(&event);
      }
    }
    std::ranges::sort(upcoming, [](const CalendarEvent* a, const CalendarEvent* b) { return a->start < b->start; });
    return upcoming;
  }

  // "all day", "now", "07:42", "tmrw 07:42" or "Fri 07:42": when an event starts; `today` is a localDay().
  std::string eventWhen(const CalendarEvent& event, std::chrono::system_clock::time_point now, int today) {
    if (event.allDay) {
      return "all day";
    }
    if (event.start <= now) {
      return "now";
    }
    const std::string clock = clockText(event.start);
    const int days = localDay(event.start) - today;
    if (days == 0) {
      return clock;
    }
    if (days == 1) {
      return "tmrw " + clock;
    }
    char weekday[8];
    const std::tm local = localTime(event.start);
    std::strftime(weekday, sizeof(weekday), "%a", &local);
    return std::string(weekday) + " " + clock;
  }

  // "in 45m" or "in 2h 5m" until the first timed event still ahead today; empty when there is none.
  std::string nextEventCountdown(
      const std::vector<const CalendarEvent*>& upcoming, std::chrono::system_clock::time_point now, int today
  ) {
    for (const CalendarEvent* event : upcoming) {
      if (event->allDay || event->start <= now || localDay(event->start) != today) {
        continue;
      }
      const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(event->start - now).count();
      return minutes < 60 ? std::format("in {}m", minutes + 1) : std::format("in {}h {}m", minutes / 60, minutes % 60);
    }
    return {};
  }

  std::vector<const TaskItem*> openTasks(TaskService* tasks) {
    std::vector<const TaskItem*> open;
    if (tasks == nullptr) {
      return open;
    }
    for (const TaskItem& item : tasks->snapshot().items) {
      if (!item.completed) {
        open.push_back(&item);
      }
    }
    return open;
  }

  // "today", "late" or "MM-DD" from a task's YYYY-MM-DD due date; empty without one.
  std::string taskDue(const std::string& dueDate, const std::string& todayKey) {
    constexpr std::size_t kYearPrefixChars = 5; // "YYYY-"
    if (dueDate.empty()) {
      return {};
    }
    if (dueDate == todayKey) {
      return "today";
    }
    return dueDate < todayKey ? "late" : dueDate.substr(std::min(kYearPrefixChars, dueDate.size()));
  }

  // One line: now, the sky, today's range.
  std::string weatherText(WeatherService* weather) {
    if (weather == nullptr || !weather->snapshot().valid) {
      return "weather --";
    }
    const WeatherSnapshot& snapshot = weather->snapshot();
    std::string text = std::format(
        "{:.0f}° {}", snapshot.current.temperatureC,
        StringUtils::toLower(WeatherService::shortDescriptionForCode(snapshot.current.weatherCode))
    );
    if (!snapshot.forecastDays.empty()) {
      text += std::format(
          " · {:.0f}°–{:.0f}°", snapshot.forecastDays.front().temperatureMinC,
          snapshot.forecastDays.front().temperatureMaxC
      );
    }
    return text;
  }

  // ---- timer ----

  // `■■□□□□ 2/6`, plus ` +1` for each session beyond the goal: filled squares for counted sessions, empty up to it.
  std::string sessionSquares(int sessions, int goal) {
    const int filled = std::min(sessions, goal);
    std::string squares;
    for (int i = 0; i < goal; ++i) {
      squares += i < filled ? "■" : "□";
    }
    squares += std::format(" {}/{}", filled, goal);
    if (sessions > goal) {
      squares += std::format(" +{}", sessions - goal);
    }
    return squares;
  }

  // Retitles a keyword and its highlighted letter only when one of them changed.
  void syncKeyword(term_frame::Keyword& key, std::string_view word, std::size_t hot, bool block) {
    if (key.word == word && key.block == block && key.hot == hot) {
      return;
    }
    key.word = std::string(word);
    key.hot = hot;
    key.block = block;
    key.blockText = key.word;
    term_frame::refreshKeyword(key);
  }

  // ---- fleet ----

  // One row of the fleet tile before it is drawn.
  struct FleetLine {
    bool show = true;
    bool down = false;
    bool live = false; // read straight from a service, never stale
    std::string value = "unknown";
  };

  FleetLine serverLine(const ScriptJob& job) {
    FleetLine line;
    const std::string_view server = scriptValue(job, "server");
    if (scriptValue(job, "tailscale") == "absent" || server == "none") {
      line.show = false;
    } else if (server == "online") {
      const std::string_view ms = scriptValue(job, "server_ms");
      line.value = ms.empty() ? std::string{"online"} : std::format("online · {} ms", ms);
    } else if (server == "offline") {
      line.down = true;
      line.value = "offline";
    }
    return line;
  }

  FleetLine syncthingLine(const ScriptJob& job) {
    FleetLine line;
    const std::string_view syncthing = scriptValue(job, "syncthing");
    if (syncthing == "absent") {
      line.show = false;
    } else if (syncthing == "ok") {
      line.value = "up to date";
    } else if (syncthing.starts_with("syncing:")) {
      line.value = std::format("syncing {}%", syncthing.substr(std::string_view{"syncing:"}.size()));
    } else if (syncthing == "out") {
      line.down = true;
      line.value = "out of sync";
    }
    return line;
  }

  // Radicale: the calendar service's own connection; no check of ours.
  FleetLine radicaleLine(CalendarService* calendar) {
    FleetLine line;
    if (calendar == nullptr || !calendar->enabled()) {
      line.show = false;
      return line;
    }
    line.live = true;
    switch (calendar->syncError()) {
    case CalendarService::SyncError::None:
      line.value = calendar->hasData() ? "reachable" : "unknown";
      break;
    case CalendarService::SyncError::NoCalendars:
      line.value = "reachable";
      break;
    case CalendarService::SyncError::Unreachable:
      line.down = true;
      line.value = "unreachable";
      break;
    case CalendarService::SyncError::AuthFailed:
      line.down = true;
      line.value = "auth failed";
      break;
    case CalendarService::SyncError::Failed:
      line.down = true;
      line.value = "sync failed";
      break;
    }
    return line;
  }

  FleetLine tailscaleLine(const ScriptJob& job) {
    FleetLine line;
    const std::string_view state = scriptValue(job, "ts_state");
    if (scriptValue(job, "tailscale") == "absent") {
      line.show = false;
    } else if (state == "Running") {
      line.value =
          std::format("{} of {} peers online", numberValue(job, "ts_online", 0), numberValue(job, "ts_total", 0));
    } else if (!state.empty() && state != "unknown") {
      line.down = true;
      line.value = StringUtils::toLower(state);
    }
    return line;
  }

  struct PeerText {
    std::string name;
    std::string status;
  };

  // The row text of a tailscale peer from its `name<TAB>online|offline<TAB>last-seen epoch seconds` line.
  PeerText peerText(std::string_view entry, long long nowSeconds) {
    const std::size_t first = entry.find('\t');
    const std::size_t second = first == std::string_view::npos ? first : entry.find('\t', first + 1);
    PeerText peer{.name = std::string(entry.substr(0, first)), .status = "unknown"};
    if (second == std::string_view::npos) {
      return peer;
    }
    long long lastSeen = 0;
    const std::string_view seen = entry.substr(second + 1);
    std::from_chars(seen.data(), seen.data() + seen.size(), lastSeen);
    if (entry.substr(first + 1, second - first - 1) == "online") {
      peer.status = "online";
    } else if (lastSeen > kMinValidEpochSeconds) {
      const long long ago = std::max(0LL, nowSeconds - lastSeen);
      peer.status = ago < kSecondsPerHour ? std::format("last seen {}m", ago / kSecondsPerMinute)
          : ago < kSecondsPerDay          ? std::format("last seen {}h", ago / kSecondsPerHour)
                                          : std::format("last seen {}d", ago / kSecondsPerDay);
    } else {
      peer.status = "offline";
    }
    return peer;
  }

  // Header slot of the fleet tile. Stale answers are dimmed and carry their age.
  void setFleetHeader(
      term_frame::BtopFrame& frame, std::size_t down, std::size_t unknown, bool stale, const std::string& ageText
  ) {
    if (down > 0) {
      frame.setInfo(
          unknown > 0 ? std::format("! {} down · {} unknown", down, unknown) : std::format("! {} down", down)
      );
      frame.setInfoColor(colorSpecFromRole(stale ? ColorRole::OnSurfaceVariant : ColorRole::Warning));
    } else if (unknown > 0 || stale) {
      frame.setInfo(unknown > 0 ? std::format("{} unknown", unknown) : std::format("all ok{}", ageText));
      frame.setInfoColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    } else {
      frame.setInfo("all ok");
      frame.setInfoColor(colorSpecFromRole(ColorRole::OnSurface));
    }
  }

} // namespace

void ScriptJob::setOutput(std::string text) {
  out = std::move(text);
  values.clear();
  std::string_view rest = out;
  while (!rest.empty()) {
    const std::size_t end = std::min(rest.find('\n'), rest.size());
    const std::string_view line = rest.substr(0, end);
    rest.remove_prefix(std::min(end + 1, rest.size()));
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
      continue;
    }
    std::string_view value = line.substr(equals + 1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
      value.remove_suffix(1);
    }
    values.try_emplace(std::string(line.substr(0, equals)), value);
  }
}

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
  m_upkeepTimer.startRepeating(kUpkeepInterval, [this]() { refreshUpkeep(true); });
  loadTimerToday();
  // The last known fleet values, so the section is never empty while the first check runs.
  if (std::ifstream in(fleetPath()); in) {
    long long at = 0;
    in >> at;
    in.ignore(1, '\n');
    std::string rest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (at > 0 && !rest.empty()) {
      m_fleetAt = at;
      m_fleetJob->setOutput(std::move(rest));
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
  static_assert(kHostLiveKeys.size() == kHostLiveRows && kHostInfoKeys.size() == kHostInfoRows);
  auto box = term_frame::btopBox("¹", "host", scale, &m_hostFrame);
  auto avatar = createAvatarPicker(scale);
  auto identity = createHostIdentity(scale);
  box->addChild(
      ui::row(
          {.out = &m_hostHeader, .align = FlexAlign::Center, .gap = Style::spaceSm * scale}, std::move(avatar),
          std::move(identity)
      )
  );

  // The live rows and the static facts take turns in the same place, so opening `i` costs only the difference.
  auto liveBox = ui::column({.out = &m_hostLiveBox, .align = FlexAlign::Stretch, .gap = Style::spaceXs * scale});
  for (std::size_t i = 0; i < kHostLiveRows; ++i) {
    liveBox->addChild(term_frame::infoRow(kHostLiveKeys[i], scale, m_hostLive[i]));
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
  for (std::size_t i = 0; i < kHostInfoRows; ++i) {
    info->addChild(term_frame::infoRow(kHostInfoKeys[i], scale, m_hostInfoRows[i]));
    m_fixedRows.push_back(m_hostInfoRows[i].row);
  }
  setShown(info.get(), false);
  box->addChild(std::move(info));

  syncHost();
  return box;
}

// The profile picture, two rows tall: left click picks a new one, right click clears it; hover shows an accent frame.
// Without a picture it shows the user's initial and a hint.
std::unique_ptr<InputArea> HomeTab::createAvatarPicker(float scale) {
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
          .fontSize = Style::fontSizeCaption * scale * kAvatarHintScale,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
          .maxWidth = avatarInner,
          .maxLines = 2,
          .textAlign = TextAlign::Center,
      })
  );
  return ui::inputArea(
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
}

// Beside the avatar: the user name over the dim @host.
std::unique_ptr<Flex> HomeTab::createHostIdentity(float scale) {
  return ui::column(
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
    const std::string_view value = scriptValue(job, key);
    return std::string(value.empty() ? "--" : value);
  };

  const auto up = systemUptime();
  term_frame::setLabelTone(m_hostLive[LiveUptime].value, up.has_value() ? formatUptime(*up) : "n/a", Tone::Normal);

  const std::string_view running = scriptValue(job, "kernel_run");
  const std::string_view installed = scriptValue(job, "kernel_inst");
  const bool kernelDiffers = !running.empty() && installed != running;
  term_frame::InfoRow& kernel = m_hostLive[LiveKernel];
  if (kernel.key != nullptr) {
    kernel.key->setText(kernelDiffers ? "! kernel" : "kernel"); // a leading "!" says "warning" without the colour
  }
  if (running.empty()) {
    term_frame::setLabelTone(kernel.value, kernelLabel(), Tone::Normal);
  } else if (kernelDiffers) {
    term_frame::setLabelTone(kernel.value, kernelDelta(running, installed), Tone::Warning);
  } else {
    term_frame::setLabelTone(kernel.value, running, Tone::Normal);
  }

  const std::string_view wm = scriptValue(job, "wm");
  const long long outputs = numberValue(job, "outputs", -1);
  term_frame::setLabelTone(
      m_hostLive[LiveSession].value,
      wm.empty() || outputs < 0 ? std::string{"--"} : std::format("{} · {}", wm, formatOutputs(outputs)), Tone::Normal
  );

  const std::array<std::string, kHostInfoRows> values{m_osLabel,  fact("shell"),  fact("wm"), fact("packages"),
                                                      m_gpuLabel, fact("display")}; // HostInfo order
  for (std::size_t i = 0; i < kHostInfoRows; ++i) {
    term_frame::setLabelTone(m_hostInfoRows[i].value, values[i], Tone::Normal);
  }

  // The tile never changes height: the rows that fit are shown, by priority. Open, the static facts replace the
  // live rows and the avatar; the key row keeps its line.
  bool shapeChanged = setShown(m_hostInfoBox, m_infoOpen);
  shapeChanged |= setShown(m_hostLiveBox, !m_infoOpen);
  std::vector<FitItem> items;
  if (m_infoOpen) {
    for (const HostInfo index : kInfoPriority) {
      items.push_back({m_hostInfoRows[index].row, 1, true});
    }
    items.push_back({m_hostHeader, 2, false});
  } else {
    items.push_back({m_hostHeader, 2, true});
    for (const HostLive index : kLivePriority) {
      items.push_back({m_hostLive[index].row, 1, true});
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
    setShown(row->row, false);
    m_fixedRows.push_back(row->row);
  }
  box->addChild(
      term_frame::actionRow(
          "! failed", 2, scale, m_failedRow,
          [this]() {
            launchInTerminal(
                "failed", "systemctl", "systemctl --failed --no-pager; systemctl --user --failed --no-pager"
            );
          },
          nullptr
      )
  );
  box->addChild(
      term_frame::actionRow(
          "! journal", 2, scale, m_journalRow,
          [this]() {
            markJournalSeen();
            launchInTerminal("journal", "journalctl", "journalctl -b -p err");
          },
          nullptr
      )
  );
  for (term_frame::ActionRow* row : {&m_failedRow, &m_journalRow}) {
    setShown(row->group, false);
    m_fixedRows.push_back(row->row);
    m_detailLabels.push_back(row->detail);
  }

  box->addChild(dimCaption(&m_upkeepSummary, scale));

  // Housekeeping: each key shows what it will do and asks before it runs.
  box->addChild(
      term_frame::actionRow(
          "update", 0, scale, m_updateRow,
          [this]() {
            pressConfirmed(m_updateRow.key, &m_updateRow, "runs yay -Syu in a terminal", "", [this]() {
              launchInTerminal("update", "yay", "yay -Syu");
            });
          },
          leave(m_updateRow.key)
      )
  );
  box->addChild(
      term_frame::actionRow(
          "orphans", 0, scale, m_orphansRow,
          [this]() {
            const int count = static_cast<int>(numberValue(*m_upkeepJob, "orphans", -1));
            if (count <= 0) {
              clearArmed();
              if (term_frame::setActionDetail(
                      m_orphansRow, count == 0 ? "nothing to remove" : "not checked yet", Tone::Dim
                  )) {
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
      )
  );
  box->addChild(
      term_frame::actionRow(
          "cache", 0, scale, m_cacheRow,
          [this]() {
            pressConfirmed(
                m_cacheRow.key, &m_cacheRow, "paccache -rv keeps three versions of each package and deletes the rest",
                "", [this]() { launchInTerminal("cache", "paccache", "sudo paccache -rv"); }
            );
          },
          leave(m_cacheRow.key)
      )
  );
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
  const std::string_view running = scriptValue(live, "kernel_run");
  const std::string_view installed = scriptValue(live, "kernel_inst");
  const bool rebootNeeded =
      live.loaded && (scriptValue(live, "reboot") == "1" || (!running.empty() && running != installed));
  const health::Level rootLevel = health::disk("root", m_rootUsed * 100.0);
  const std::string_view failed = scriptValue(live, "failed");
  const int failedCount = failed.empty() ? 0 : static_cast<int>(std::ranges::count(failed, ',')) + 1;
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
    term_frame::setActionValue(
        m_failedRow, failedWarn ? unitList(failed) : "none now", failedWarn ? Tone::Warning : Tone::Dim
    );
  }
  if (showJournal) {
    term_frame::setActionValue(
        m_journalRow, std::format("{} new · {} total", journalNew, journalTotal),
        journalWarn ? Tone::Warning : Tone::Dim
    );
  }

  // What is fine collapses into one dim line.
  std::string summary;
  if (live.loaded && failedCount == 0) {
    summary = "failed 0";
  }
  if (live.loaded && !journalWarn) {
    summary += (summary.empty() ? "" : " · ") + std::format("errors {} seen", journalTotal);
  }
  term_frame::setLabelTone(m_upkeepSummary, summary, Tone::Dim);
  syncUpkeepHousekeeping();

  // Header slot: how many of the warnings need action.
  const int actions = (showReboot ? 1 : 0) + (showRoot ? 1 : 0) + (failedWarn ? 1 : 0) + (journalWarn ? 1 : 0);
  setUpkeepHeader(*m_upkeepFrame, live.loaded, actions);

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
      {m_lastUpdate, 1, !m_lastUpdate->text().empty()},
  };
  if (fitItems(items, m_capacity[TileUpkeep])) {
    relayout();
  }
}

// The housekeeping rows (updates, orphans, cache) and the last-upgrade line, from the slow package check.
void HomeTab::syncUpkeepHousekeeping() {
  const ScriptJob& slow = *m_upkeepJob;
  std::string updates = slow.running ? "checking…" : "--";
  Tone updateTone = Tone::Dim;
  if (slow.loaded) {
    const long long repoCount = numberValue(slow, "repo", -1);
    const long long aurCount = numberValue(slow, "aur", -1);
    const long long repo = std::max(0LL, repoCount);
    const long long aur = std::max(0LL, aurCount);
    if (repo + aur == 0) {
      // Zero only means up to date when both checks ran.
      if (repoCount == 0 && aurCount == 0) {
        updates = "up to date";
      }
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

  const long long cacheBytes = numberValue(slow, "cache", -1);
  term_frame::setActionValue(
      m_cacheRow, slow.loaded && cacheBytes >= 0 ? formatCacheSize(cacheBytes) : std::string{"--"}, Tone::Normal
  );

  const long long last = numberValue(slow, "last", 0);
  term_frame::setLabelTone(
      m_lastUpdate, last > 0 ? "updated " + formatAgo(std::time(nullptr) - static_cast<std::time_t>(last)) : "",
      Tone::Dim
  );
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
  box->addChild(
      ui::label({
          .out = &m_clipFull,
          .fontSize = term_frame::kBtopFontSize * scale,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
          .maxLines = 3,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  m_detailLabels.push_back(m_clipFull);
  box->addChild(
      ui::label({
          .out = &m_inboxDetail,
          .fontSize = term_frame::kBtopFontSize * scale,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::toneColor(Tone::Error),
          .maxLines = 3,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  m_detailLabels.push_back(m_inboxDetail);
  syncInbox();
  return box;
}

void HomeTab::syncInbox() {
  if (m_inboxFrame == nullptr) {
    return;
  }
  const auto now = std::chrono::system_clock::now();
  const std::size_t rowLimit = static_cast<std::size_t>(m_capacity[TileInbox]);

  // Newest unread first: the bar's own history, then what mako still shows. ● marks a notification.
  std::size_t unread = 0;
  const std::vector<NoteLine> notes = collectNotes(m_notifications, *m_notesJob, kInboxNotes, now, unread);
  const std::array<bool, kInboxNotes> noteWanted = fillRows(m_noteRows, notes, rowLimit, [](const NoteLine& note) {
    return std::pair{"● " + note.title, note.age};
  });
  for (std::size_t i = 0; i < kInboxNotes; ++i) {
    m_noteMako[i] = i < notes.size() ? notes[i].makoId : std::string{};
  }
  m_inboxFrame->setInfo(std::format("{} unread", unread));

  // The last clipboard entries, newest first. ▸ marks one.
  static const std::deque<ClipboardEntry> kNoClips;
  const std::deque<ClipboardEntry>& clips =
      m_services.clipboard != nullptr ? m_services.clipboard->history() : kNoClips;
  const std::array<bool, kInboxClips> clipWanted =
      fillRows(m_clipRows, clips, rowLimit, [&](const ClipboardEntry& clip) {
        const bool hasTime = clip.capturedAt.time_since_epoch().count() > 0;
        return std::pair{"▸ " + clipTitle(clip), hasTime ? ageShort(now, clip.capturedAt) : std::string{}};
      });
  std::string fullText; // the whole text of the focused clipboard row
  if (const InputArea* focused = inputDispatcher() != nullptr ? inputDispatcher()->focusedArea() : nullptr) {
    for (std::size_t i = 0; i < std::min(clips.size(), kInboxClips); ++i) {
      if (m_clipRows[i].button != nullptr && m_clipRows[i].button->inputArea() == focused && !clips[i].isImage()) {
        fullText = oneLine(clips[i].textPreview).substr(0, kClipFullChars);
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
  if (fitItems(items, capacity)) {
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
          .onExit = [mailbox = m_mailbox](process::RunResult result) {
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
  const std::size_t count = std::min(shortcuts.size(), kMaxToggleRows);

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

  // A plugin shortcut seeds its Luau runtime once, at construction. Plugin settings changed
  // since the last build means every reusable plugin instance is stale, so drop it and let
  // ShortcutRegistry::create() re-seed from the current config.
  const bool pluginsChanged = m_config != nullptr && !(m_config->config().plugins == m_lastPlugins);
  if (m_config != nullptr) {
    m_lastPlugins = m_config->config().plugins;
  }

  for (std::size_t i = 0; i < count; ++i) {
    const auto& sc = shortcuts[i];
    auto shortcut = takeShortcut(previous, sc.type);
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
    addShortcutRow(*list, std::move(shortcut), scale);
  }

  if (!m_shortcutPads.empty()) {
    box->addChild(std::move(list));
  } else {
    m_togglesList = nullptr;
  }
  return box;
}

void HomeTab::addShortcutRow(Flex& list, std::unique_ptr<Shortcut> shortcut, float scale) {
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
  if (auto* area = pad.row.button->inputArea(); area != nullptr) {
    area->setOnAxisHandler([this, padIdx](const InputArea::PointerData& data) -> bool {
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
  list.addChild(std::move(row));
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
    )
                             .count();
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
        term_frame::makeKeyword(m_timerPresetKeys[i], std::to_string(minutes), kNoHotkey, scale, [this, minutes]() {
          setTimerDuration(std::chrono::minutes{minutes});
        })
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
  second->addChild(term_frame::makeKeyword(m_timerControlKeys[2], "-1m", kNoHotkey, scale, [this]() {
    setTimerDuration(m_timerDuration - std::chrono::minutes{1});
  }));
  second->addChild(term_frame::makeKeyword(m_timerControlKeys[3], "+1m", kNoHotkey, scale, [this]() {
    setTimerDuration(m_timerDuration + std::chrono::minutes{1});
  }));
  controls->addChild(std::move(first));
  controls->addChild(std::move(second));
  run->addChild(std::move(controls));
  run->setVisible(false);
  run->setParticipatesInLayout(false);
  box.addChild(std::move(run));
  m_timerExpanded = false;
}

int HomeTab::timerGoal() const {
  const int goal = m_config != nullptr ? m_config->config().controlCenter.timerDailyGoal : kDefaultTimerGoal;
  return std::clamp(goal, kMinTimerGoal, kMaxTimerGoal);
}

std::chrono::milliseconds HomeTab::timerRemaining() const {
  if (m_timerState != TimerState::Running) {
    return m_timerRemaining;
  }
  const auto left =
      std::chrono::duration_cast<std::chrono::milliseconds>(m_timerEnd - std::chrono::steady_clock::now());
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
  m_timerShown = ceilSeconds(timerRemaining());
  m_timerTime->setText(formatTimer(m_timerShown));
  if (m_timerIdleTime != nullptr) {
    m_timerIdleTime->setText(formatTimer(m_timerDuration)); // the idle line shows the length that will start
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
    m_timerStatus->setText(m_timerState == TimerState::Done ? "done" : "ready");
  }

  // Session squares in the tile's header slot.
  rollTimerDay();
  m_squaresText = sessionSquares(m_timerSessions, timerGoal());
  updateTodayHeader();

  // `start` and `pause` share the letter `a`.
  syncKeyword(m_timerControlKeys[0], running ? "pause" : "start", running ? 1 : 2, false);
  for (std::size_t i = 0; i < m_timerPresetKeys.size(); ++i) {
    term_frame::Keyword& key = m_timerPresetKeys[i];
    syncKeyword(key, key.word, key.hot, m_timerDuration == std::chrono::minutes{kTimerPresetMinutes[i]});
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
  addHiddenRows(*box, scale, m_eventRows, m_fixedRows, [](std::size_t) {
    return []() { openControlCenterTab("calendar"); };
  });
  addHiddenRows(*box, scale, m_taskRows, m_fixedRows, [](std::size_t) {
    return []() { openControlCenterTab("tasks"); };
  });
  box->addChild(
      ui::label({
          .out = &m_weatherLine,
          .fontSize = term_frame::kBtopFontSize * scale,
          .fontFamily = std::string(term_frame::kFont),
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
      })
  );
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
  const auto now = std::chrono::system_clock::now();
  const int today = localDay(now);
  const std::size_t rowLimit = static_cast<std::size_t>(m_capacity[TileToday]);

  // Events that have not ended yet, soonest first; then the open tasks, marked with an empty box so they read apart
  // from the events above them.
  const std::vector<const CalendarEvent*> upcoming = upcomingEvents(m_calendar, now);
  const std::array<bool, kTodayItems> eventWanted =
      fillRows(m_eventRows, upcoming, rowLimit, [&](const CalendarEvent* event) {
        return std::pair{"◇ " + oneLine(event->title), eventWhen(*event, now, today)};
      }); // ◇ marks an event
  const std::string todayKey = localDateKey();
  const std::vector<const TaskItem*> open = openTasks(m_tasks);
  const std::array<bool, kTodayItems> taskWanted = fillRows(m_taskRows, open, rowLimit, [&](const TaskItem* item) {
    return std::pair{"□ " + oneLine(item->title), taskDue(item->dueDate, todayKey)};
  });
  term_frame::setLabelTone(m_weatherLine, weatherText(m_services.weather), Tone::Normal);

  // Header slot: the countdown to the next timed event today; the session squares only when there is none.
  m_nextText = nextEventCountdown(upcoming, now, today);
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
  if (fitItems(items, capacity)) {
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
    box->addChild(
        term_frame::toggleRow(kFleetNames[i], scale, m_fleetRows[i], [this, i]() { activateFleet(i); }, nullptr)
    );
    m_fixedRows.push_back(m_fleetRows[i].button);
  }
  // One row per tailscale peer; shown instead of the summary line when the tile has room.
  addHiddenRows(*box, scale, m_peerRows, m_fixedRows, [](std::size_t) {
    return []() { openControlCenterTab("network"); };
  });
  box->addChild(
      ui::label({
          .out = &m_fleetDetail,
          .fontSize = term_frame::kBtopFontSize * scale,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::toneColor(Tone::Error),
          .maxLines = 3,
          .visible = false,
          .participatesInLayout = false,
      })
  );
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
  const long long nowSeconds = std::time(nullptr);
  const long long age = m_fleetAt > 0 ? nowSeconds - m_fleetAt : -1;
  const bool stale = job.running || age < 0 || age > kFleetStaleSeconds;
  const std::string ageText = age >= 0 ? " · " + formatAgeShort(age) : std::string{};

  // In FleetRow order.
  const std::array<FleetLine, kFleetRows> lines{
      serverLine(job), syncthingLine(job), radicaleLine(m_calendar), tailscaleLine(job)
  };
  std::size_t down = 0;
  std::size_t unknown = 0;
  bool changed = false;
  for (std::size_t i = 0; i < kFleetRows; ++i) {
    term_frame::ToggleRow& row = m_fleetRows[i];
    const FleetLine& line = lines[i];
    changed |= setShown(row.button, line.show);
    if (!line.show || row.button == nullptr) {
      continue;
    }
    const bool fresh = line.live || !stale;
    down += line.down ? 1 : 0;
    unknown += line.value == "unknown" ? 1 : 0;
    setRowName(row, std::string(line.down ? "! " : "") + kFleetNames[i]);
    term_frame::setToggleRow(row, true, false, line.value + (fresh || line.value == "unknown" ? "" : ageText));
    setRowTone(row, line.down && fresh ? Tone::Warning : Tone::Dim);
  }

  // Peers: with room, one row each (state, or when it was last seen) replaces the summary line; otherwise the
  // summary stays. Offline peers are not "down": only the server and the tailscale daemon are.
  const int fleetCapacity = m_capacity[TileFleet] - detailLines(m_fleetDetail);
  const int baseRows =
      (lines[FleetServer].show ? 1 : 0) + (lines[FleetSyncthing].show ? 1 : 0) + (lines[FleetRadicale].show ? 1 : 0);
  const long long peerCount =
      !lines[FleetTailscale].show || scriptValue(job, "ts_state") != "Running" ? 0 : numberValue(job, "peer_count", 0);
  const bool showPeers = peerCount > 0
      && peerCount <= static_cast<long long>(kPeerRows)
      && baseRows + static_cast<int>(peerCount) <= fleetCapacity;
  changed |= setShown(
      m_fleetRows[FleetTailscale].button, lines[FleetTailscale].show && !showPeers && baseRows + 1 <= fleetCapacity
  );
  for (std::size_t i = 0; i < kPeerRows; ++i) {
    term_frame::ToggleRow& row = m_peerRows[i];
    const bool show = showPeers && static_cast<long long>(i) < peerCount;
    changed |= setShown(row.button, show);
    if (!show || row.button == nullptr) {
      continue;
    }
    const PeerText peer = peerText(scriptValue(job, std::format("peer_{}", i + 1)), nowSeconds);
    setRowName(row, "· " + peer.name);
    term_frame::setToggleRow(row, true, false, peer.status);
    setRowTone(row, Tone::Dim);
  }
  setFleetHeader(*m_fleetFrame, down, unknown, stale, ageText);
  if (changed) {
    relayout();
  }
}

// Enter: syncthing opens its web UI, server and tailscale the network panel, radicale the calendar tab.
void HomeTab::activateFleet(std::size_t row) {
  if (row == FleetRadicale) {
    openControlCenterTab("calendar");
    return;
  }
  if (row != FleetSyncthing) {
    openControlCenterTab("network");
    return;
  }
  std::string url(scriptValue(*m_fleetJob, "syncthing_url"));
  if (url.empty()) {
    url = kSyncthingDefaultUrl;
  }
  const bool started = process::runAsync(
      std::vector<std::string>{"xdg-open", url},
      process::RunCallbacks{
          .onExit = [mailbox = m_mailbox, url](process::RunResult result) {
            if (result.exitCode != 0) {
              reportOutcome(
                  mailbox,
                  {.id = "fleet", .exitCode = 1, .error = "could not open " + url + ": " + failureReason(result)}
              );
            }
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
  static_assert(kPowerKeys.size() == kPowerKeyCount);
  auto box = term_frame::btopBox("⁷", "system", scale, &m_systemFrame);
  const auto addKeys = [&](Flex& row, std::size_t first, std::size_t last) {
    for (std::size_t index = first; index < last; ++index) {
      const PowerKey& spec = kPowerKeys[index];
      row.addChild(
          term_frame::makeKeyword(
              m_powerKeys[index], spec.word, spec.hot, scale, [this, index]() { pressPower(index); },
              [this, index]() {
                if (m_armed == &m_powerKeys[index]) {
                  clearArmed();
                }
              },
              term_frame::kActionKeyPaddingH, term_frame::kActionKeyRestAlpha, spec.warning
          )
      );
    }
  };
  auto strip = ui::row({.out = &m_stripRow, .align = FlexAlign::Center, .gap = Style::spaceMd * scale});
  auto plain = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale});
  addKeys(*plain, 0, kFirstDangerKey);
  strip->addChild(std::move(plain));

  auto middle = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .flexGrow = 1.0F});
  middle->addChild(
      ui::label({
          .out = &m_systemDetail,
          .fontSize = term_frame::kBtopFontSize * scale,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::toneColor(Tone::Error),
          .maxLines = 1,
          .visible = false,
          .participatesInLayout = false,
          .configure = [](Label& detail) { detail.setEllipsize(TextEllipsize::End); },
      })
  );
  strip->addChild(std::move(middle));

  auto danger = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale});
  addKeys(*danger, kFirstDangerKey, kPowerKeyCount);
  strip->addChild(std::move(danger));
  box->addChild(std::move(strip));
  return box;
}

void HomeTab::pressPower(std::size_t index) {
  if (index >= kPowerKeyCount) {
    return;
  }
  if (index >= kFirstConfirmedKey) {
    pressConfirmed(m_powerKeys[index], nullptr, "", "confirm?", [this, index]() { runPower(index); });
    return;
  }
  clearArmed();
  runPower(index);
}

void HomeTab::runPower(std::size_t index) {
  const std::string action = kPowerKeys[index].action;
  const std::string id = "power:" + action;
  if (setShown(m_systemDetail, false)) {
    relayout();
  }
  const bool started = process::runAsync(
      std::vector<std::string>{"terminus-bar", "msg", "session", action},
      process::RunCallbacks{
          .onExit = [mailbox = m_mailbox, id](process::RunResult result) {
            if (result.exitCode != 0) {
              reportOutcome(mailbox, {.id = id, .exitCode = result.exitCode, .error = failureReason(result, true)});
            }
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
  if (KeySymbol::isEnterOrSpace(sym)
      && focused != nullptr
      && m_armed->button != nullptr
      && focused == m_armed->button->inputArea()) {
    return;
  }
  if (m_armed->hot < m_armed->word.size()
      && lowerKeysym(sym) == lowerKeysym(static_cast<unsigned char>(m_armed->word[m_armed->hot]))) {
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
      "{}; rc=$?; echo $rc > '{}'; printf '\\n[ exit %s · press enter to close ]' \"$rc\"; read _; exit $rc", command,
      rc
  );
  const bool started = process::runAsync(
      std::vector<std::string>{"uwsm-app", "--", "ghostty", "-e", "sh", "-c", script},
      process::RunCallbacks{
          .onExit = [mailbox = m_mailbox, id, rc,
                     launched = std::chrono::steady_clock::now()](process::RunResult result) {
            // Only a terminal that dies at once, before its shell wrote an exit code, never started; a window
            // closed later exits non-zero too, and that is not a failure of the action.
            std::error_code ec;
            const bool early = std::chrono::steady_clock::now() - launched < kTerminalStartWindow;
            if (result.exitCode == 0 || !early || std::filesystem::exists(rc, ec)) {
              return;
            }
            reportOutcome(
                mailbox,
                {.id = id, .exitCode = result.exitCode, .error = "terminal did not start: " + failureReason(result)}
            );
          },
      }
  );
  if (!started) {
    setActionFailure(id, "could not start a terminal");
    return;
  }
  m_pending.push_back({.id = id, .what = what, .rcFile = rc, .startedAt = std::chrono::steady_clock::now()});
  if (!m_actionPoll.active()) {
    m_actionPoll.startRepeating(kActionPollInterval, [this]() { pollTerminalActions(); });
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
      runScriptJob(m_notesJob, kNotesScript, true, kNotesRefreshInterval);
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
          std::remove_if(
              m_pending.begin(), m_pending.end(), [&](const PendingAction& p) { return p.id == outcome.id; }
          ),
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
  const std::uint32_t letter = lowerKeysym(sym);
  // A release always frees the letter, even one that arrives with a modifier down.
  if (!pressed && m_hotkeyHeld == letter) {
    m_hotkeyHeld = 0;
  }
  const bool shifted = modifiers == KeyMod::Shift;
  const bool upper = sym >= 'A' && sym <= 'Z'; // a capital keysym counts as Shift even when the modifier is absent
  const bool isLetter = (sym >= 'a' && sym <= 'z') || upper;
  if (!isLetter || (modifiers != 0U && !shifted)) {
    return false;
  }
  for (const HotKey& entry : m_hotkeys) {
    if (!hotkeyMatches(*entry.key, letter, shifted, upper) || (entry.gate != nullptr && !entry.gate->visible())) {
      continue;
    }
    if (pressed && m_hotkeyHeld != letter) {
      m_hotkeyHeld = letter;
      const auto activate = entry.key->activate; // a copy: running it may rebuild the keyword
      activate();
    }
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
  const GridMetrics grid = gridMetrics(contentWidth, bodyHeight, scale);
  const float chrome = term_frame::btopChrome(scale);
  const float rowHeight = kHomeRowHeight * scale;
  const float rowGap = Style::spaceXs * scale;
  m_innerWidth = grid.inner;

  for (Flex* row : m_fixedRows) {
    if (row != nullptr) {
      term_frame::pinSize(*row, grid.inner, rowHeight);
    }
  }
  for (Label* detail : m_detailLabels) {
    if (detail != nullptr) {
      detail->setMaxWidth(grid.inner);
    }
  }

  // How many one-line rows each tile holds (the toggle rows sit without a gap between them).
  const auto capacityOf = [&](float tileHeight) {
    return std::max(1, static_cast<int>(std::floor((tileHeight - chrome + rowGap) / (rowHeight + rowGap))));
  };
  const std::array<int, TileCount> capacity{
      capacityOf(grid.rowHeights[0]),
      std::max(1, static_cast<int>(std::floor((grid.rowHeights[0] - chrome) / rowHeight))),
      capacityOf(grid.rowHeights[1]),
      capacityOf(grid.rowHeights[1]),
      capacityOf(grid.rowHeights[2]),
      capacityOf(grid.rowHeights[2]),
  };
  if (capacity != m_capacity) {
    m_capacity = capacity;
    relayout();
  }

  layoutHostHeader(grid.inner, rowHeight, rowGap, scale);
  if (m_hostInfoBox != nullptr) {
    m_hostInfoBox->setMinWidth(grid.inner);
    m_hostInfoBox->setMaxWidth(grid.inner);
  }
  if (m_timerTime != nullptr) {
    m_timerTime->setFontSize(kTimerFontSize * scale);
  }
  if (m_systemDetail != nullptr) {
    m_systemDetail->setMaxWidth(grid.stripInner * kSystemDetailWidthShare);
  }

  const auto pin = [](Flex* node, float w, float h) {
    if (node != nullptr) {
      term_frame::pinSize(*node, w, h);
    }
  };
  if (m_margin != nullptr) {
    m_margin->setMinWidth(grid.margin);
    m_margin->setMaxWidth(grid.margin);
  }
  pin(m_grid, grid.gridWidth, grid.height);
  for (std::size_t i = 0; i < m_gridRows.size(); ++i) {
    pin(m_gridRows[i], grid.gridWidth, grid.rowHeights[i]);
  }
  pin(m_hostFrame, grid.leftWidth, grid.rowHeights[0]);
  pin(m_togglesFrame, grid.rightWidth, grid.rowHeights[0]);
  pin(m_upkeepFrame, grid.leftWidth, grid.rowHeights[1]);
  pin(m_todayFrame, grid.rightWidth, grid.rowHeights[1]);
  pin(m_inboxFrame, grid.leftWidth, grid.rowHeights[2]);
  pin(m_fleetFrame, grid.rightWidth, grid.rowHeights[2]);
  pin(m_systemFrame, grid.gridWidth, grid.stripHeight);
  pin(m_stripRow, grid.stripInner, rowHeight);
  m_rootLayout->setMinWidth(grid.width);
  m_rootLayout->setMaxWidth(grid.width);
  m_rootLayout->layout(renderer);
}

// The avatar is two rows tall; the user and host sit beside it.
void HomeTab::layoutHostHeader(float inner, float rowHeight, float rowGap, float scale) {
  if (m_hostHeader == nullptr || m_avatarFrame == nullptr) {
    return;
  }
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
  runScriptJob(m_notesJob, kNotesScript, false, kNotesRefreshInterval);
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
      "zenity",
      "--file-selection",
      "--title=Choose profile picture",
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
          .onExit = [config, accounts, busy](process::RunResult result) {
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
