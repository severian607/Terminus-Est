#pragma once

#include "core/timer_manager.h"
#include "dbus/network/enterprise_credentials.h"
#include "dbus/network/network_secret_agent.h"
#include "dbus/network/network_types.h"
#include "shell/control_center/tab.h"
#include "shell/control_center/tabs/network_quick_data.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class AccessPointRow;
class Button;
class CellularRow;
class ExternalIpService;
class Flex;
class Input;
class Label;
class ModemManagerService;
class ScrollView;
class Select;
class Spinner;
class Toggle;
class INetworkService;
class SystemMonitorService;
class TextClipboard;
class Graph;
class Box;
class Button;

namespace netpanel {
  struct QElem;
  struct QRow;
  struct QSection;
} // namespace netpanel

namespace term_frame {
  struct Keyword;
  struct ToggleRow;
} // namespace term_frame

class NetworkTab : public Tab {
public:
  // `quick` selects the btop-style layout hosted in a bar-attached QuickPanel; the control-center tab keeps the
  // default (false). `monitor` (traffic graph) and `clipboard` (click-to-copy) may be null; quick mode only.
  NetworkTab(
      INetworkService* network, NetworkSecretAgent* secrets, ExternalIpService* externalIp, ModemManagerService* modem,
      SystemMonitorService* monitor = nullptr, TextClipboard* clipboard = nullptr, bool quick = false
  );
  ~NetworkTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;

  [[nodiscard]] float quickPanelHeight(float width, float fallback) const override;
  [[nodiscard]] std::string quickPanelHints() const override;
  [[nodiscard]] std::string quickPanelStatus() const override;
  [[nodiscard]] bool
  handleQuickPanelKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;

