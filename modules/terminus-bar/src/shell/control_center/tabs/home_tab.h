#pragma once

#include "config/config_types.h"
#include "core/timer_manager.h"
#include "shell/control_center/control_center_services.h"
#include "shell/control_center/shortcut_services.h"
#include "shell/control_center/tab.h"
#include "shell/control_center/term_frame.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Button;
class ConfigService;
class Glyph;
class GridView;
class InputArea;
class InputDispatcher;
class Label;
class Shortcut;
class NotificationManager;
class Slider;
class AccountsService;
class AsyncTextureCache;
class Image;
class CalendarService;
class TaskService;

inline constexpr std::array<int, 4> kTimerPresetMinutes{5, 10, 25, 45};

struct ShortcutPad {
  // Survives HomeTab::onClose(); the row's pointers are nulled with the scene.
  std::unique_ptr<Shortcut> shortcut;
  term_frame::ToggleRow row;
};

// Output of a shell command run on a worker thread. Shared so the completion callback outlives the tab.
struct ScriptJob {
  std::string out;
  bool loaded = false;
  bool running = false;
  std::chrono::steady_clock::time_point startedAt{};
};

// The outcome of an action that ran in a terminal, handed from the worker thread to the main loop.
struct ActionOutcome {
  std::string id;
  int exitCode = 0;
  std::string error; // why it could not run at all; empty when the command ran
};

struct ActionMailbox {
  std::vector<ActionOutcome> done;
};

