#pragma once

#include "core/process/process.h"
#include "core/timer_manager.h"
#include "pipewire/pipewire_service.h"
#include "shell/control_center/tab.h"
#include "shell/control_center/term_frame.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Button;
class ConfigService;
class EasyEffectsService;
class Flex;
class Label;
class MprisService;
class Node;
class RenderContext;
class Renderer;
class ScrollView;
class Slider;
class WaylandConnection;

// Sound quick panel in the terminal look. `¹output` / `²input`: one row per device with short names (Enter makes it
// the default; bluetooth devices can switch card profile) and the default device's `vol` meter directly under it.
// `³apps`: one meter row per playing application (←→ volume, `a` mute / move to another output). `⁴equaliser`: a
// single line while off, the preset line and ten centre-zero bands while on (driven through the `terminus-audiofx eq`
// command line). The whole body scrolls when it outgrows the panel.
class SoundTab : public Tab {
public:
  static constexpr std::size_t kBandCount = 10;

  SoundTab(
      PipeWireService* audio, EasyEffectsService* easyEffects, MprisService* mpris, ConfigService* config,
      WaylandConnection* wayland, RenderContext* renderContext
  );
  ~SoundTab() override;

  std::unique_ptr<Flex> create() override;
  void setActive(bool active) override;
  void onClose() override;

  [[nodiscard]] float quickPanelHeight(float width, float fallback) const override;
  [[nodiscard]] std::string quickPanelHints() const override;
  [[nodiscard]] std::string quickPanelStatus() const override;
  [[nodiscard]] bool
  handleQuickPanelKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

protected:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;

private:
  enum class Kind : std::uint8_t { OutVolume, OutDevice, InVolume, InDevice, App, EqMain, Band };

  struct EqState {
    bool enabled = false;
    std::string preset;
    std::vector<std::string> presets;
    std::array<float, kBandCount> gains{};
    float minGain = -12.0F;
    float maxGain = 12.0F;
  };

  struct Job {
    std::vector<std::string> args;
    bool status = false;
  };

  // What the layout depends on; a change asks the host to re-read the panel height.
  struct Shape {
    std::size_t outs = 0;
    std::size_t ins = 0;
    std::size_t apps = 0;
    std::size_t eqRows = 0;
    std::size_t menuRows = 0;
    bool operator==(const Shape&) const = default;
  };

  // Identity of a selectable row across rebuilds.
  struct Ident {
    Kind kind = Kind::OutVolume;
    std::uint32_t id = 0;
    std::size_t index = 0;
    bool operator==(const Ident&) const = default;
  };

  struct MenuAction {
    char key = 0;
    std::string word;
    std::size_t hot = 0;
  };

  // In-progress / failure text shown in the row it belongs to.
  struct RowStatus {
    std::string text;
    bool error = false;
    std::chrono::steady_clock::time_point until;
  };

  // A service call whose effect is confirmed (or timed out) by watching the PipeWire state.
  struct Pending {
    std::string key;
    std::function<bool()> done;
    std::chrono::steady_clock::time_point deadline;
  };

  struct CardProfile {
    std::string name;
    bool available = true;
  };

  struct Card {
    std::string active;
    std::vector<CardProfile> profiles;
  };
  using CardMap = std::unordered_map<std::string, Card>;

  // One selectable row (device meter, device, application, equaliser line or band).
  struct Item {
    Ident ident;
    Flex* row = nullptr;
    Label* tag = nullptr;
    std::string base;
    // Meter rows (volume, application, band).
    Slider* slider = nullptr;
    Label* value = nullptr;
    Label* status = nullptr;
    Button* meter = nullptr;
    // Device rows and the equaliser line.
    Button* nameButton = nullptr;
    Label* detail = nullptr;
    std::string lastMarkup;
    std::unique_ptr<term_frame::Keyword> badge;        // `muted` (meters, devices) or `on`/`off` (equaliser)
    std::unique_ptr<term_frame::Keyword> defaultBadge; // `default` (devices)
  };

  [[nodiscard]] Shape currentShape() const;
  [[nodiscard]] static float heightFor(const Shape& shape);

  // ---- Data helpers
  [[nodiscard]] std::vector<AudioNode> devices(bool input) const;
  [[nodiscard]] std::size_t deviceCount(bool input) const; // `devices(input).size()` without copying or sorting
  [[nodiscard]] const AudioNode* defaultNode(bool input) const;
  [[nodiscard]] std::uint32_t defaultId(bool input) const;
  [[nodiscard]] bool eqUsable() const; // status loaded and no error
  [[nodiscard]] std::vector<AudioNode> sortedApps() const;
  [[nodiscard]] const AudioNode* findNode(const Item& item) const;
  [[nodiscard]] std::string statusKey(const Item& item) const;
  [[nodiscard]] const Card* cardFor(const AudioNode& node) const;