  // ---- Quick (QuickPanel) mode ----
  struct QuickSnapshot {
    std::string iface;
    std::string medium; // wired | wi-fi | cellular
    std::string speed;
    std::string band;
    std::string ssid;
    std::string ip;      // addr/prefix
    std::string ipPlain; // addr
    std::string gateway;
    int signal = 0;
    bool connected = false;
    bool hasWifiDevice = false;
  };
  // What the panel body shows, derived from the live state; the rendered rows and the panel height both come from it.
  struct QuickModel;
  // A transient message shown in a row: progress, a result or a failure.
  enum class NoteTone : std::uint8_t { Dim, Accent, Warning, Error };
  struct Note {
    std::string text;
    NoteTone tone = NoteTone::Dim;
    bool busy = false; // progress: stays until the command reports
    std::chrono::steady_clock::time_point until;
  };
  struct WifiAttempt {
    std::string ssid;
    std::chrono::steady_clock::time_point since;
    bool withPassword = false; // a password was submitted for this attempt
    bool sawResolving = false;
  };
  struct MenuKey {
    std::uint32_t key = 0; // lowercase letter or digit
    std::string id;
    bool destructive = false;
  };
  [[nodiscard]] QuickSnapshot readQuickSnapshot() const;
  [[nodiscard]] QuickModel buildQuickModel(float width, const QuickSnapshot& snap) const;
  // Row builders of buildQuickModel, one per section; `now` is read once per model.
  [[nodiscard]] std::string quickPasswordRowId() const;
  [[nodiscard]] std::string
  quickDetailOr(const std::string& id, std::string fallback, std::chrono::steady_clock::time_point now) const;
  [[nodiscard]] std::vector<netpanel::QRow>
  quickIpRows(const QuickSnapshot& snap, std::chrono::steady_clock::time_point now) const;
  [[nodiscard]] std::vector<netpanel::QRow> quickConnectionRows(std::chrono::steady_clock::time_point now) const;
  [[nodiscard]] std::vector<netpanel::QRow>
  quickWifiRows(std::chrono::steady_clock::time_point now, std::string& emptyText) const;
  [[nodiscard]] std::vector<netpanel::QRow> quickTailscaleRows(std::chrono::steady_clock::time_point now) const;
  void placeQuickRows(
      QuickModel& model, netpanel::QSection& section, const std::vector<netpanel::QRow>& rows, std::size_t cap,
      float usablePx
  ) const;
  std::unique_ptr<Flex> createQuick();
  // Pieces of createQuick, in the order it adds them.
  std::unique_ptr<Flex> buildQuickHero(float scale);
  std::unique_ptr<Flex> buildQuickTrafficBox(float scale);
  std::unique_ptr<Flex> buildQuickGraphAxis(float scale);
  std::unique_ptr<Box> buildQuickGraphBox(float scale);
  std::unique_ptr<Flex> buildPasswordCard(float scale);
  // Pieces of buildPasswordCard, in the order it adds them.
  std::unique_ptr<Flex> buildEnterpriseFields(float scale);
  std::unique_ptr<Flex> buildEapRow(float scale);
  std::unique_ptr<Flex> buildPasswordRow(float scale);
  void doLayoutQuick(Renderer& renderer, float contentWidth, float bodyHeight);
  void syncQuick(Renderer& renderer, bool force);
  void rebuildQuickBody(const QuickModel& model, float scale);
  // Pieces of rebuildQuickBody.
  [[nodiscard]] std::string quickClearBody();
  void quickRenderElems(Flex& parent, const netpanel::QSection& section, const std::string& selectedId, float scale);
  void quickFillBox(
      Flex* holder, const netpanel::QSection& section, std::size_t& numeral, const std::string& selectedId, float scale
  );
  [[nodiscard]] std::unique_ptr<Button> quickRowButton(const netpanel::QRow& row, bool selected, float scale);
  [[nodiscard]] std::unique_ptr<Flex> quickMenuBlock(const netpanel::QElem& elem, float scale);
  [[nodiscard]] std::unique_ptr<Flex> quickPasswordRow(float scale);
  void syncQuickTraffic(Renderer& renderer);
  void quickTick();
  void quickRefreshProfiles();
  void quickRefreshTailscale();
  void quickProbeLatency();
  void quickProbe(const std::string& host, netq::Probe& slot);
  void quickHeroText(std::string& main, std::string& dim) const;
  [[nodiscard]] std::string quickLatencyMarkup() const;
  void quickTrackPasswordOwner();
  void quickSyncAttempts();
  void quickPruneNotes();
  void quickRequestUpdate() const;
  void quickSetNote(const std::string& rowId, std::string text, NoteTone tone, std::chrono::milliseconds ttl);
  void quickSetBusy(const std::string& rowId, std::string text);
  void quickClearNote(const std::string& rowId);
  void quickMoveSelection(int delta);
  void quickEnter();
  void quickToggleMenu();
  void quickDisarm();
  // Runs menu item `itemId` of row `rowId`; destructive items ask for a second press first.
  void quickRunItem(const std::string& rowId, const std::string& itemId, bool destructive);
  [[nodiscard]] bool quickConfirmDestructive(const std::string& rowId, const std::string& itemId);
  void quickRunIpItem(const std::string& rowId, const std::string& itemId);
  void quickRunProfileItem(const std::string& rowId, const std::string& itemId);
  void quickRunTailscaleItem(const std::string& rowId, const std::string& itemId);
  void quickRunRowCommand(
      const std::string& rowId, std::vector<std::string> args, std::chrono::seconds timeout,
      void (NetworkTab::*refresh)()
  );
  void quickConnectProfile(const netq::Profile& profile);
  void quickDisconnectProfile(const netq::Profile& profile);
  void quickCopyProfileIp(const std::string& rowId, const netq::Profile& profile);
  void quickConnectAccessPoint(const AccessPointInfo& ap);
  void quickTailscaleToggle();
  void quickTailscaleExit(const std::string& target, const std::string& label);
  void quickCopy(const std::string& rowId, const std::string& text);
  void quickToggleWifi();
  void quickEditConnections(const std::string& uuid, const std::string& name);
  [[nodiscard]] bool quickTextInputFocused() const;
  void resetQuick();
  // Settles the pending Wi-Fi toggle once the requested state is observed.
  void settleWifiToggle(const NetworkState& state);

