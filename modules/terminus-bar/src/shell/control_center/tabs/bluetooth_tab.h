#pragma once

#include "core/timer_manager.h"
#include "dbus/bluetooth/bluetooth_agent.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "shell/control_center/tab.h"
#include "shell/control_center/term_frame.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class BluetoothDeviceRow;
class Button;
class Flex;
class Input;
class Label;
class ScrollView;
class Spinner;
class Toggle;

class BluetoothTab : public Tab {
public:
  // `quick` selects the terminal-style layout hosted in a bar-attached QuickPanel; the control-center tab keeps the
  // default (false).
  BluetoothTab(BluetoothService* service, BluetoothAgent* agent, bool quick = false);
  ~BluetoothTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setActive(bool active) override;
  [[nodiscard]] float quickPanelHeight(float width, float fallback) const override;
  [[nodiscard]] std::string quickPanelHints() const override;
  [[nodiscard]] std::string quickPanelStatus() const override;
  [[nodiscard]] bool handleQuickPanelKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;

  // Discovery started by this tab's Rescan. Pending until BlueZ confirms Discovering,
  // so a stop still fires while StartDiscovery is in flight.
  enum class DiscoveryLease : std::uint8_t {
    None,
    Pending,
    Active,
  };

  // Reconciles the lease with the adapter state; drops it when discovery ended elsewhere.
  void syncDiscoveryLease();
  // Forgets the lease without asking BlueZ to stop.
  void releaseDiscoveryLease();
  // Releases the lease and stops the discovery this tab started.
  void stopRequestedDiscovery();

  // Starts (or renews) an explicit scan; no-op while the adapter is off.
  void requestScan();
  void submitPairingInput(const std::string& value);
  void acceptPairing();
  void trustAndAllowPairing();
  void rejectPairing();

  void syncHeader();
  void syncPairingCard();
  void rebuildDeviceList(Renderer& renderer);
  // Pushes live metric values into the existing rows. Returns true if any changed.
  bool syncDeviceRows();
  [[nodiscard]] std::string structureKey(const std::vector<BluetoothDeviceInfo>& devices) const;

  // ---- Quick (QuickPanel) mode ----
  // What the panel shows, derived from the service state; drives both the height and the rebuilds.
  struct QuickPlan {
    bool pairing = false;
    bool pairingInput = false;
    int devLines = 1;
    bool availBox = false;
    int availLines = 1;
    std::string key;
  };
  // One device row of ²devices / ³available.
  struct QuickRow {
    Flex* node = nullptr;
    Label* gutter = nullptr;
    bool avail = false; // listed under ³available
    term_frame::ToggleRow row;
    std::string name;   // markup of the name button
    std::string detail; // markup of the dim detail at the end of the row
  };
  // Card profiles of a headphone, from `pactl -f json list cards`.
  struct QuickProfiles {
    std::string card;
    std::string active;
    std::vector<std::string> profiles; // selectable ones, without "off"
    bool operator==(const QuickProfiles&) const = default;
  };

  [[nodiscard]] QuickPlan quickPlan() const;
  [[nodiscard]] std::optional<BluetoothDeviceInfo> findDevice(const std::string& path) const;
  [[nodiscard]] bool textInputFocused() const;
  // The live (under 10 s) failure of a device path or "adapter"; the service's own or one this tab recorded.
  [[nodiscard]] std::optional<std::string> quickFailure(const std::string& key) const;
  [[nodiscard]] const QuickProfiles* quickProfilesFor(const BluetoothDeviceInfo& device) const;
  std::unique_ptr<Flex> createQuick();
  void doLayoutQuick(Renderer& renderer, float contentWidth, float bodyHeight);
  void syncQuick(Renderer& renderer, bool force);
  void rebuildQuickLists(Renderer& renderer);
  void buildQuickRows(Flex& list, const std::vector<BluetoothDeviceInfo>& devices, float scale);
  void buildQuickPairRow(Flex& list, float scale);
  std::unique_ptr<Flex> buildQuickMenu(const BluetoothDeviceInfo& device, float scale);
  // A fixed-height line led by the selection gutter; the gutter label is returned through `gutterOut`.
  std::unique_ptr<Flex> makeQuickLine(float scale, Label** gutterOut, Flex** nodeOut = nullptr);
  // Updates names, states and meters in place. Returns true when a row's text changed.
  bool syncQuickRows();
  void syncQuickGutters();
  void quickSelectId(const std::string& id);
  void quickActivateSelected();
  void quickActivate(const std::string& path);
  void quickToggleMenu();
  void quickToggleTrust(const std::string& path);
  void quickForget(const std::string& path);
  void quickCycleProfile(const std::string& path);
  void quickSelect(int delta);
  void quickTogglePower();
  void quickToggleDiscoverable();
  void quickPressConfirmed(term_frame::Keyword& keyword, const std::function<void()>& run);
  void quickClearArmed();
  bool quickPressMenuKey(std::uint32_t sym);
  void setLocalFailure(const std::string& key, std::string reason);
  void clearQuickFailure(const std::string& key);
  void refreshProfiles();
  void applyProfiles(const std::string& json);
  void deferDiscoveryStop();
  void resetQuick();