  // ---- Body construction
  [[nodiscard]] std::string structureKey() const;
  void rebuildBody();
  void clearBody();
  void addMutedBadge(Item& item, Flex& row, float scale);
  [[nodiscard]] Item& addItem(Kind kind, std::uint32_t id, std::size_t index);
  [[nodiscard]] std::unique_ptr<Flex>
  buildMeterRow(Item& item, std::string tag, std::function<void(double)> onChange, float scale);
  [[nodiscard]] std::unique_ptr<Flex>
  buildDeviceRow(Item& item, const AudioNode& node, std::string shortName, float scale);
  [[nodiscard]] std::unique_ptr<Flex> buildEqRow(Item& item, float scale);
  [[nodiscard]] std::unique_ptr<Flex> buildBandRow(Item& item, std::size_t band, float scale);
  void buildDeviceSection(Flex& content, bool input, float scale);
  void buildAppsSection(Flex& content, float scale);
  void buildEqSection(Flex& content, float scale);
  void buildMenu(Flex& box, Item& item, float scale);
  [[nodiscard]] bool buildMoveMenu(Flex& box, float scale);
  [[nodiscard]] std::vector<MenuAction> menuActions(const Item& item) const;

  // ---- Sync
  void refresh();
  void syncItems();
  void syncMeter(Item& item, const AudioNode* node, bool hold);
  void syncDeviceRow(Item& item, const AudioNode* node);
  void syncPending();
  void syncEqControls();
  void renderBand(std::size_t index);
  [[nodiscard]] std::string bandMarkup(float gain) const;
  void setBandFromX(std::size_t index, float x);
  void setVolumeLocal(Item& item, float volume, bool muted);
  void noteVolumeWrite();

  // ---- Selection and actions
  void select(std::size_t index);
  void selectItem(const Item& item);
  void updateSelection();
  void styleItem(Item& item, bool selected);
  void adjustSelected(int direction);
  [[nodiscard]] bool handleMoveKey(std::uint32_t sym, bool pressed);
  void activateSelected();
  void toggleMenu();
  void closeMenu();
  void runAction(Item& item, char key);
  void toggleMute(const Item& item);
  void setDefaultDevice(const Item& item);
  void startMove(const Item& item);
  void executeMove(std::size_t targetIndex);
  void cycleProfile(const Item& item);
  void toggleEq();
  void cyclePreset(int direction);
  void markDirty();

  void setStatus(const std::string& key, std::string text, bool error, std::chrono::milliseconds ttl);
  void clearStatus(const std::string& key);
  [[nodiscard]] const RowStatus* statusFor(const std::string& key) const;
  void addPending(std::string key, std::function<bool()> done, std::chrono::milliseconds timeout);

  // ---- pactl (bluetooth card profiles)
  void loadCards();
  [[nodiscard]] static std::optional<CardMap> parseCards(const std::string& json);
  void applyCards(const std::string& json);

  // ---- Equaliser bands
  void scheduleBandCommit(std::size_t index);
  void flushBand();
  void commitBand(std::size_t index);

  // ---- terminus-audiofx jobs
  void enqueue(std::vector<std::string> args);
  void enqueueStatus();
  void pump();
  void onJobDone(const Job& job, const process::RunResult& result);
  [[nodiscard]] static std::optional<EqState> parseEqStatus(const std::string& json);
  void applyStatus(const std::string& json);
  void setError(std::string message);

  PipeWireService* m_pw = nullptr;

  ScrollView* m_scroll = nullptr;
  Label* m_eqNotice = nullptr;
  std::vector<Flex*> m_sections;
  std::vector<std::unique_ptr<Item>> m_items;
  std::vector<std::unique_ptr<term_frame::Keyword>> m_menuKw;
  std::array<Item*, kBandCount> m_bands{};
  std::string m_bodyKey;
  bool m_bodyDirty = true;

  Shape m_shape;
  std::size_t m_selected = 0;
  Ident m_selIdent;
  bool m_menuOpen = false;
  bool m_moveMode = false;
  std::size_t m_moveSel = 0;
  std::size_t m_menuRows = 0;
  std::size_t m_eqCells = 31;
  std::chrono::steady_clock::time_point m_volumeHoldUntil;

  std::unordered_map<std::string, RowStatus> m_statuses;
  std::vector<Pending> m_pending;
  CardMap m_cards;
  std::unordered_set<std::string> m_cardsTried;
  bool m_cardsLoading = false;
  // WirePlumber leaves `default.audio.source` unset while the configured microphone is unplugged; this is the
  // source `pactl get-default-source` still names (read on open).
  std::string m_fallbackSource;
  Timer m_tickTimer;

  EqState m_eq;
  std::array<float, kBandCount> m_work{};
  std::array<float, kBandCount> m_applied{};
  std::string m_error;
  bool m_eqLoaded = false;
  bool m_eqDirty = false;
  bool m_eqPending = false;
  bool m_statusRequested = false;
  bool m_syncing = false;
  bool m_busy = false;
  bool m_active = false;
  std::size_t m_dragBand = kBandCount;
  std::size_t m_pendingBand = kBandCount;
  Timer m_commitTimer;
  std::deque<Job> m_queue;
  // Flipped to false on destruction; worker completions hop to the main loop and check it.
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