  // A backend exists and its daemon is on the bus.
  [[nodiscard]] bool networkAvailable() const noexcept;
  void syncCurrentCard();
  void beginPendingAction(bool wasConnected);
  void requestWirelessEnabled(bool enabled);
  void handleWirelessEnabledCompletion(std::uint64_t generation, bool success);
  void rebuildApList(Renderer& renderer);
  // Pushes live signal values into the existing rows. Returns true if any changed.
  bool syncApRows();
  // Same for the cellular rows and the cellular toggle. Returns true if any changed.
  bool syncCellularCard();
  // GNOME-style mobile-data semantics when the network backend owns a saved gsm
  // connection (toggle reflects cellularActive); otherwise raw modem power.
  [[nodiscard]] bool cellularToggleChecked() const;
  [[nodiscard]] bool cellularToggleDisplayChecked() const;
  void requestCellularEnabled(bool enabled);
  void syncPasswordCard();
  // Moves keyboard focus into the prompt once after it appears on the active tab.
  void focusPendingPasswordPrompt();
  void showPasswordPrompt(const NetworkSecretAgent::SecretRequest& request);
  void showPasswordPrompt(const AccessPointInfo& ap);
  void submitPasswordPrompt(const std::string& value);
  void cancelPasswordPrompt();
  void clearPasswordPrompt();
  // Reads the enterprise form back out. Password comes from the shared field.
  [[nodiscard]] network_enterprise::EnterpriseCredentials
  collectEnterpriseCredentials(const std::string& password) const;
  // Shows a message inside the credential card; empty hides the row. The prompt
  // stays open so the user can correct what is wrong.
  void setCredentialError(const std::string& message);
  // Reason this access point cannot be joined with a password, empty when it can.
  [[nodiscard]] std::string enterpriseBlockReason(const AccessPointInfo& ap) const;
  [[nodiscard]] std::string
  structureKey(const std::vector<AccessPointInfo>& aps, const std::vector<VpnConnectionInfo>& vpns) const;

  INetworkService* m_network = nullptr;
  NetworkSecretAgent* m_secrets = nullptr;
  ExternalIpService* m_externalIpService = nullptr;
  ModemManagerService* m_modem = nullptr;

  Flex* m_rootLayout = nullptr;
  Flex* m_currentCard = nullptr;
  Label* m_currentTitle = nullptr;
  Label* m_currentDetail = nullptr;
  Flex* m_passwordCard = nullptr;
  Label* m_passwordTitle = nullptr;
  Input* m_passwordInput = nullptr;
  Button* m_passwordRevealButton = nullptr;
  // 802.1X form. Hidden for pre-shared-key networks, which keep the single
  // password field below it.
  Flex* m_enterpriseFields = nullptr;
  Select* m_eapSelect = nullptr;
  Select* m_phase2Select = nullptr;
  Input* m_identityInput = nullptr;
  Input* m_anonymousIdentityInput = nullptr;
  Input* m_caCertInput = nullptr;
  Input* m_domainMatchInput = nullptr;
  Label* m_credentialError = nullptr;
  ScrollView* m_listScroll = nullptr;
  Flex* m_list = nullptr;

  Button* m_rescanButton = nullptr;
  Toggle* m_wifiToggle = nullptr;
  Flex* m_currentRow = nullptr;
  Button* m_disconnectButton = nullptr;
  Spinner* m_scanSpinner = nullptr;
  bool m_vpnVisible = true;

  std::unordered_map<std::string, AccessPointRow*> m_apRows;

  Toggle* m_cellularToggle = nullptr;
  std::vector<CellularRow*> m_cellularRows;