// The Home dashboard's left side: two columns of content-sized tiles, numbered in reading order
//   left:  1 host, 3 upkeep, 5 inbox      right: 2 toggles, 4 today, 6 fleet      below, across both: 7 system
class HomeTab : public Tab {
public:
  explicit HomeTab(const ControlCenterServices& services);
  ~HomeTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onFrameTick(float deltaMs) override;
  void setActive(bool active) override;
  void onClose() override;
  bool handleHotkey(std::uint32_t sym, std::uint32_t modifiers, bool pressed);
  // Called for every key while the dashboard is up: anything but the confirming press disarms an armed action.
  void noteKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, const InputArea* focused);
  [[nodiscard]] Flex* hostBox() const noexcept { return m_hostFrame; }
  [[nodiscard]] Flex* upkeepBox() const noexcept { return m_upkeepFrame; }
  [[nodiscard]] Flex* inboxBox() const noexcept { return m_inboxFrame; }
  [[nodiscard]] Flex* togglesBox() const noexcept { return m_togglesFrame; }
  [[nodiscard]] Flex* todayBox() const noexcept { return m_todayFrame; }
  [[nodiscard]] Flex* fleetBox() const noexcept { return m_fleetFrame; }
  [[nodiscard]] Flex* systemBox() const noexcept { return m_systemFrame; }
  // Height the tiles occupy: the grid fills the panel, so this is the panel's height.
  [[nodiscard]] float naturalHeight() const;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void sync();
  void relayout();

  // Sections.
  [[nodiscard]] std::unique_ptr<Flex> createHostBox(float scale);
  [[nodiscard]] std::unique_ptr<Flex> createUpkeepBox(float scale);
  [[nodiscard]] std::unique_ptr<Flex> createInboxBox(float scale);
  [[nodiscard]] std::unique_ptr<Flex> createTogglesBox(float scale);
  void addTimerRows(Flex& box, float scale);
  [[nodiscard]] int detailLines(const Label* label) const;
  [[nodiscard]] std::unique_ptr<Flex> createTodayBox(float scale);
  [[nodiscard]] std::unique_ptr<Flex> createFleetBox(float scale);
  [[nodiscard]] std::unique_ptr<Flex> createSystemBox(float scale);
  void syncHost();
  void syncUpkeep();
  void syncInbox();
  void syncToday();
  void updateTodayHeader();
  void syncFleet();
  void activateFleet(std::size_t row);
  void syncShortcuts();
  void syncShortcutPad(ShortcutPad& pad);
  void syncToggleCount();
  void syncTimer();
  void syncHeaderActions();
  void syncAvatar(Renderer& renderer);
  void chooseAvatar();
  void clearAvatar();
  std::string toggleDetail(const Shortcut& shortcut, bool on);

  // Data jobs.
  void refreshLive(bool force);
  void refreshUpkeep(bool force);

  // Actions: armed keywords, terminal runs and their outcomes.
  void pressConfirmed(
      term_frame::Keyword& keyword, term_frame::ActionRow* row, std::string_view detail, std::string_view blockText,
      const std::function<void()>& run
  );
  void clearArmed();
  void pressPower(std::size_t index);
  void runPower(std::size_t index);
  void launchInTerminal(const std::string& id, const std::string& what, const std::string& command);
  void collectOutcomes();
  void pollTerminalActions();
  void setActionFailure(const std::string& id, const std::string& text);
  [[nodiscard]] term_frame::ActionRow* actionRowFor(std::string_view id);
  void markJournalSeen();
  void copyClipboardEntry(std::size_t slot);
  void activateNote(std::size_t slot);

  // Timer.
  [[nodiscard]] std::chrono::milliseconds timerRemaining() const;
  void setTimerDuration(std::chrono::seconds duration);
  void toggleTimer();
  void resetTimer();
  void finishTimer();
  void onTimerTick();
  void loadTimerToday();
  void recordTimerSession();
  void rollTimerDay();
  [[nodiscard]] int timerGoal() const;

  ConfigService* m_config = nullptr;
  ShortcutServices m_services;
  bool m_active = false;
  AccountsService* m_accounts = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;
  NotificationManager* m_notifications = nullptr;
  CalendarService* m_calendar = nullptr;
  TaskService* m_tasks = nullptr;

  // Layout: a grid of three rows of two tiles and a strip. The tiles keep their size; each one shows as many rows as
  // its capacity (in lines) holds.
  enum Tile : std::size_t { TileHost, TileToggles, TileUpkeep, TileToday, TileInbox, TileFleet, TileCount };
  Flex* m_rootLayout = nullptr;
  Flex* m_margin = nullptr;
  Flex* m_grid = nullptr;
  std::array<Flex*, 3> m_gridRows{};
  Flex* m_stripRow = nullptr;
  std::array<int, TileCount> m_capacity{99, 99, 99, 99, 99, 99};
  float m_innerWidth = 300.0F;
  std::shared_ptr<std::atomic<bool>> m_relayoutQueued = std::make_shared<std::atomic<bool>>(false);
  // Rows that span the tile width and keep one text line of height.
  std::vector<Flex*> m_fixedRows;
  // Wrapped lines under a row; their width follows the tile.
  std::vector<Label*> m_detailLabels;

  // Keywords the hotkey letters reach. A keyword inside a hidden group does not act.
  struct HotKey {
    term_frame::Keyword* key;
    Flex* gate;
  };
  std::vector<HotKey> m_hotkeys;
  std::uint32_t m_hotkeyHeld = 0; // letter whose press is still down, so key repeat does not act again

  // 1 host: avatar, user and hostname, three live rows, and the `i` key that shows the static facts.
  term_frame::BtopFrame* m_hostFrame = nullptr;
  Flex* m_hostHeader = nullptr;
  Flex* m_hostText = nullptr;
  Label* m_hostUser = nullptr;
  Label* m_hostName = nullptr;
  InputArea* m_avatarArea = nullptr;
  Flex* m_avatarFrame = nullptr;
  Image* m_avatar = nullptr;
  Label* m_avatarInitial = nullptr;
  Label* m_avatarHint = nullptr;
  // Set while the external file chooser runs; shared so its worker-thread callback never touches `this`.
  std::shared_ptr<std::atomic<bool>> m_avatarPickerBusy = std::make_shared<std::atomic<bool>>(false);
  static constexpr std::size_t kHostLiveRows = 3;  // uptime, kernel, session
  static constexpr std::size_t kHostInfoRows = 6;  // os, shell, wm, pkgs, gpu, display
  std::array<term_frame::InfoRow, kHostLiveRows> m_hostLive{};
  std::array<term_frame::InfoRow, kHostInfoRows> m_hostInfoRows{};
  Flex* m_hostLiveBox = nullptr;
  Flex* m_hostInfoBox = nullptr;
  term_frame::ActionRow m_infoKey;
  bool m_infoOpen = false;
  float m_avatarSize = 0.0F;
  std::string m_avatarPath;
  std::string m_osLabel;
  std::string m_gpuLabel;
  bool m_systemInfoLoaded = false;
  std::shared_ptr<ScriptJob> m_liveJob = std::make_shared<ScriptJob>();

  // 3 upkeep: warnings first (reboot, root, failed units, new journal errors), one dim line for what is fine, then
  // the housekeeping keys. The numbers of the slow package check live in m_upkeepJob.
  term_frame::BtopFrame* m_upkeepFrame = nullptr;
  term_frame::InfoRow m_rebootRow;
  term_frame::InfoRow m_rootRow;
  term_frame::ActionRow m_failedRow;
  term_frame::ActionRow m_journalRow;
  Label* m_upkeepSummary = nullptr;
  term_frame::ActionRow m_updateRow;
  term_frame::ActionRow m_orphansRow;
  term_frame::ActionRow m_cacheRow;
  Label* m_lastUpdate = nullptr;
  std::shared_ptr<ScriptJob> m_upkeepJob = std::make_shared<ScriptJob>();
  std::chrono::steady_clock::time_point m_journalMarkedAt{}; // when the errors were last marked seen
  Signal<>::ScopedConnection m_keywordPaletteConn;
  double m_rootUsed = 0.0;
  Timer m_upkeepTimer;

  // 5 inbox: the latest unread notifications and the last clipboard entries. The pools only bound the widgets that
  // exist; how many rows show is decided by the tile's height.
  static constexpr std::size_t kInboxNotes = 8;
  static constexpr std::size_t kInboxClips = 16;
  term_frame::BtopFrame* m_inboxFrame = nullptr;
  std::array<term_frame::ToggleRow, kInboxNotes> m_noteRows{};
  std::array<std::string, kInboxNotes> m_noteMako{}; // mako's id for a row, empty for the bar's own notification
  std::shared_ptr<ScriptJob> m_notesJob = std::make_shared<ScriptJob>();
  std::array<term_frame::ToggleRow, kInboxClips> m_clipRows{};
  Label* m_inboxDetail = nullptr;
  Label* m_clipFull = nullptr; // the whole text of the focused clipboard row

  // 4 today: the next calendar events, the open tasks and a weather line.
  static constexpr std::size_t kTodayItems = 16;
  term_frame::BtopFrame* m_todayFrame = nullptr;
  std::array<term_frame::ToggleRow, kTodayItems> m_eventRows{};
  std::array<term_frame::ToggleRow, kTodayItems> m_taskRows{};
  Label* m_weatherLine = nullptr;
  std::string m_nextText;    // "in 45m" while a timed event is coming up today
  std::string m_squaresText; // the session squares, shown when there is no such event

  // 6 fleet: whether the machines and services this one depends on are up; polled at most once a minute.
  static constexpr std::size_t kFleetRows = 4; // server, syncthing, radicale, tailscale
  static constexpr std::array<const char*, kFleetRows> kFleetNames{"server", "syncthing", "radicale", "tailscale"};
  term_frame::BtopFrame* m_fleetFrame = nullptr;
  std::array<term_frame::ToggleRow, kFleetRows> m_fleetRows{};
  static constexpr std::size_t kPeerRows = 16; // one per tailscale peer, shown when the tile has room
  std::array<term_frame::ToggleRow, kPeerRows> m_peerRows{};
  Label* m_fleetDetail = nullptr;
  std::shared_ptr<ScriptJob> m_fleetJob = std::make_shared<ScriptJob>();
  std::chrono::steady_clock::time_point m_fleetSeenStart{};
  long long m_fleetAt = 0; // epoch seconds when the shown values were checked

  // 2 toggles.
  term_frame::BtopFrame* m_togglesFrame = nullptr;
  Flex* m_togglesList = nullptr;
  std::vector<ShortcutPad> m_shortcutPads;
  // Plugin runtime configuration must match before an instance can be reused.
  PluginsConfig m_lastPlugins;

  // The timer is the first line of today: one line while idle; the large time beside the controls while running.
  Flex* m_timerIdleRow = nullptr;
  Label* m_timerIdleTime = nullptr;
  Label* m_timerStatus = nullptr;
  term_frame::Keyword m_timerStartKey;
  std::array<term_frame::Keyword, kTimerPresetMinutes.size()> m_timerPresetKeys{};
  Flex* m_timerRunBox = nullptr;
  Flex* m_timerTimeBlock = nullptr;
  Label* m_timerTime = nullptr;
  bool m_timerLit = false;
  bool m_timerExpanded = false;
  // pause/start, reset, -1m, +1m.
  std::array<term_frame::Keyword, 4> m_timerControlKeys{};

  // 7 system: lock, sleep and logout; reboot and shutdown apart, in the warning tone.
  static constexpr std::size_t kPowerKeyCount = 5;
  term_frame::BtopFrame* m_systemFrame = nullptr;
  std::array<term_frame::Keyword, kPowerKeyCount> m_powerKeys{};
  Label* m_systemDetail = nullptr;

  // The keyword waiting for its confirming second press (one at a time, shared by every section).
  term_frame::Keyword* m_armed = nullptr;
  term_frame::ActionRow* m_armedRow = nullptr;
  Timer m_armTimer;

  // Actions that opened a terminal; their exit codes arrive in files, and the launcher's own failure in the mailbox.
  struct PendingAction {
    std::string id;
    std::string what;
    std::string rcFile;
    std::chrono::steady_clock::time_point startedAt;
  };
  std::vector<PendingAction> m_pending;
  std::shared_ptr<ActionMailbox> m_mailbox = std::make_shared<ActionMailbox>();
  Timer m_actionPoll;

  // Timer state outlives the scene (onClose) so a running countdown keeps going with the panel
  // closed and still notifies on completion.
  enum class TimerState : std::uint8_t { Idle, Running, Paused, Done };
  TimerState m_timerState = TimerState::Idle;
  std::chrono::seconds m_timerDuration{300};
  // Time left while Idle/Paused; recomputed from m_timerEnd while Running.
  std::chrono::milliseconds m_timerRemaining{300000};
  std::chrono::steady_clock::time_point m_timerEnd;
  std::chrono::seconds m_timerShown{-1};
  Timer m_timerTick;
  // Today's finished timers of the goal length, kept in $XDG_STATE_HOME/terminus-est/dashboard-timer-today.
  std::string m_timerDay;
  int m_timerSessions = 0;
  long long m_timerSecondsToday = 0;

  Button* m_sessionButton = nullptr;
};