  BluetoothService* m_service = nullptr;
  BluetoothAgent* m_agent = nullptr;
  bool m_quick = false;
  // Cleared on destruction so a pactl result that lands afterwards is dropped.
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);

  term_frame::ToggleRow m_qPower;
  term_frame::ToggleRow m_qDiscoverable;
  Label* m_qPowerGutter = nullptr;
  Label* m_qDiscoverableGutter = nullptr;
  Label* m_qPairGutter = nullptr;
  Flex* m_qPairNode = nullptr;
  Flex* m_qDevBox = nullptr;
  Flex* m_qAvailBox = nullptr;
  ScrollView* m_qDevScroll = nullptr;
  ScrollView* m_qAvailScroll = nullptr;
  Flex* m_qDevList = nullptr;
  Flex* m_qAvailList = nullptr;
  Label* m_qAvailChip = nullptr;
  std::array<term_frame::Keyword, 3> m_qPairKeys;
  std::array<term_frame::Keyword, 4> m_qMenuKeys; // connect/disconnect/pair, trust, profile, forget
  term_frame::Keyword* m_qArmed = nullptr;
  Timer m_qArmTimer;
  Timer m_qFailTimer;
  std::unordered_map<std::string, QuickRow> m_qRows;
  std::vector<std::string> m_qOrder; // selectable ids in display order: "power", "discoverable", device paths, "pair"
  std::string m_qSelected;
  std::string m_qMenuFor; // device path whose action menu line is open
  std::unordered_map<std::string, BluetoothFailure> m_qLocalFailures;
  std::unordered_map<std::string, QuickProfiles> m_qProfiles; // by upper-case address
  int m_qProfilesRev = 0;
  bool m_qProfilesBusy = false;
  std::string m_qConnectedKey;
  bool m_qScrollToSelected = false;
  bool m_qFocusInput = false;
  bool m_qHadInput = false;
  std::string m_qPlanKey;
  std::string m_qListKey;
  std::string m_qHeightKey;

  Flex* m_rootLayout = nullptr;

  Flex* m_pairingCard = nullptr;
  Label* m_pairingTitle = nullptr;
  Label* m_pairingDetail = nullptr;
  Label* m_pairingCode = nullptr;
  Flex* m_pairingInputRow = nullptr;
  Input* m_pairingInput = nullptr;
  Flex* m_pairingButtonRow = nullptr;
  Button* m_pairingAccept = nullptr;
  Button* m_pairingTrustAndAllow = nullptr;
  Button* m_pairingReject = nullptr;

  ScrollView* m_listScroll = nullptr;
  Flex* m_list = nullptr;

  Toggle* m_powerToggle = nullptr;
  Toggle* m_discoverableToggle = nullptr;
  Button* m_rescanButton = nullptr;
  Spinner* m_scanSpinner = nullptr;

  Timer m_discoveryTimer;
  DiscoveryLease m_discoveryLease = DiscoveryLease::None;

  std::unordered_map<std::string, BluetoothDeviceRow*> m_deviceRows;

  std::string m_lastStructureKey;
  float m_lastListWidth = -1.0F;
};