  // Quick mode.
  bool m_quick = false;
  SystemMonitorService* m_monitor = nullptr;
  TextClipboard* m_clipboard = nullptr;
  Label* m_qHeroMain = nullptr;
  Label* m_qHeroDim = nullptr;
  Label* m_qLatency = nullptr;
  Flex* m_qIpHolder = nullptr;
  Flex* m_qConnHolder = nullptr;
  Flex* m_qWifiHolder = nullptr;
  Flex* m_qTsHolder = nullptr;
  Flex* m_qTrafficBox = nullptr;
  Label* m_qTrafficTitle = nullptr;
  Label* m_qAxisTop = nullptr;
  Label* m_qAxisBottom = nullptr;
  Box* m_qGraphBox = nullptr;
  Graph* m_qGraph = nullptr;
  Label* m_qTrafficText = nullptr;
  Input* m_qPwInput = nullptr;
  std::vector<std::unique_ptr<term_frame::Keyword>> m_qKeywords;
  QuickSnapshot m_qSnap;
  std::string m_qBodyKey;
  std::string m_qHeightKey;
  std::string m_qSel;     // id of the selected row
  std::string m_qPwOwner; // row the credential prompt belongs to
  std::vector<std::string> m_qSelectables;
  std::vector<MenuKey> m_qMenuKeys; // keys of the open action menu
  bool m_qSelHasMenu = false;
  std::string m_qMenuId; // row whose action menu is open
  std::string m_qArmed;  // "<row>|<item>" waiting for its confirming second press
  std::chrono::steady_clock::time_point m_qArmedUntil;
  std::unordered_map<std::string, Note> m_qNotes;
  mutable std::unordered_map<std::string, std::size_t> m_qWinStart;
  std::vector<netq::Profile> m_qProfiles;
  bool m_qProfilesLoaded = false;
  bool m_qProfilesInFlight = false;
  netq::TsState m_qTs;
  bool m_qTsInFlight = false;
  bool m_qHasNmcli = false;
  bool m_qHasTailscale = false;
  netq::Probe m_qPingGateway;
  netq::Probe m_qPingInternet;
  std::string m_qPingGatewayHost;
  std::optional<WifiAttempt> m_qAttempt;
  bool m_qTrafficDirty = true;
  bool m_qFocusPassword = false;
  float m_qGraphWidth = 0.0F;
  float m_qWidth = 440.0F;
  std::uint32_t m_qHeldKey = 0;
  std::chrono::steady_clock::time_point m_qLastPoll;
  std::chrono::steady_clock::time_point m_qProfilesAt;
  std::chrono::steady_clock::time_point m_qTsAt;
  std::chrono::steady_clock::time_point m_qPingAt;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  Timer m_qPollTimer;
  Timer m_qArmTimer;

  std::string m_lastStructureKey;
  float m_lastListWidth = -1.0F;

  bool m_hasPendingSecret = false;
  bool m_pendingEnterprise = false;
  bool m_focusPasswordPrompt = false;
  std::string m_pendingSsid;
  std::optional<AccessPointInfo> m_pendingAccessPoint;
  bool m_active = false;

  // Connect/disconnect stays disabled from click until the state flips (or a
  // timeout), so a click on stale state cannot fire the inverse action.
  bool m_actionPending = false;
  bool m_actionPendingConnected = false;
  std::chrono::steady_clock::time_point m_actionPendingSince;

  bool m_wifiTogglePending = false;
  bool m_wifiToggleTarget = false;
  bool m_wifiToggleWriteComplete = false;
  bool m_wifiToggleTargetObserved = false;
  std::uint64_t m_wifiToggleRequestGeneration = 0;

  // A cellular request is only observable once ModemManager and NM have walked
  // the modem through enable/registration, so the switch shows the requested
  // position until then. It stays clickable: a second click just retargets.
  bool m_cellularTogglePending = false;
  bool m_cellularToggleTarget = false;
  std::chrono::steady_clock::time_point m_cellularTogglePendingSince;

  Timer m_actionPendingTimer;
  Timer m_cellularTogglePendingTimer;

  static constexpr std::chrono::seconds kActionPendingTimeout = std::chrono::seconds(6);
  static constexpr std::chrono::seconds kCellularPendingTimeout = std::chrono::seconds(25);
};