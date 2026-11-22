#include "shell/control_center/tabs/network_tab.h"

#include "core/process/process.h"
#include "core/text_clipboard.h"
#include "core/ui_phase.h"
#include "dbus/modem/modem_manager_service.h"
#include "dbus/network/external_ip_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_display.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "shell/control_center/tabs/async_command.h"
#include "shell/control_center/term_frame.h"
#include "shell/panel/panel_manager.h"
#include "system/system_monitor_service.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/graph.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <ifaddrs.h>
#include <memory>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

using namespace control_center;

namespace {

  constexpr float kRowMinHeight = Style::controlHeightLg;

  // Label and wire value in one row, so the visible dropdown order and the value
  // sent to NetworkManager cannot drift apart.
  struct EapOption {
    std::string_view label;
    network_enterprise::EapMethod method;
  };

  struct Phase2Option {
    std::string_view label;
    network_enterprise::Phase2Auth auth;
  };

  constexpr std::array kEapOptions{
      EapOption{"PEAP", network_enterprise::EapMethod::Peap},
      EapOption{"TTLS", network_enterprise::EapMethod::Ttls},
  };

  // MSCHAPv2 first: it is the only inner method PEAP accepts, and PEAP is the
  // default EAP method.
  constexpr std::array kPhase2Options{
      Phase2Option{"MSCHAPv2", network_enterprise::Phase2Auth::MsChapV2},
      Phase2Option{"PAP", network_enterprise::Phase2Auth::Pap},
      Phase2Option{"MSCHAP", network_enterprise::Phase2Auth::MsChap},
      Phase2Option{"CHAP", network_enterprise::Phase2Auth::Chap},
  };

  constexpr std::size_t phase2IndexOf(network_enterprise::Phase2Auth auth) {
    for (std::size_t index = 0U; index < kPhase2Options.size(); ++index) {
      if (kPhase2Options[index].auth == auth) {
        return index;
      }
    }
    return kPhase2Options.size();
  }

  // Both dropdowns open on their first option, and PEAP accepts MSCHAPv2 only, so
  // choosing PEAP pins the inner dropdown to this row.
  constexpr std::size_t kDefaultOptionIndex = 0U;
  constexpr std::size_t kPeapPhase2Index = phase2IndexOf(network_enterprise::Phase2Auth::MsChapV2);
  static_assert(kPeapPhase2Index < kPhase2Options.size());
  static_assert(kEapOptions[kDefaultOptionIndex].method == network_enterprise::EapMethod::Peap);
  static_assert(kPeapPhase2Index == kDefaultOptionIndex);

  template <typename Options> std::vector<std::string> optionLabels(const Options& options) {
    std::vector<std::string> labels;
    labels.reserve(options.size());
    for (const auto& option : options) {
      labels.emplace_back(option.label);
    }
    return labels;
  }

  // A Select with no selection reports npos; the first option is the documented
  // default for both dropdowns.
  template <typename Options> const auto& optionAt(const Options& options, std::size_t index) {
    return index < options.size() ? options[index] : options.front();
  }

  std::string validationMessage(network_enterprise::Validation problem) {
    switch (problem) {
    case network_enterprise::Validation::MissingIdentity:
      return i18n::tr("control-center.network.error-missing-identity");
    case network_enterprise::Validation::MissingPassword:
      return i18n::tr("control-center.network.error-missing-password");
    case network_enterprise::Validation::UnsupportedPhase2:
      return i18n::tr("control-center.network.error-unsupported-phase2");
    case network_enterprise::Validation::CaCertNotAbsolute:
      return i18n::tr("control-center.network.error-ca-not-absolute");
    case network_enterprise::Validation::Ok:
      break;
    }
    return {};
  }

  std::string currentTitle(const NetworkState& s, const CellularModemInfo* modem) {
    if (s.kind == NetworkConnectivity::Wireless && s.connected && !s.ssid.empty()) {
      return s.ssid;
    }
    if (s.kind == NetworkConnectivity::Wired && s.connected) {
      return s.interfaceName.empty() ? i18n::tr("control-center.network.wired-connection") : s.interfaceName;
    }
    if (s.kind == NetworkConnectivity::Cellular && s.connected) {
      if (modem != nullptr && !modem->operatorName.empty()) {
        return modem->operatorName;
      }
      return i18n::tr("control-center.network.cellular-connection");
    }
    return i18n::tr("control-center.network.not-connected");
  }

  std::string currentDetail(const NetworkState& s, const std::string& externalIp, const CellularModemInfo* modem) {
    if (!s.connected) {
      if (s.kind == NetworkConnectivity::Cellular && modem != nullptr) {
        return cellularStateText(modem->state);
      }
      return s.wirelessEnabled ? i18n::tr("control-center.network.wifi-on")
                               : i18n::tr("control-center.network.wifi-off");
    }
    std::string out;
    const auto append = [&out](std::string_view part) {
      if (!out.empty()) {
        out += "  •  ";
      }
      out += part;
    };
    if (!s.ipv4.empty()) {
      append(s.ipv4);
    }
    if (s.kind == NetworkConnectivity::Wireless) {
      if (s.signalStrength > 0) {
        append(std::to_string(static_cast<int>(s.signalStrength)) + "%");
      }
      if (const char* band = network_display::wifiFrequencyBandLabel(s.frequencyMhz); band != nullptr) {
        append(band);
      }
    }
    if (s.kind == NetworkConnectivity::Cellular && modem != nullptr) {
      if (!out.empty()) {
        out += "  •  ";
      }
      out += std::to_string(static_cast<int>(modem->signalQuality)) + "%";
      if (const char* tech = cellularAccessTechnologyName(modem->accessTechnologies); tech[0] != '\0') {
        out += "  •  ";
        out += tech;
      }
    }
    if (!externalIp.empty()) {
      append(i18n::tr("control-center.network.external-ip", "ip", externalIp));
    }
    return out;
  }

  const char* cellularGlyphFor(const CellularModemInfo& modem) {
    return modem.enabled() ? network_display::cellularGlyphForSignal(modem.signalQuality)
                           : network_display::cellularOffGlyph();
  }

  std::string cellularTitleFor(const CellularModemInfo& modem) {
    if (!modem.operatorName.empty()) {
      return modem.operatorName;
    }
    if (!modem.name.empty()) {
      return modem.name;
    }
    return i18n::tr("control-center.network.cellular");
  }

  std::string cellularDetailFor(const CellularModemInfo& modem) {
    std::string out = cellularStateText(modem.state);
    if (modem.enabled()) {
      if (const char* tech = cellularAccessTechnologyName(modem.accessTechnologies); tech[0] != '\0') {
        out += "  •  ";
        out += tech;
      }
      out += "  •  " + std::to_string(static_cast<int>(modem.signalQuality)) + "%";
    }
    return out;
  }

  std::string percentText(std::uint8_t percent) { return std::to_string(static_cast<int>(percent)) + "%"; }

  std::unique_ptr<Flex> makeWifiBucketHeaderRow(const std::string& title, float scale) {
    auto row = ui::row({
        .align = FlexAlign::Center,
    });

    auto pill = ui::row({
        .align = FlexAlign::Center,
        .paddingV = Style::spaceXs * 0.5F * scale,
        .paddingH = Style::spaceSm * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.8F),
        .radius = Style::scaledRadiusSm(scale),
    });
    pill->addChild(
        ui::label({
            .text = title,
            .fontSize = Style::fontSizeMini * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    row->addChild(std::move(pill));
    return row;
  }

  // Active first, then by signal band, then by name. Ordering on the raw strength —
  // as the services do — reshuffles rows on every scan update, moving the row you are
  // aiming at out from under the pointer.
  std::vector<AccessPointInfo> sortedAccessPoints(std::vector<AccessPointInfo> aps) {
    std::ranges::sort(aps, [](const AccessPointInfo& a, const AccessPointInfo& b) {
      if (a.active != b.active) {
        return a.active;
      }
      const int bandA = network_display::wifiSignalBand(a.strength);
      const int bandB = network_display::wifiSignalBand(b.strength);
      if (bandA != bandB) {
        return bandA > bandB;
      }
      return a.ssid < b.ssid;
    });
    return aps;
  }

} // namespace

class AccessPointRow : public Flex {
public:
  AccessPointRow(
      float scale, AccessPointInfo ap, bool saved, std::function<void(const AccessPointInfo&)> onActivate,
      std::function<void(const AccessPointInfo&)> onForget
  )
      : m_ap(std::move(ap)), m_onActivate(std::move(onActivate)), m_onForget(std::move(onForget)) {
    setDirection(FlexDirection::Horizontal);
    setAlign(FlexAlign::Center);
    setGap(Style::spaceSm * scale);
    setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
    setMinHeight(kRowMinHeight * scale);
    setRadius(Style::scaledRadiusMd(scale));
    setFill(colorSpecFromRole(ColorRole::Surface));
    clearBorder();

    addChild(
        ui::glyph({
            .out = &m_signalGlyph,
            .glyph = network_display::wifiGlyphForSignal(m_ap.strength),
            .glyphSize = Style::baseGlyphSize * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );

    addChild(
        ui::label({
            .out = &m_title,
            .text = m_ap.ssid,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = m_ap.active ? FontWeight::Bold : FontWeight::Normal,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );

    if (m_ap.secured) {
      addChild(
          ui::glyph({
              .glyph = "lock",
              .glyphSize = Style::baseGlyphSize * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    }

    addChild(
        ui::label({
            .out = &m_signalValue,
            .text = percentText(m_ap.strength),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );

    const float actionOpacity = (m_ap.active || saved) ? 1.0F : 0.0F;
    auto action = ui::button({
        .out = &m_actionButton,
        .glyphSize = Style::baseGlyphSize * scale,
        .variant = ButtonVariant::Ghost,
        .padding = Style::spaceXs * scale,
        .radius = Style::scaledRadiusSm(scale),
        .opacity = actionOpacity,
    });
    if (m_ap.active) {
      action->setGlyph("check");
    } else if (saved) {
      action->setGlyph("trash");
      action->setOnClick([this]() {
        if (m_onForget) {
          m_onForget(m_ap);
        }
      });
    }
    addChild(std::move(action));

    auto area = ui::inputArea({});
    area->setPropagateEvents(true);
    area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyState(); });
    area->setOnLeave([this]() { applyState(); });
    area->setOnPress([this](const InputArea::PointerData& /*data*/) { applyState(); });
    area->setOnClick([this](const InputArea::PointerData& /*data*/) {
      if (m_onActivate) {
        m_onActivate(m_ap);
      }
    });
    m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));

    applyState();
    m_paletteConn = paletteChanged().connect([this] { applyState(); });
  }

  void doLayout(Renderer& renderer) override {
    if (m_inputArea == nullptr) {
      return;
    }
    m_inputArea->setVisible(false);
    Flex::doLayout(renderer);
    m_inputArea->setVisible(true);
    m_inputArea->setPosition(0.0F, 0.0F);
    m_inputArea->setSize(width(), height());
    if (m_actionButton != nullptr) {
      const float areaWidth = std::max(0.0F, m_actionButton->x() - gap());
      m_inputArea->setSize(areaWidth, height());
    }
    applyState();
  }

  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
    return measureByLayout(renderer, constraints);
  }

  void doArrange(Renderer& renderer, const LayoutRect& rect) override { arrangeByLayout(renderer, rect); }

  [[nodiscard]] const std::string& ssid() const noexcept { return m_ap.ssid; }

  // Refresh the values that move while a scan runs — the signal glyph and percent.
  // Everything else about the row is structural and belongs to the list key.
  // Returns true when a value actually changed, i.e. the row needs relayout.
  bool syncLiveMetrics(const AccessPointInfo& ap) {
    bool changed = false;
    if (m_signalGlyph != nullptr && m_signalGlyph->setGlyph(network_display::wifiGlyphForSignal(ap.strength))) {
      changed = true;
    }
    if (m_signalValue != nullptr && m_signalValue->setText(percentText(ap.strength))) {
      changed = true;
    }
    m_ap.strength = ap.strength;
    return changed;
  }

private:
  void applyState() {
    const bool hov = m_inputArea != nullptr && m_inputArea->hovered();
    const bool pressed = m_inputArea != nullptr && m_inputArea->pressed();
    if (pressed) {
      setFill(colorSpecFromRole(ColorRole::Primary));
      setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
      if (m_title != nullptr) {
        m_title->setColor(colorSpecFromRole(ColorRole::OnPrimary));
      }
    } else {
      setFill(colorSpecFromRole(ColorRole::Surface));
      if (hov) {
        setBorder(colorSpecFromRole(ColorRole::Hover), Style::borderWidth);
      } else {
        clearBorder();
      }
      if (m_title != nullptr) {
        m_title->setColor(colorSpecFromRole(ColorRole::OnSurface));
      }
    }
  }

  AccessPointInfo m_ap;
  std::function<void(const AccessPointInfo&)> m_onActivate;
  std::function<void(const AccessPointInfo&)> m_onForget;
  Label* m_title = nullptr;
  Button* m_actionButton = nullptr;
  InputArea* m_inputArea = nullptr;
  Glyph* m_signalGlyph = nullptr;
  Label* m_signalValue = nullptr;
  Signal<>::ScopedConnection m_paletteConn;
};

namespace {

  class VpnConnectionRow : public Flex {
  public:
    VpnConnectionRow(
        float scale, VpnConnectionInfo vpn, std::function<void(const VpnConnectionInfo&)> onActivate,
        std::function<void(const VpnConnectionInfo&)> onDeactivate
    )
        : m_vpn(std::move(vpn)), m_onActivate(std::move(onActivate)), m_onDeactivate(std::move(onDeactivate)) {
      setDirection(FlexDirection::Horizontal);
      setAlign(FlexAlign::Center);
      setGap(Style::spaceSm * scale);
      setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
      setMinHeight(kRowMinHeight * scale);
      setRadius(Style::scaledRadiusMd(scale));
      setFill(colorSpecFromRole(ColorRole::Surface));
      clearBorder();

      addChild(
          ui::label({
              .out = &m_title,
              .text = m_vpn.name,
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = m_vpn.active ? FontWeight::Bold : FontWeight::Normal,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .flexGrow = 1.0F,
          })
      );

      addChild(
          ui::button({
              .out = &m_checkButton,
              .glyph = "check",
              .glyphSize = Style::baseGlyphSize * scale,
              .variant = ButtonVariant::Ghost,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .opacity = m_vpn.active ? 1.0F : 0.0F,
          })
      );

      addChild(
          ui::button({
              .out = &m_actionButton,
              .glyph = m_vpn.active ? "plug-off" : "plug",
              .glyphSize = Style::baseGlyphSize * scale,
              .variant = m_vpn.active ? ButtonVariant::Destructive : ButtonVariant::Default,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = [this]() { triggerAction(); },
          })
      );

      auto area = ui::inputArea({});
      area->setPropagateEvents(true);
      area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyState(); });
      area->setOnLeave([this]() { applyState(); });
      area->setOnPress([this](const InputArea::PointerData& /*data*/) { applyState(); });
      area->setOnClick([this](const InputArea::PointerData& /*data*/) { triggerAction(); });
      m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));

      applyState();
      m_paletteConn = paletteChanged().connect([this] { applyState(); });
    }

    void doLayout(Renderer& renderer) override {
      if (m_inputArea == nullptr) {
        return;
      }
      m_inputArea->setVisible(false);
      Flex::doLayout(renderer);
      m_inputArea->setVisible(true);
      m_inputArea->setPosition(0.0F, 0.0F);
      m_inputArea->setSize(width(), height());
      if (m_actionButton != nullptr) {
        const float areaWidth = std::max(0.0F, m_actionButton->x() - gap());
        m_inputArea->setSize(areaWidth, height());
      }
      applyState();
    }

    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
      return measureByLayout(renderer, constraints);
    }

    void doArrange(Renderer& renderer, const LayoutRect& rect) override { arrangeByLayout(renderer, rect); }

  private:
    void triggerAction() {
      if (m_vpn.active) {
        if (m_onDeactivate) {
          m_onDeactivate(m_vpn);
        }
      } else {
        if (m_onActivate) {
          m_onActivate(m_vpn);
        }
      }
    }

    void applyState() {
      const bool hov = m_inputArea != nullptr && m_inputArea->hovered();
      const bool pressed = m_inputArea != nullptr && m_inputArea->pressed();
      if (pressed) {
        setFill(colorSpecFromRole(ColorRole::Primary));
        setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
        if (m_title != nullptr) {
          m_title->setColor(colorSpecFromRole(ColorRole::OnPrimary));
        }
        return;
      }
      setFill(colorSpecFromRole(ColorRole::Surface));
      if (hov) {
        setBorder(colorSpecFromRole(ColorRole::Hover), Style::borderWidth);
      } else {
        clearBorder();
      }
      if (m_title != nullptr) {
        m_title->setColor(colorSpecFromRole(ColorRole::OnSurface));
      }
    }

    VpnConnectionInfo m_vpn;
    std::function<void(const VpnConnectionInfo&)> m_onActivate;
    std::function<void(const VpnConnectionInfo&)> m_onDeactivate;
    Label* m_title = nullptr;
    Button* m_checkButton = nullptr;
    Button* m_actionButton = nullptr;
    InputArea* m_inputArea = nullptr;
    Signal<>::ScopedConnection m_paletteConn;
  };

} // namespace

// Informational modem row: signal glyph, operator/modem name, and live status
// detail. It has no actions of its own; the card header carries the enable toggle.
class CellularRow : public Flex {
public:
  CellularRow(float scale, CellularModemInfo modem) : m_modem(std::move(modem)) {
    setDirection(FlexDirection::Horizontal);
    setAlign(FlexAlign::Center);
    setGap(Style::spaceSm * scale);
    setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
    setMinHeight(kRowMinHeight * scale);
    setRadius(Style::scaledRadiusMd(scale));
    setFill(colorSpecFromRole(ColorRole::Surface));
    clearBorder();

    addChild(
        ui::glyph({
            .out = &m_signalGlyph,
            .glyph = cellularGlyphFor(m_modem),
            .glyphSize = Style::baseGlyphSize * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );

    addChild(
        ui::label({
            .out = &m_title,
            .text = cellularTitleFor(m_modem),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );

    addChild(
        ui::label({
            .out = &m_detail,
            .text = cellularDetailFor(m_modem),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
  }

  // Push live modem values into the row. Returns true when a value actually changed.
  bool syncLive(const CellularModemInfo& modem) {
    bool changed = false;
    if (m_signalGlyph != nullptr && m_signalGlyph->setGlyph(cellularGlyphFor(modem))) {
      changed = true;
    }
    if (m_title != nullptr && m_title->setText(cellularTitleFor(modem))) {
      changed = true;
    }
    if (m_detail != nullptr && m_detail->setText(cellularDetailFor(modem))) {
      changed = true;
    }
    m_modem = modem;
    return changed;
  }

private:
  CellularModemInfo m_modem;
  Glyph* m_signalGlyph = nullptr;
  Label* m_title = nullptr;
  Label* m_detail = nullptr;
};

NetworkTab::NetworkTab(
    INetworkService* network, NetworkSecretAgent* secrets, ExternalIpService* externalIp, ModemManagerService* modem,
    SystemMonitorService* monitor, TextClipboard* clipboard, bool quick
)
    : m_network(network), m_secrets(secrets), m_externalIpService(externalIp), m_modem(modem), m_quick(quick),
      m_monitor(monitor), m_clipboard(clipboard) {
  if (m_secrets != nullptr) {
    m_secrets->setRequestCallback([this](const NetworkSecretAgent::SecretRequest& request) {
      showPasswordPrompt(request);
      PanelManager::instance().refresh();
    });
  }
  if (m_quick) {
    // Looked up once; the first lists are read now so the panel opens with them.
    m_qHasNmcli = process::commandExists("nmcli");
    m_qHasTailscale = process::commandExists("tailscale");
    quickRefreshProfiles();
    quickRefreshTailscale();
  }
}

NetworkTab::~NetworkTab() {
  *m_alive = false;
  if (m_secrets != nullptr) {
    m_secrets->setRequestCallback(nullptr);
    m_secrets->cancelSecret();
  }
}

std::unique_ptr<Flex> NetworkTab::create() {
  if (m_quick) {
    return createQuick();
  }
  const float scale = contentScale();

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto currentCard = ui::column({
      .out = &m_currentCard,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) { applySectionCardStyle(card, scale, opacity); },
  });
  addTitle(*currentCard, i18n::tr("control-center.network.current-connection"), scale);

  auto connRow = ui::row(
      {.out = &m_currentRow, .align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::label({
          .out = &m_currentTitle,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      }),
      ui::label({
          .out = &m_currentDetail,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .flexGrow = 1.0F,
      }),
      ui::button({
          .out = &m_disconnectButton,
          .glyph = "plug-off",
          .glyphSize = Style::baseGlyphSize * scale,
          .variant = ButtonVariant::Destructive,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusSm(scale),
          .onClick = [this]() {
            if (m_network == nullptr || m_actionPending) {
              return;
            }
            const bool wasConnected = m_network->state().connected;
            if (wasConnected) {
              m_network->disconnect();
            } else if (!m_network->activateWiredConnection()) {
              return;
            }
            beginPendingAction(wasConnected);
            PanelManager::instance().refresh();
          },
      })
  );
  currentCard->addChild(std::move(connRow));

  tab->addChild(std::move(currentCard));

  auto listScroll = ui::scrollView({
      .out = &m_listScroll,
      .contentScale = contentScale(),
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = 1.0F,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  // The password card scrolls with the list so a tall 802.1X form stays reachable.
  // m_list is a separate column because rebuildApList() clears all of its children.
  Flex* scrollContent = listScroll->content();
  scrollContent->setDirection(FlexDirection::Vertical);
  scrollContent->setAlign(FlexAlign::Stretch);
  scrollContent->setGap(Style::spaceMd * scale);
  scrollContent->addChild(buildPasswordCard(scale));
  scrollContent->addChild(
      ui::column({
          .out = &m_list,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceMd * scale,
      })
  );

  tab->addChild(std::move(listScroll));

  // NetworkManager's own editor covers what this tab does not: static addresses, DNS, proxies, profiles.
  tab->addChild(
      term_frame::tile(
          {
              .glyph = "settings",
              .glyphSize = Style::baseGlyphSize * scale,
              .label = "edit connections",
              .tooltip = "nmtui",
              .onClick =
                  []() {
                    if (process::runAsync({"uwsm-app", "--", "ghostty", "-e", "nmtui"})) {
                      PanelManager::instance().close();
                    }
                  },
          },
          scale
      )
  );
  return tab;
}

std::unique_ptr<Flex> NetworkTab::createHeaderActions() { return nullptr; }

void NetworkTab::setActive(bool active) {
  if (m_active == active) {
    return;
  }
  m_active = active;
  if (m_quick) {
    if (m_active) {
      // The monitor samples on its own cadence; a 1 Hz tick follows link, traffic and Wi-Fi, and drives the
      // profile, tailscale and latency refreshes.
      m_qPollTimer.startRepeating(std::chrono::milliseconds(1000), [this]() {
        quickTick();
        quickRequestUpdate();
      });
      m_qLastPoll = {};
      m_qProfilesAt = {};
      m_qTsAt = {};
      m_qPingAt = {};
      quickTick();
    } else {
      m_qPollTimer.stop();
    }
  }
  if (m_active && m_network != nullptr) {
    m_network->requestScan();
  }
}

void NetworkTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_quick) {
    doLayoutQuick(renderer, contentWidth, bodyHeight);
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  syncPasswordCard();
  rebuildApList(renderer);
  syncApRows();
  syncCellularCard();
  syncCurrentCard();
  m_rootLayout->layout(renderer);
}

void NetworkTab::doUpdate(Renderer& renderer) {
  if (m_quick) {
    syncQuick(renderer, false);
    return;
  }
  syncPasswordCard();
  focusPendingPasswordPrompt();
  rebuildApList(renderer);
  // A signal percent's text changes its width, so the list has to be laid out again.
  bool listChanged = syncApRows();
  listChanged = syncCellularCard() || listChanged;
  if (listChanged && m_list != nullptr) {
    m_list->layout(renderer);
  }
  syncCurrentCard();
}

void NetworkTab::onClose() {
  resetQuick();
  m_rootLayout = nullptr;
  m_currentCard = nullptr;
  m_currentTitle = nullptr;
  m_currentDetail = nullptr;
  m_passwordCard = nullptr;
  m_passwordTitle = nullptr;
  m_passwordInput = nullptr;
  m_passwordRevealButton = nullptr;
  m_enterpriseFields = nullptr;
  m_eapSelect = nullptr;
  m_phase2Select = nullptr;
  m_identityInput = nullptr;
  m_anonymousIdentityInput = nullptr;
  m_caCertInput = nullptr;
  m_domainMatchInput = nullptr;
  m_credentialError = nullptr;
  m_listScroll = nullptr;
  m_list = nullptr;
  m_rescanButton = nullptr;
  m_wifiToggle = nullptr;
  m_scanSpinner = nullptr;
  m_currentRow = nullptr;
  m_disconnectButton = nullptr;
  m_cellularToggle = nullptr;
  m_cellularRows.clear();
  m_cellularTogglePending = false;
  m_cellularTogglePendingTimer.stop();
  m_apRows.clear();
  m_lastStructureKey.clear();
  m_lastListWidth = -1.0F;
  m_pendingAccessPoint.reset();
  m_active = false;
  m_actionPending = false;
  m_actionPendingTimer.stop();
  m_wifiTogglePending = false;
  m_wifiToggleWriteComplete = false;
  m_wifiToggleTargetObserved = false;
  ++m_wifiToggleRequestGeneration;
}

void NetworkTab::syncPasswordCard() {
  if (m_hasPendingSecret
      && !m_pendingAccessPoint.has_value()
      && m_secrets != nullptr
      && !m_secrets->hasPendingRequest()) {
    clearPasswordPrompt();
  }
  if (m_passwordCard == nullptr) {
    return;
  }
  m_passwordCard->setVisible(m_hasPendingSecret);
  if (m_enterpriseFields != nullptr) {
    m_enterpriseFields->setVisible(m_hasPendingSecret && m_pendingEnterprise);
  }
  if (m_hasPendingSecret && m_passwordTitle != nullptr) {
    if (m_pendingSsid.empty()) {
      m_passwordTitle->setText(i18n::tr("control-center.network.password-prompt"));
    } else if (m_pendingEnterprise) {
      m_passwordTitle->setText(i18n::tr("control-center.network.enterprise-prompt-for", "ssid", m_pendingSsid));
    } else {
      m_passwordTitle->setText(i18n::tr("control-center.network.password-prompt-for", "ssid", m_pendingSsid));
    }
  }
}

void NetworkTab::focusPendingPasswordPrompt() {
  if (!std::exchange(m_focusPasswordPrompt, false) || !m_hasPendingSecret) {
    return;
  }
  // The enterprise form starts with the identity; a plain prompt only has the password.
  Input* target = m_pendingEnterprise ? m_identityInput : m_passwordInput;
  if (target != nullptr) {
    PanelManager::instance().focusArea(target->inputArea());
  }
}

void NetworkTab::setCredentialError(const std::string& message) {
  if (m_credentialError == nullptr) {
    return;
  }
  m_credentialError->setText(message);
  m_credentialError->setVisible(!message.empty());
  // Showing or hiding the message changes the card's height, and the card is
  // above the network list.
  PanelManager::instance().requestLayout();
}

network_enterprise::EnterpriseCredentials NetworkTab::collectEnterpriseCredentials(const std::string& password) const {
  const auto selectedIndexOf = [](const Select* select) -> std::size_t {
    return select != nullptr ? select->selectedIndex() : kDefaultOptionIndex;
  };
  const auto valueOf = [](const Input* input) -> std::string {
    return input != nullptr ? input->value() : std::string{};
  };

  network_enterprise::EnterpriseCredentials credentials;
  credentials.eap = optionAt(kEapOptions, selectedIndexOf(m_eapSelect)).method;
  credentials.phase2 = optionAt(kPhase2Options, selectedIndexOf(m_phase2Select)).auth;
  credentials.identity = valueOf(m_identityInput);
  credentials.anonymousIdentity = valueOf(m_anonymousIdentityInput);
  credentials.password = password;
  credentials.caCertPath = valueOf(m_caCertInput);
  credentials.domainSuffixMatch = valueOf(m_domainMatchInput);
  return credentials;
}

std::string NetworkTab::enterpriseBlockReason(const AccessPointInfo& ap) const {
  if (m_network == nullptr || !m_network->supportsEnterprise()) {
    return i18n::tr("control-center.network.error-enterprise-unsupported");
  }
  if (!network_enterprise::passwordAuthUsable(ap.keyManagement)) {
    return i18n::tr("control-center.network.error-certificate-required");
  }
  return {};
}

void NetworkTab::showPasswordPrompt(const NetworkSecretAgent::SecretRequest& request) {
  // A form the user is filling outranks NM's own secret request: letting it
  // through would replace the card mid-typing and discard what has been entered.
  // Decline instead, so NM stops waiting rather than blocking on a prompt that
  // will never be answered.
  if (m_hasPendingSecret && m_pendingAccessPoint.has_value()) {
    if (m_secrets != nullptr) {
      m_secrets->cancelSecret();
    }
    return;
  }

  clearPasswordPrompt();
  m_hasPendingSecret = true;
  m_pendingSsid = request.ssid;
  // NM is asking for one secret against a profile it already holds, so only the
  // password is missing; the rest of the 802.1X form would have nothing to fill.
  m_pendingEnterprise = false;
  m_focusPasswordPrompt = m_active;
  PanelManager::instance().requestLayout();
}

void NetworkTab::showPasswordPrompt(const AccessPointInfo& ap) {
  clearPasswordPrompt();
  m_hasPendingSecret = true;
  m_pendingSsid = ap.ssid;
  m_pendingAccessPoint = ap;
  m_pendingEnterprise = ap.isEnterprise();
  m_focusPasswordPrompt = m_active;
  if (m_pendingEnterprise) {
    // Say up front when this network cannot be joined with a password, rather
    // than after the user has filled in the whole form.
    setCredentialError(enterpriseBlockReason(ap));
  }
  if (m_listScroll != nullptr) {
    m_listScroll->setScrollOffset(0.0F);
  }
  PanelManager::instance().requestLayout();
}

void NetworkTab::submitPasswordPrompt(const std::string& value) {
  if (m_pendingAccessPoint.has_value() && m_pendingEnterprise) {
    if (m_network == nullptr) {
      return;
    }
    // Every rejection below leaves the card open with a reason showing, so the
    // user can fix the field rather than watch the prompt close and nothing happen.
    if (const auto blocked = enterpriseBlockReason(*m_pendingAccessPoint); !blocked.empty()) {
      setCredentialError(blocked);
      PanelManager::instance().refresh();
      return;
    }
    const auto credentials = collectEnterpriseCredentials(value);
    const auto problem = network_enterprise::validate(credentials);
    if (problem != network_enterprise::Validation::Ok) {
      setCredentialError(validationMessage(problem));
      PanelManager::instance().refresh();
      return;
    }
    m_network->activateEnterpriseAccessPoint(*m_pendingAccessPoint, credentials);
  } else if (m_pendingAccessPoint.has_value()) {
    if (value.empty()) {
      return;
    }
    if (m_network != nullptr) {
      m_network->activateAccessPoint(*m_pendingAccessPoint, value);
    }
  } else if (m_secrets != nullptr) {
    m_secrets->submitSecret(value);
  }
  clearPasswordPrompt();
  PanelManager::instance().refresh();
}

void NetworkTab::cancelPasswordPrompt() {
  if (!m_pendingAccessPoint.has_value() && m_secrets != nullptr) {
    m_secrets->cancelSecret();
  }
  clearPasswordPrompt();
  PanelManager::instance().refresh();
}

void NetworkTab::clearPasswordPrompt() {
  m_hasPendingSecret = false;
  m_pendingEnterprise = false;
  m_pendingSsid.clear();
  m_pendingAccessPoint.reset();
  if (m_passwordInput != nullptr) {
    m_passwordInput->setValue("");
    m_passwordInput->setPasswordRevealed(false);
  }
  if (m_passwordRevealButton != nullptr) {
    m_passwordRevealButton->setGlyph("eye");
  }
  for (Input* input : {m_identityInput, m_anonymousIdentityInput, m_caCertInput, m_domainMatchInput}) {
    if (input != nullptr) {
      input->setValue("");
    }
  }
  if (m_eapSelect != nullptr) {
    m_eapSelect->setSelectedIndexSilently(kDefaultOptionIndex);
  }
  if (m_phase2Select != nullptr) {
    m_phase2Select->setSelectedIndexSilently(kPeapPhase2Index);
    m_phase2Select->setEnabled(false); // PEAP is the default, and it pins MSCHAPv2.
  }
  setCredentialError({});
}

bool NetworkTab::networkAvailable() const noexcept { return m_network != nullptr && m_network->available(); }

void NetworkTab::syncCurrentCard() {
  if (m_currentTitle == nullptr || m_currentDetail == nullptr) {
    return;
  }
  if (!networkAvailable()) {
    m_currentTitle->setText(i18n::tr("control-center.network.unavailable-title"));
    m_currentDetail->setText(i18n::tr("control-center.network.unavailable-detail"));
    if (m_currentRow != nullptr) {
      m_currentRow->setVisible(false);
    }
    return;
  }
  if (m_currentRow != nullptr) {
    m_currentRow->setVisible(true);
  }
  const NetworkState& s = m_network->state();
  if (m_actionPending) {
    const bool flipped = s.connected != m_actionPendingConnected;
    const bool timedOut = std::chrono::steady_clock::now() - m_actionPendingSince > kActionPendingTimeout;
    if (flipped || timedOut) {
      m_actionPending = false;
      m_actionPendingTimer.stop();
    }
  }
  settleWifiToggle(s);
  static const std::string kNoExternalIp;
  const std::string& externalIp = m_externalIpService != nullptr ? m_externalIpService->externalIp() : kNoExternalIp;
  const CellularModemInfo* modem = m_modem != nullptr ? m_modem->primaryModem() : nullptr;
  m_currentTitle->setText(currentTitle(s, modem));
  m_currentDetail->setText(currentDetail(s, externalIp, modem));
  if (m_disconnectButton != nullptr) {
    const bool canReconnectWired = !s.connected && m_network->canActivateWiredConnection();
    m_disconnectButton->setVisible(s.connected || canReconnectWired || m_actionPending);
    m_disconnectButton->setGlyph(s.connected ? "plug-off" : "plug");
    m_disconnectButton->setVariant(s.connected ? ButtonVariant::Destructive : ButtonVariant::Default);
    m_disconnectButton->setEnabled(!m_actionPending);
  }
  if (m_wifiToggle != nullptr) {
    m_wifiToggle->setChecked(m_wifiTogglePending ? m_wifiToggleTarget : s.wirelessEnabled);
    m_wifiToggle->setEnabled(!m_wifiTogglePending);
  }
  if (m_scanSpinner != nullptr) {
    m_scanSpinner->setVisible(s.scanning);
    if (s.scanning && !m_scanSpinner->spinning()) {
      m_scanSpinner->start();
    } else if (!s.scanning && m_scanSpinner->spinning()) {
      m_scanSpinner->stop();
    }
  }
}

void NetworkTab::beginPendingAction(bool wasConnected) {
  m_actionPending = true;
  m_actionPendingConnected = wasConnected;
  m_actionPendingSince = std::chrono::steady_clock::now();
  m_actionPendingTimer.start(kActionPendingTimeout + std::chrono::milliseconds(50), []() {
    PanelManager::instance().requestUpdateOnly();
    PanelManager::instance().requestRedraw();
  });
  if (m_disconnectButton != nullptr) {
    m_disconnectButton->setEnabled(false);
  }
}

void NetworkTab::requestWirelessEnabled(bool enabled) {
  m_wifiTogglePending = true;
  m_wifiToggleTarget = enabled;
  m_wifiToggleWriteComplete = false;
  m_wifiToggleTargetObserved = false;
  const std::uint64_t generation = ++m_wifiToggleRequestGeneration;
  if (m_wifiToggle != nullptr) {
    m_wifiToggle->setEnabled(false);
  }
  if (m_network == nullptr) {
    handleWirelessEnabledCompletion(generation, false);
    return;
  }
  m_network->setWirelessEnabled(enabled, [this, generation](bool success) {
    handleWirelessEnabledCompletion(generation, success);
  });
}

void NetworkTab::handleWirelessEnabledCompletion(std::uint64_t generation, bool success) {
  if (!m_wifiTogglePending || generation != m_wifiToggleRequestGeneration) {
    return;
  }
  m_wifiToggleWriteComplete = success;
  if (!success) {
    m_wifiTogglePending = false;
    m_wifiToggleTargetObserved = false;
  }
  PanelManager::instance().requestUpdateOnly();
  PanelManager::instance().requestRedraw();
}

// Identity of the built list: which rows exist, in which order, which controls
// each carries, and how each activates. The signal strength is absent by design —
// it refreshes in place through syncApRows(), so a scan update no longer tears the
// list down. Access points arrive sorted, so a change in row order changes the key.
std::string
NetworkTab::structureKey(const std::vector<AccessPointInfo>& aps, const std::vector<VpnConnectionInfo>& vpns) const {
  std::string key;
  for (const auto& ap : aps) {
    key += ap.ssid;
    key.push_back(':');
    key += ap.secured ? '1' : '0';
    key.push_back(':');
    key += static_cast<char>('0' + static_cast<int>(ap.keyManagement));
    key.push_back(':');
    key += ap.active ? '1' : '0';
    key.push_back(':');
    key += (m_network != nullptr && m_network->hasSavedConnection(ap.ssid)) ? '1' : '0';
    key.push_back('\n');
  }
  key += "---\n";
  for (const auto& vpn : vpns) {
    key += vpn.path;
    key.push_back(':');
    key += vpn.name;
    key.push_back(':');
    key += vpn.active ? '1' : '0';
    key.push_back('\n');
  }
  const bool wirelessEnabled = m_network != nullptr && m_network->state().wirelessEnabled;
  const bool scanning = m_network != nullptr && m_network->state().scanning;
  key += "avail:";
  key += networkAvailable() ? '1' : '0';
  key += "\nvis:";
  key += m_vpnVisible ? '1' : '0';
  key += "\nwifi:";
  key += wirelessEnabled ? '1' : '0';
  key += "\nscan:";
  key += scanning ? '1' : '0';
  // Cellular rows live-sync their state/signal, so only the structural
  // identity (which modems exist) belongs to the key.
  key += "\ncellular:\n";
  if (m_modem != nullptr) {
    for (const auto& modem : m_modem->modems()) {
      key += modem.path;
      key.push_back(':');
      key += modem.name;
      key.push_back('\n');
    }
  }
  return key;
}

void NetworkTab::rebuildApList(Renderer& renderer) {
  uiAssertNotRendering("NetworkTab::rebuildApList");
  if (m_list == nullptr || m_listScroll == nullptr) {
    return;
  }
  const float listWidth = m_listScroll->contentViewportWidth();
  if (listWidth <= 0.0F) {
    return;
  }

  std::vector<AccessPointInfo> aps;
  if (m_network != nullptr) {
    aps = sortedAccessPoints(m_network->accessPoints());
  }
  const auto& vpns = m_network != nullptr ? m_network->vpnConnections() : std::vector<VpnConnectionInfo>{};
  const std::string nextStructure = structureKey(aps, vpns);
  if (listWidth == m_lastListWidth && nextStructure == m_lastStructureKey) {
    return;
  }
  m_lastListWidth = listWidth;
  m_lastStructureKey = nextStructure;
  const float scale = contentScale();

  auto buildApRows = [&]() {
    auto container = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
    });
    if (aps.empty()) {
      container->addChild(
          ui::label({
              .text = i18n::tr("control-center.network.no-networks"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    } else {
      std::vector<AccessPointInfo> activeAps;
      std::vector<AccessPointInfo> savedAps;
      std::vector<AccessPointInfo> availableAps;
      activeAps.reserve(aps.size());
      savedAps.reserve(aps.size());
      availableAps.reserve(aps.size());

      for (const auto& ap : aps) {
        const bool saved = m_network != nullptr && m_network->hasSavedConnection(ap.ssid);
        if (ap.active) {
          activeAps.push_back(ap);
        } else if (saved) {
          savedAps.push_back(ap);
        } else {
          availableAps.push_back(ap);
        }
      }

      auto addRows = [&](Flex& parent, const std::vector<AccessPointInfo>& bucket) {
        if (bucket.empty()) {
          return;
        }
        for (const auto& ap : bucket) {
          const bool saved = m_network != nullptr && m_network->hasSavedConnection(ap.ssid);
          auto row = std::make_unique<AccessPointRow>(
              scale, ap, saved,
              [this](const AccessPointInfo& clicked) {
                if (clicked.active || m_network == nullptr) {
                  return;
                }
                if (clicked.requiresCredentials() && !m_network->hasSavedConnection(clicked.ssid)) {
                  showPasswordPrompt(clicked);
                  PanelManager::instance().refresh();
                  return;
                }
                m_network->activateAccessPoint(clicked);
              },
              [this](const AccessPointInfo& clicked) {
                if (m_network != nullptr) {
                  m_network->forgetSsid(clicked.ssid);
                }
                PanelManager::instance().refresh();
              }
          );
          auto* rowPtr = row.get();
          parent.addChild(std::move(row));
          m_apRows.emplace(rowPtr->ssid(), rowPtr);
        }
      };

      addRows(*container, activeAps);

      auto addBucket = [&](const std::string& title, const std::vector<AccessPointInfo>& bucket) {
        if (bucket.empty()) {
          return;
        }
        auto group = ui::column({
            .align = FlexAlign::Stretch,
            .gap = Style::spaceXs * scale,
        });
        group->addChild(makeWifiBucketHeaderRow(title, scale));
        addRows(*group, bucket);
        container->addChild(std::move(group));
      };

      addBucket(i18n::tr("control-center.network.saved"), savedAps);
      addBucket(i18n::tr("control-center.network.available"), availableAps);
    }
    return container;
  };

  m_wifiToggle = nullptr;
  m_scanSpinner = nullptr;
  m_rescanButton = nullptr;
  m_cellularToggle = nullptr;
  m_cellularRows.clear();
  m_apRows.clear();

  while (!m_list->children().empty()) {
    m_list->removeChild(m_list->children().front().get());
  }

  if (!networkAvailable()) {
    m_list->addChild(
        ui::label({
            .text = i18n::tr("control-center.network.unavailable-title"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
  } else {
    const float opacity = panelCardOpacity();

    if (!vpns.empty()) {
      auto vpnCard = ui::column({
          .configure = [scale, opacity](Flex& card) { applySectionCardStyle(card, scale, opacity); },
      });

      auto vpnHeader = makeCardHeaderRow(i18n::tr("control-center.network.vpns"), scale);
      vpnHeader->addChild(
          ui::toggle({
              .checkedImmediate = m_vpnVisible,
              .toggleSize = ToggleSize::Medium,
              .scale = scale,
              .onChange = [this, vpns](bool checked) {
                if (!checked && m_network != nullptr) {
                  for (const auto& vpn : vpns) {
                    if (vpn.active) {
                      m_network->deactivateVpnConnection(vpn);
                    }
                  }
                }
                m_vpnVisible = checked;
                PanelManager::instance().refresh();
              },
          })
      );
      vpnCard->addChild(std::move(vpnHeader));

      if (m_vpnVisible) {
        for (const auto& vpn : vpns) {
          auto row = std::make_unique<VpnConnectionRow>(
              scale, vpn,
              [this](const VpnConnectionInfo& clicked) {
                if (m_network != nullptr) {
                  m_network->activateVpnConnection(clicked);
                }
                PanelManager::instance().refresh();
              },
              [this](const VpnConnectionInfo& clicked) {
                if (m_network != nullptr) {
                  m_network->deactivateVpnConnection(clicked);
                }
                PanelManager::instance().refresh();
              }
          );
          vpnCard->addChild(std::move(row));
        }
      }

      m_list->addChild(std::move(vpnCard));
    }

    if (m_modem != nullptr && !m_modem->modems().empty()) {
      auto cellularCard = ui::column({
          .configure = [scale, opacity](Flex& card) { applySectionCardStyle(card, scale, opacity); },
      });

      auto cellularHeader = makeCardHeaderRow(i18n::tr("control-center.network.cellular"), scale);
      cellularHeader->addChild(
          ui::toggle({
              .out = &m_cellularToggle,
              .checkedImmediate = cellularToggleDisplayChecked(),
              .toggleSize = ToggleSize::Medium,
              .scale = scale,
              .onChange = [this](bool checked) { requestCellularEnabled(checked); },
          })
      );
      cellularCard->addChild(std::move(cellularHeader));

      for (const auto& modem : m_modem->modems()) {
        auto row = std::make_unique<CellularRow>(scale, modem);
        m_cellularRows.push_back(row.get());
        cellularCard->addChild(std::move(row));
      }

      m_list->addChild(std::move(cellularCard));
    }

    {
      auto wifiCard = ui::column({
          .configure = [scale, opacity](Flex& card) { applySectionCardStyle(card, scale, opacity); },
      });

      auto wifiHeader = makeCardHeaderRow(i18n::tr("control-center.network.wifi"), scale);
      wifiHeader->addChild(
          ui::spinner({
              .out = &m_scanSpinner,
              .color = colorSpecFromRole(ColorRole::Primary),
              .spinnerSize = Style::baseGlyphSize * scale,
              .visible = false,
          })
      );

      wifiHeader->addChild(
          ui::button({
              .out = &m_rescanButton,
              .glyph = "refresh",
              .glyphSize = Style::baseGlyphSize * scale,
              .variant = ButtonVariant::Ghost,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = [this]() {
                if (m_network != nullptr) {
                  m_network->requestScan();
                }
              },
          })
      );

      wifiHeader->addChild(
          ui::toggle({
              .out = &m_wifiToggle,
              .checkedImmediate = m_network->state().wirelessEnabled,
              .toggleSize = ToggleSize::Medium,
              .scale = scale,
              .onChange = [this](bool checked) { requestWirelessEnabled(checked); },
          })
      );
      wifiCard->addChild(std::move(wifiHeader));

      wifiCard->addChild(buildApRows());

      m_list->addChild(std::move(wifiCard));

      // Live state (spinner visibility/animation, toggle checked) is owned by
      // syncCurrentCard(), which runs every frame after the card is attached.
      // rebuildApList() builds structure only and must not drive animations.
    }
  }
  m_list->layout(renderer);
}

bool NetworkTab::syncApRows() {
  if (m_network == nullptr || m_apRows.empty()) {
    return false;
  }
  bool changed = false;
  for (const auto& ap : m_network->accessPoints()) {
    const auto it = m_apRows.find(ap.ssid);
    if (it != m_apRows.end() && it->second->syncLiveMetrics(ap)) {
      changed = true;
    }
  }
  return changed;
}

bool NetworkTab::syncCellularCard() {
  if (m_modem == nullptr) {
    return false;
  }
  if (m_cellularTogglePending) {
    // Bringing mobile data up walks Enabling -> Searching -> Registered ->
    // Connecting, so the requested position is held instead of snapping back
    // while the modem works. Give up once it lands or the request times out.
    const bool reached = cellularToggleChecked() == m_cellularToggleTarget;
    const bool timedOut = std::chrono::steady_clock::now() - m_cellularTogglePendingSince > kCellularPendingTimeout;
    if (reached || timedOut) {
      m_cellularTogglePending = false;
      m_cellularTogglePendingTimer.stop();
    }
  }
  const auto& modems = m_modem->modems();
  bool changed = false;
  const std::size_t count = std::min(m_cellularRows.size(), modems.size());
  for (std::size_t i = 0; i < count; ++i) {
    if (m_cellularRows[i] != nullptr && m_cellularRows[i]->syncLive(modems[i])) {
      changed = true;
    }
  }
  if (m_cellularToggle != nullptr) {
    m_cellularToggle->setChecked(cellularToggleDisplayChecked());
  }
  return changed;
}

bool NetworkTab::cellularToggleChecked() const {
  if (m_network != nullptr && m_network->canActivateCellularConnection()) {
    return m_network->state().cellularActive;
  }
  if (m_modem != nullptr) {
    return std::ranges::any_of(m_modem->modems(), [](const CellularModemInfo& modem) { return modem.enabled(); });
  }
  return false;
}

// The requested position while a toggle request is in flight, the observed one
// otherwise.
bool NetworkTab::cellularToggleDisplayChecked() const {
  return m_cellularTogglePending ? m_cellularToggleTarget : cellularToggleChecked();
}

void NetworkTab::requestCellularEnabled(bool enabled) {
  const bool nmCellular = m_network != nullptr && m_network->canActivateCellularConnection();
  if (enabled) {
    // GNOME parity: powering the modem is a prerequisite; the gsm connection is
    // what actually brings up mobile data.
    if (m_modem != nullptr) {
      m_modem->setAllModemsEnabled(true);
    }
    if (nmCellular) {
      m_network->activateCellularConnection();
    }
  } else {
    // Data off while the modem stays registered, like GNOME's mobile-data
    // switch. Without an NM gsm profile there is only modem power to cut.
    if (nmCellular) {
      m_network->deactivateCellularConnection();
    } else if (m_modem != nullptr) {
      m_modem->setAllModemsEnabled(false);
    }
  }
  m_cellularTogglePending = true;
  m_cellularToggleTarget = enabled;
  m_cellularTogglePendingSince = std::chrono::steady_clock::now();
  // Nothing else wakes the panel if the request never changes any state.
  m_cellularTogglePendingTimer.start(kCellularPendingTimeout + std::chrono::milliseconds(50), []() {
    PanelManager::instance().requestUpdateOnly();
    PanelManager::instance().requestRedraw();
  });
}

void NetworkTab::onPanelCardOpacityChanged(float opacity) {
  if (m_passwordInput != nullptr) {
    m_passwordInput->setSurfaceOpacity(opacity);
  }
}

std::unique_ptr<Flex> NetworkTab::buildEapRow(float scale) {
  return ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::select({
          .out = &m_eapSelect,
          .options = optionLabels(kEapOptions),
          .selectedIndex = std::size_t{0},
          .placeholder = i18n::tr("control-center.network.eap-method"),
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0F,
          .onSelectionChanged =
              [this](std::size_t index, std::string_view /*text*/) {
                // PEAP tunnels MSCHAPv2 only. Pinning and locking the inner method
                // beats letting the user assemble a pair that cannot authenticate
                // and only fails later, at association time.
                if (m_phase2Select == nullptr) {
                  return;
                }
                const bool pinsInnerMethod = optionAt(kEapOptions, index).method == network_enterprise::EapMethod::Peap;
                if (pinsInnerMethod) {
                  m_phase2Select->setSelectedIndexSilently(kPeapPhase2Index);
                }
                m_phase2Select->setEnabled(!pinsInnerMethod);
              },
      }),
      ui::select({
          .out = &m_phase2Select,
          .options = optionLabels(kPhase2Options),
          .selectedIndex = kPeapPhase2Index,
          .placeholder = i18n::tr("control-center.network.phase2-auth"),
          .enabled = false,
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0F,
      })
  );
}

// 802.1x fields, shown only for an enterprise network: EAP method, then identity, anonymous identity, domain suffix
// and CA certificate.
std::unique_ptr<Flex> NetworkTab::buildEnterpriseFields(float scale) {
  auto fields = ui::column({
      .out = &m_enterpriseFields,
      // Flex defaults to Center on the cross axis, which collapses each field to
      // its natural width. The card itself stretches via applySectionCardStyle();
      // this nested column has to say so for itself.
      .align = FlexAlign::Stretch,
      .visible = false,
      .configure = [scale](Flex& column) { column.setGap(Style::spaceSm * scale); },
  });
  fields->addChild(buildEapRow(scale));

  const auto submitFromForm = [this](const std::string& /*value*/) {
    submitPasswordPrompt(m_passwordInput != nullptr ? m_passwordInput->value() : std::string{});
  };
  struct TextField {
    Input** out;
    const char* placeholderKey;
  };
  const std::array<TextField, 4> textFields{{
      {&m_identityInput, "control-center.network.identity"},
      {&m_anonymousIdentityInput, "control-center.network.anonymous-identity"},
      {&m_domainMatchInput, "control-center.network.domain-suffix-match"},
      {&m_caCertInput, "control-center.network.ca-certificate"},
  }};
  for (const TextField& field : textFields) {
    fields->addChild(
        ui::input({
            .out = field.out,
            .placeholder = i18n::tr(field.placeholderKey),
            .surfaceOpacity = panelCardOpacity(),
            .surfaceRole = ColorRole::Surface,
            .onSubmit = submitFromForm,
        })
    );
  }
  return fields;
}

// Password field with its reveal toggle, then Connect and Cancel.
std::unique_ptr<Flex> NetworkTab::buildPasswordRow(float scale) {
  return ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::input({
          .out = &m_passwordInput,
          .placeholder = i18n::tr("control-center.network.password"),
          .passwordMode = true,
          .surfaceOpacity = panelCardOpacity(),
          .surfaceRole = ColorRole::Surface,
          .flexGrow = 1.0F,
          .onSubmit = [this](const std::string& value) { submitPasswordPrompt(value); },
      }),
      ui::button({
          .out = &m_passwordRevealButton,
          .glyph = "eye",
          .glyphSize = Style::baseGlyphSize * scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick =
              [this]() {
                if (m_passwordInput == nullptr) {
                  return;
                }
                const bool revealed = !m_passwordInput->passwordRevealed();
                m_passwordInput->setPasswordRevealed(revealed);
                if (m_passwordRevealButton != nullptr) {
                  m_passwordRevealButton->setGlyph(revealed ? "eye-off" : "eye");
                }
              },
      }),
      ui::button({
          .text = i18n::tr("control-center.network.connect"),
          .variant = ButtonVariant::Default,
          .onClick =
              [this]() { submitPasswordPrompt(m_passwordInput != nullptr ? m_passwordInput->value() : std::string{}); },
      }),
      ui::button({
          .text = i18n::tr("common.actions.cancel"),
          .variant = ButtonVariant::Ghost,
          .onClick = [this]() { cancelPasswordPrompt(); },
      })
  );
}

std::unique_ptr<Flex> NetworkTab::buildPasswordCard(float scale) {
  auto passwordCard = ui::column({
      .out = &m_passwordCard,
      .visible = false,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) {
        applySectionCardStyle(card, scale, opacity);
        card.setGap(Style::spaceMd * scale);
      },
  });
  passwordCard->addChild(
      ui::label({
          .out = &m_passwordTitle,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  passwordCard->addChild(
      ui::label({
          .out = &m_credentialError,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Error),
          .maxLines = 4,
          .visible = false,
      })
  );
  passwordCard->addChild(buildEnterpriseFields(scale));
  passwordCard->addChild(buildPasswordRow(scale));
  return passwordCard;
}

// ---- Quick (QuickPanel) mode ------------------------------------------------------------------------
//
// One selectable list across the sections (the ip line, saved connections, wi-fi, tailscale): Up/Down select, Enter
// runs the row's main action, `a` opens its action menu, `r` rescans. The body is derived from a QuickModel; the
// rendered rows and the panel height both come from it, so they cannot drift apart.

namespace {

  constexpr float kQuickRow = term_frame::kRowHeight;
  constexpr float kQuickHeroHeight = 30.0F;
  constexpr float kQuickGraphHeight = 44.0F;
  constexpr float kQuickPasswordHeight = 26.0F;
  constexpr float kQuickGap = Style::spaceSm;
  // Border + padding of a term_frame box, both sides.
  constexpr float kQuickBoxInset = 2.0F * (Style::spaceSm + Style::borderWidth);
  constexpr float kMono = term_frame::kBtopFontSize;
  // Advance of one monospace cell as a fraction of the font size.
  constexpr float kGlyphWidthEm = 0.6F;
  // The two-cell marker column every selectable row reserves.
  constexpr float kGutterPx = 2.0F * kMono * kGlyphWidthEm;
  constexpr float kAxisWidth = 4.0F * Style::fontSizeMini * kGlyphWidthEm + 2.0F;
  constexpr float kMinMenuWidthPx = 80.0F;
  constexpr float kMenuTrailPx = 4.0F;   // right-hand slack when wrapping menu items
  constexpr float kMenuItemPadPx = 6.0F; // per menu item, before the inter-item gap
  constexpr int kSignalStepPercent = 5;
  constexpr std::size_t kExitNameChars = 12; // exit-node label width in the menu
  constexpr int kTrafficSamples = 60;
  constexpr std::size_t kConnCap = 6;
  constexpr std::size_t kWifiCap = 7;
  constexpr std::size_t kTailscaleCap = 8;
  constexpr std::size_t kMaxExitNodes = 9;
  constexpr std::chrono::seconds kFailureTtl{10};
  constexpr std::chrono::seconds kInfoTtl{4};
  constexpr std::chrono::seconds kBusyTtl{60};
  constexpr std::chrono::seconds kArmTtl{4};
  constexpr std::chrono::seconds kProfilesEvery{3};
  constexpr std::chrono::seconds kTailscaleEvery{5};
  constexpr std::chrono::seconds kPingEvery{5};
  constexpr std::chrono::seconds kAttemptTimeout{25};
  constexpr std::chrono::seconds kExternalIpTtl{8};
  constexpr std::chrono::seconds kProfileUpTimeout{45}; // activation may wait for DHCP or a VPN handshake
  constexpr std::chrono::seconds kProfileDownTimeout{20};
  constexpr std::chrono::seconds kProfileEditTimeout{8};
  constexpr std::chrono::seconds kProfileDeleteTimeout{10};
  constexpr std::chrono::seconds kTailscaleExitTimeout{10};
  constexpr std::chrono::seconds kPingTimeout{3};
  constexpr const char* kInternetProbeHost = "1.1.1.1";
  constexpr std::chrono::seconds kAttemptGrace{8};
  constexpr double kSlowInternetMs = 150.0;
  constexpr std::array<std::string_view, 5> kNumerals{"¹", "²", "³", "⁴", "⁵"};

  using term_frame::escapeMarkup;
  using term_frame::setShown;

  std::string colorSpan(ColorRole role, std::string_view text) {
    return "<span foreground=\"" + term_frame::paletteHex(role) + "\">" + escapeMarkup(text) + "</span>";
  }

  std::string dimSpan(std::string_view text) { return colorSpan(ColorRole::OnSurfaceVariant, text); }

  std::string readWord(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string word;
    in >> word;
    return word;
  }

  std::string linkSpeedText(const std::string& iface) {
    if (iface.empty()) {
      return {};
    }
    const std::string raw = readWord("/sys/class/net/" + iface + "/speed");
    if (raw.empty()) {
      return {};
    }
    char* end = nullptr;
    const long mbps = std::strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str() || mbps <= 0) {
      return {};
    }
    if (mbps >= 1000) {
      return mbps % 1000 == 0 ? std::format("{} Gbps", mbps / 1000)
                              : std::format("{:.1f} Gbps", static_cast<double>(mbps) / 1000.0);
    }
    return std::format("{} Mbps", mbps);
  }

  std::string defaultGateway(const std::string& iface) {
    std::ifstream in("/proc/net/route");
    std::string line;
    std::getline(in, line); // header
    while (std::getline(in, line)) {
      std::istringstream fields(line);
      std::string name;
      std::string destination;
      std::string gateway;
      fields >> name >> destination >> gateway;
      if (destination != "00000000" || (!iface.empty() && name != iface)) {
        continue;
      }
      const unsigned long value = std::strtoul(gateway.c_str(), nullptr, 16);
      if (value == 0UL) {
        continue;
      }
      in_addr addr{};
      addr.s_addr = static_cast<in_addr_t>(value);
      std::array<char, INET_ADDRSTRLEN> buffer{};
      if (inet_ntop(AF_INET, &addr, buffer.data(), buffer.size()) != nullptr) {
        return buffer.data();
      }
    }
    return {};
  }

  struct IfaceAddress {
    std::string ip;
    int prefix = 0;
  };

  IfaceAddress ifaceAddress(const std::string& iface) {
    IfaceAddress result;
    ifaddrs* list = nullptr;
    if (iface.empty() || getifaddrs(&list) != 0) {
      return result;
    }
    for (const ifaddrs* p = list; p != nullptr; p = p->ifa_next) {
      if (p->ifa_addr == nullptr || p->ifa_addr->sa_family != AF_INET || iface != p->ifa_name) {
        continue;
      }
      const auto* addr = reinterpret_cast<const sockaddr_in*>(p->ifa_addr);
      std::array<char, INET_ADDRSTRLEN> buffer{};
      if (inet_ntop(AF_INET, &addr->sin_addr, buffer.data(), buffer.size()) == nullptr) {
        continue;
      }
      result.ip = buffer.data();
      if (p->ifa_netmask != nullptr) {
        const auto* mask = reinterpret_cast<const sockaddr_in*>(p->ifa_netmask);
        result.prefix = std::popcount(ntohl(mask->sin_addr.s_addr));
      }
      break;
    }
    freeifaddrs(list);
    return result;
  }

  bool machineHasWifiDevice() {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/net", ec)) {
      if (std::filesystem::exists(entry.path() / "wireless", ec)) {
        return true;
      }
    }
    return false;
  }

  std::string rateText(double bytesPerSec) {
    if (bytesPerSec >= 1e9) {
      return std::format("{:.1f} G", bytesPerSec / 1e9);
    }
    if (bytesPerSec >= 1e5 || bytesPerSec == 0.0) {
      return std::format("{:.1f} M", bytesPerSec / 1e6);
    }
    if (bytesPerSec >= 1e4) {
      return std::format("{:.0f} K", bytesPerSec / 1e3);
    }
    return std::format("{:.1f} K", bytesPerSec / 1e3);
  }

  std::pair<double, double> throughputOf(const SystemStats& stats, const std::string& iface) {
    if (!iface.empty()) {
      if (const auto it = stats.netThroughputByInterface.find(iface); it != stats.netThroughputByInterface.end()) {
        return {it->second.rxBytesPerSec, it->second.txBytesPerSec};
      }
    }
    return {stats.netRxBytesPerSec, stats.netTxBytesPerSec};
  }

  // A term_frame box whose title row is pinned to one grid row, so the height arithmetic holds.
  std::unique_ptr<Flex>
  quickBox(std::string_view numeral, std::string_view title, float scale, Label** titleOut, Label** chipOut = nullptr) {
    // Built here rather than via term_frame::box(): a bordered Flex keeps its own background node among its
    // children, so the title row is not reliably children().front().
    auto box = term_frame::frame(scale, Style::spaceSm * scale);
    auto titleRow = term_frame::titleRow(numeral, title, scale, chipOut);
    titleRow->setMinHeight(kQuickRow * scale);
    if (titleOut != nullptr) {
      *titleOut = nullptr;
      for (const auto& child : titleRow->children()) {
        if (auto* label = dynamic_cast<Label*>(child.get()); label != nullptr) {
          *titleOut = label;
          break;
        }
      }
    }
    box->addChild(std::move(titleRow));
    return box;
  }

  std::string probeMarkup(const netq::Probe& probe, bool gateway) {
    if (!probe.known) {
      return dimSpan("…");
    }
    if (!probe.ms.has_value()) {
      return colorSpan(gateway ? ColorRole::Error : ColorRole::Warning, "timeout");
    }
    const double ms = std::max(1.0, std::round(*probe.ms));
    const std::string text = std::format("{:.0f} ms", ms);
    if (!gateway && *probe.ms > kSlowInternetMs) {
      return colorSpan(ColorRole::Warning, text);
    }
    return escapeMarkup(text);
  }

} // namespace

// ---- model ----
// Named (not anonymous) so network_tab.h can forward-declare the row types its builders return.

namespace netpanel {

  struct MenuItem {
    std::string id;
    std::string word;
    std::size_t hot = 0;
    std::uint32_t key = 0;
    bool destructive = false;
  };

  struct QRow {
    std::string id;
    std::string name;
    bool dimName = false;
    std::string badge; // accent filled block when `lit`, dim text otherwise; empty hides it
    bool lit = false;
    std::string detail; // markup
    std::vector<MenuItem> menu;
  };

  enum class ElemKind : std::uint8_t { Row, Menu, Password, Text };

  struct QElem {
    ElemKind kind = ElemKind::Text;
    QRow row;
    std::vector<std::vector<MenuItem>> lines;
    std::string text;
  };

  struct QSection {
    std::string key;
    std::string title;
    std::string chip;
    std::vector<QElem> elems;
  };

} // namespace netpanel

using namespace netpanel;

namespace {

  MenuItem makeItem(std::string id, std::string word, std::size_t hot, bool destructive = false) {
    MenuItem item;
    item.hot = std::min(hot, word.empty() ? std::size_t{0} : word.size() - 1);
    item.key = word.empty() ? 0U : static_cast<std::uint32_t>(std::tolower(static_cast<unsigned char>(word[item.hot])));
    item.id = std::move(id);
    item.word = std::move(word);
    item.destructive = destructive;
    return item;
  }

  float elemHeight(const QElem& elem) {
    switch (elem.kind) {
    case ElemKind::Menu:
      return static_cast<float>(elem.lines.size()) * kQuickRow;
    case ElemKind::Password:
      return kQuickPasswordHeight;
    case ElemKind::Row:
    case ElemKind::Text:
      break;
    }
    return kQuickRow;
  }

  float elemsHeight(const QSection& section) {
    float sum = 0.0F;
    for (const auto& elem : section.elems) {
      sum += elemHeight(elem);
    }
    return sum;
  }

  // A bordered section: inset + title row + children, one gap between each pair.
  float boxHeight(const QSection& section) {
    if (section.elems.empty()) {
      return 0.0F;
    }
    return kQuickBoxInset
        + kQuickRow
        + elemsHeight(section)
        + static_cast<float>(section.elems.size()) * Style::spaceXs;
  }

  std::vector<std::vector<MenuItem>> wrapMenu(const std::vector<MenuItem>& items, float usablePx, bool armedAny) {
    std::vector<std::vector<MenuItem>> lines;
    float used = 0.0F;
    for (const auto& item : items) {
      const std::size_t chars =
          item.destructive && armedAny ? std::max<std::size_t>(item.word.size(), 8) : item.word.size();
      const float px = static_cast<float>(chars) * kMono * kGlyphWidthEm + kMenuItemPadPx + Style::spaceSm;
      if (lines.empty() || (used + px > usablePx && !lines.back().empty())) {
        lines.emplace_back();
        used = 0.0F;
      }
      lines.back().push_back(item);
      used += px;
    }
    return lines;
  }

  std::string clip(std::string_view text, std::size_t chars) {
    if (text.size() <= chars) {
      return std::string(text);
    }
    std::size_t cut = chars;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) {
      --cut;
    }
    return std::string(text.substr(0, cut)) + "…";
  }

  // First visible row of a section that shows `cap` of `rows`: the previous window moved just far enough to keep the
  // credential row (else the selection) in view.
  std::size_t visibleWindowStart(
      const std::vector<QRow>& rows, std::size_t cap, std::size_t previousStart, std::string_view selectedId,
      std::string_view passwordRowId
  ) {
    std::size_t focus = rows.size();
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (!passwordRowId.empty() && rows[i].id == passwordRowId) {
        focus = i;
        break;
      }
      if (rows[i].id == selectedId) {
        focus = i;
      }
    }
    std::size_t start = previousStart;
    if (focus < rows.size()) {
      if (focus < start) {
        start = focus;
      } else if (focus >= start + cap) {
        start = focus + 1 - cap;
      }
    }
    return std::min(start, rows.size() - cap);
  }

  // Total panel height: hero, then every non-empty section and the traffic box, one gap between each pair.
  float stackHeight(const std::array<const QSection*, 4>& sections, bool traffic) {
    const QSection& ip = *sections[0];
    std::size_t blocks = 1;
    float total = kQuickHeroHeight + kQuickRow;
    if (!ip.elems.empty()) {
      total += elemsHeight(ip);
      ++blocks;
    }
    for (std::size_t i = 1; i < sections.size(); ++i) {
      if (!sections[i]->elems.empty()) {
        total += boxHeight(*sections[i]);
        ++blocks;
      }
    }
    if (traffic) {
      total += kQuickBoxInset + 2.0F * kQuickRow + kQuickGraphHeight + 2.0F * Style::spaceXs;
      ++blocks;
    }
    return total + kQuickGap * static_cast<float>(blocks - 1);
  }

  // Identity of everything the body draws; the rows are rebuilt only when it changes.
  std::string bodyKey(
      const std::array<const QSection*, 4>& sections, float width, const std::string& selectedId,
      const std::string& armed
  ) {
    std::string key = std::format("{}|{}|", static_cast<int>(width), selectedId);
    for (const QSection* section : sections) {
      key += section->title + "/" + section->chip + "{";
      for (const auto& elem : section->elems) {
        key += std::format(
            "{}:{}:{}:{}:{}:{}:", static_cast<int>(elem.kind), elem.row.id, elem.row.name, elem.row.badge,
            elem.row.lit ? 1 : 0, elem.row.detail
        );
        key += elem.text;
        for (const auto& line : elem.lines) {
          for (const auto& item : line) {
            key += item.word;
            key += (item.destructive && armed == elem.row.id + "|" + item.id) ? '!' : '.';
          }
          key += '\n';
        }
        key += ';';
      }
      key += "}";
    }
    return key;
  }

} // namespace

struct NetworkTab::QuickModel {
  QSection ip;
  QSection conn;
  QSection wifi;
  QSection tailscale;
  std::vector<std::string> selectables;
  std::vector<MenuKey> menuKeys;
  std::string sel;
  std::string passwordRow; // row id that shows the credential field; empty when none
  bool passwordPlaced = false;
  bool selHasMenu = false;
  bool traffic = false;
  float height = 0.0F;
  std::string key;
};

NetworkTab::QuickSnapshot NetworkTab::readQuickSnapshot() const {
  QuickSnapshot snap;
  snap.hasWifiDevice = machineHasWifiDevice();
  if (!networkAvailable()) {
    return snap;
  }
  const NetworkState& s = m_network->state();
  snap.connected = s.connected;
  if (!s.connected) {
    return snap;
  }
  snap.iface = s.interfaceName;
  switch (s.kind) {
  case NetworkConnectivity::Wired:
    snap.medium = "wired";
    snap.speed = linkSpeedText(snap.iface);
    break;
  case NetworkConnectivity::Wireless:
    snap.medium = "wi-fi";
    snap.ssid = s.ssid;
    snap.signal = static_cast<int>(s.signalStrength);
    if (const char* band = network_display::wifiFrequencyBandLabel(s.frequencyMhz); band != nullptr) {
      snap.band = band;
    }
    break;
  case NetworkConnectivity::Cellular:
    snap.medium = "cellular";
    break;
  default:
    snap.medium = "link";
    break;
  }
  if (const IfaceAddress addr = ifaceAddress(snap.iface); !addr.ip.empty()) {
    snap.ipPlain = addr.ip;
    snap.ip = addr.prefix > 0 ? std::format("{}/{}", addr.ip, addr.prefix) : addr.ip;
  } else if (!s.ipv4.empty()) {
    snap.ipPlain = s.ipv4;
    snap.ip = s.ipv4;
  }
  snap.gateway = defaultGateway(snap.iface);
  return snap;
}

std::string NetworkTab::quickPasswordRowId() const {
  return (m_hasPendingSecret && !m_pendingSsid.empty()) ? "w:" + m_pendingSsid : std::string{};
}

// A live note replaces a row's detail; otherwise `fallback`.
std::string NetworkTab::quickDetailOr(
    const std::string& id, std::string fallback, std::chrono::steady_clock::time_point now
) const {
  const auto it = m_qNotes.find(id);
  if (it == m_qNotes.end() || it->second.until <= now) {
    return fallback;
  }
  const Note& note = it->second;
  switch (note.tone) {
  case NoteTone::Accent:
    return colorSpan(ColorRole::Primary, note.text);
  case NoteTone::Warning:
    return colorSpan(ColorRole::Warning, note.text);
  case NoteTone::Error:
    return colorSpan(ColorRole::Error, note.text);
  case NoteTone::Dim:
    break;
  }
  return dimSpan(note.text);
}

std::vector<QRow> NetworkTab::quickIpRows(const QuickSnapshot& snap, std::chrono::steady_clock::time_point now) const {
  std::vector<QRow> rows;
  if (!snap.connected || snap.ipPlain.empty()) {
    return rows;
  }
  QRow row;
  row.id = "ip";
  row.name = snap.ip;
  if (!snap.gateway.empty()) {
    row.name += " · gw " + snap.gateway;
  }
  row.dimName = true;
  row.detail = quickDetailOr(row.id, {}, now);
  row.menu = {makeItem("copy", "copy ip", 0), makeItem("ext", "ext ip", 0)};
  rows.push_back(std::move(row));
  return rows;
}

std::vector<QRow> NetworkTab::quickConnectionRows(std::chrono::steady_clock::time_point now) const {
  std::vector<QRow> rows;
  rows.reserve(m_qProfiles.size());
  for (const auto& profile : m_qProfiles) {
    QRow row;
    row.id = "c:" + profile.uuid;
    row.name = profile.name;
    row.lit = profile.active;
    row.badge = profile.active ? "active" : "";
    row.detail = quickDetailOr(
        row.id, profile.activating ? dimSpan("connecting…") : dimSpan(netq::kindLabel(profile.kind)), now
    );
    if (profile.active || profile.activating) {
      row.menu.push_back(makeItem("disconnect", "disconnect", 0));
    } else {
      row.menu.push_back(makeItem("connect", "connect", 0));
    }
    if (profile.active) {
      row.menu.push_back(makeItem("copyip", "copy ip", 5));
    }
    row.menu.push_back(makeItem("auto", profile.autoconnect ? "autoconnect [on]" : "autoconnect off", 2));
    row.menu.push_back(makeItem("forget", "forget", 0, true));
    row.menu.push_back(makeItem("edit", "edit", 0));
    rows.push_back(std::move(row));
  }
  return rows;
}

// The radio row, then one row per distinct SSID. `emptyText` is set when there is nothing to list.
std::vector<QRow> NetworkTab::quickWifiRows(std::chrono::steady_clock::time_point now, std::string& emptyText) const {
  std::vector<QRow> rows;
  if (!networkAvailable()) {
    emptyText = "network service unavailable";
    return rows;
  }
  const NetworkState& state = m_network->state();
  const bool radioShown = m_wifiTogglePending ? m_wifiToggleTarget : state.wirelessEnabled;
  QRow radio;
  radio.id = "w:radio";
  radio.name = "wifi radio";
  radio.lit = radioShown;
  radio.badge = radioShown ? "on" : "off";
  radio.detail = quickDetailOr(
      radio.id, m_wifiTogglePending ? dimSpan("switching…") : (state.scanning ? dimSpan("scanning") : std::string{}),
      now
  );
  if (state.wirelessEnabled && !m_wifiTogglePending) {
    radio.menu.push_back(makeItem("off", "turn off", 5, true));
  }
  rows.push_back(std::move(radio));
  if (!state.wirelessEnabled) {
    return rows;
  }

  std::vector<std::string> seen;
  for (const auto& ap : sortedAccessPoints(m_network->accessPoints())) {
    if (ap.ssid.empty() || std::ranges::find(seen, ap.ssid) != seen.end()) {
      continue;
    }
    seen.push_back(ap.ssid);
    const bool saved = m_network->hasSavedConnection(ap.ssid);
    QRow row;
    row.id = "w:" + ap.ssid;
    row.name = ap.ssid;
    row.lit = ap.active;
    row.badge = ap.active ? "active" : "";
    // Signal in coarse steps, so a scan jitter does not rebuild the rows.
    const int percent =
        ((static_cast<int>(ap.strength) + kSignalStepPercent / 2) / kSignalStepPercent) * kSignalStepPercent;
    std::string signal;
    if (saved && !ap.active) {
      signal += dimSpan("saved") + " ";
    }
    if (ap.secured) {
      signal += "\uf023 ";
    }
    signal += term_frame::dottedMeterMarkup(5, static_cast<double>(percent) / 100.0, true);
    signal += std::format(" {:>3}%", percent);
    row.detail = quickDetailOr(row.id, signal, now);
    if (saved || ap.active) {
      row.menu.push_back(makeItem("forget", "forget", 0, true));
    }
    rows.push_back(std::move(row));
  }
  if (seen.empty()) {
    emptyText = "no networks found";
  }
  return rows;
}

std::vector<QRow> NetworkTab::quickTailscaleRows(std::chrono::steady_clock::time_point now) const {
  std::vector<QRow> rows;
  if (!m_qHasTailscale) {
    return rows;
  }
  QRow toggle;
  toggle.id = "t:toggle";
  toggle.name = "tailscale";
  toggle.lit = m_qTs.running;
  toggle.badge = m_qTs.running ? "on" : "off";
  std::string state;
  if (!m_qTs.loaded) {
    state = dimSpan("…");
  } else if (!m_qTs.problem.empty()) {
    state = colorSpan(ColorRole::Warning, m_qTs.problem);
  }
  toggle.detail = quickDetailOr(toggle.id, state, now);
  rows.push_back(std::move(toggle));
  if (!m_qTs.running) {
    return rows;
  }

  QRow exit;
  exit.id = "t:exit";
  exit.name = "exit node";
  exit.detail = quickDetailOr(
      exit.id, m_qTs.exitNode.empty() ? dimSpan("none") : colorSpan(ColorRole::Primary, m_qTs.exitNode), now
  );
  std::size_t number = 0;
  for (const auto& peer : m_qTs.peers) {
    if (!peer.exitOption || number >= kMaxExitNodes) {
      continue;
    }
    exit.menu.push_back(
        makeItem("x" + std::to_string(number), std::format("{} {}", number + 1, clip(peer.name, kExitNameChars)), 0)
    );
    ++number;
  }
  exit.menu.push_back(makeItem("none", "none", 0));
  rows.push_back(std::move(exit));
  for (const auto& peer : m_qTs.peers) {
    QRow row;
    row.id = "p:" + peer.name;
    row.name = peer.name;
    const std::string dot = peer.online ? colorSpan(ColorRole::Primary, "●") : dimSpan("○");
    const std::string ip = peer.online ? escapeMarkup(peer.ip) : dimSpan(netq::lastSeenText(peer.lastSeen));
    row.detail = quickDetailOr(row.id, dot + " " + ip, now);
    if (!peer.ip.empty()) {
      row.menu.push_back(makeItem("copy", "copy ip", 0));
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

// Rows of one section into elements: a window around the selection, the open menu and the credential field
// directly under their rows.
void NetworkTab::placeQuickRows(
    QuickModel& model, QSection& section, const std::vector<QRow>& rows, std::size_t cap, float usablePx
) const {
  const bool windowed = rows.size() > cap;
  std::size_t start = 0;
  if (windowed) {
    start = visibleWindowStart(rows, cap, m_qWinStart[section.key], model.sel, model.passwordRow);
    m_qWinStart[section.key] = start;
  }
  const std::size_t end = std::min(rows.size(), start + cap);
  if (windowed && start > 0) {
    QElem more;
    more.text = std::format("  … {} more above", start);
    section.elems.push_back(std::move(more));
  }
  const bool armedAny = !m_qArmed.empty();
  for (std::size_t i = start; i < end; ++i) {
    const QRow& row = rows[i];
    QElem elem;
    elem.kind = ElemKind::Row;
    elem.row = row;
    section.elems.push_back(std::move(elem));
    if (row.id == model.sel && !row.menu.empty()) {
      model.selHasMenu = true;
      if (row.id == m_qMenuId) {
        QElem menu;
        menu.kind = ElemKind::Menu;
        menu.row.id = row.id;
        menu.lines = wrapMenu(row.menu, usablePx, armedAny);
        section.elems.push_back(std::move(menu));
        for (const auto& item : row.menu) {
          model.menuKeys.push_back(MenuKey{.key = item.key, .id = item.id, .destructive = item.destructive});
        }
      }
    }
    if (!model.passwordRow.empty() && row.id == model.passwordRow) {
      QElem password;
      password.kind = ElemKind::Password;
      password.row.id = row.id;
      section.elems.push_back(std::move(password));
      model.passwordPlaced = true;
    }
  }
  if (windowed && end < rows.size()) {
    QElem more;
    more.text = std::format("  … {} more below", rows.size() - end);
    section.elems.push_back(std::move(more));
  }
}

NetworkTab::QuickModel NetworkTab::buildQuickModel(float width, const QuickSnapshot& snap) const {
  QuickModel model;
  const auto now = std::chrono::steady_clock::now();
  const float usablePx = std::max(kMinMenuWidthPx, width - kQuickBoxInset - kGutterPx - Style::spaceSm - kMenuTrailPx);
  const bool available = networkAvailable();
  const bool wifiSection = snap.hasWifiDevice || m_hasPendingSecret;

  const std::vector<QRow> ipRows = quickIpRows(snap, now);
  const std::vector<QRow> connRows = quickConnectionRows(now);
  std::string wifiText;
  const std::vector<QRow> wifiRows = wifiSection ? quickWifiRows(now, wifiText) : std::vector<QRow>{};
  const std::vector<QRow> tsRows = quickTailscaleRows(now);

  for (const auto* rows : {&ipRows, &connRows, &wifiRows, &tsRows}) {
    for (const auto& row : *rows) {
      model.selectables.push_back(row.id);
    }
  }
  model.sel = m_qSel;
  if (std::ranges::find(model.selectables, model.sel) == model.selectables.end()) {
    model.sel = model.selectables.empty() ? std::string{} : model.selectables.front();
  }
  model.passwordRow = quickPasswordRowId();

  model.ip.key = "ip";
  placeQuickRows(model, model.ip, ipRows, 1, usablePx);
  model.conn.key = "conn";
  model.conn.title = "connections";
  placeQuickRows(model, model.conn, connRows, kConnCap, usablePx);

  model.wifi.key = "wifi";
  model.wifi.title = "wifi";
  if (wifiSection) {
    if (!wifiText.empty() && wifiRows.empty()) {
      QElem text;
      text.text = "  " + wifiText;
      model.wifi.elems.push_back(std::move(text));
    } else {
      placeQuickRows(model, model.wifi, wifiRows, kWifiCap, usablePx);
      if (!wifiText.empty()) {
        QElem text;
        text.text = "  " + wifiText + "   r to rescan";
        model.wifi.elems.push_back(std::move(text));
      }
    }
    model.wifi.chip = (available && m_network->state().scanning) ? "scanning" : "";
  }

  model.tailscale.key = "tailscale";
  model.tailscale.title = "tailscale";
  placeQuickRows(model, model.tailscale, tsRows, kTailscaleCap, usablePx);

  // A credential request whose network is not on the list (a saved profile asking for its secret).
  if (m_hasPendingSecret && !model.passwordPlaced) {
    QElem text;
    text.text = "  password for " + (m_pendingSsid.empty() ? std::string("network") : m_pendingSsid);
    QElem password;
    password.kind = ElemKind::Password;
    model.conn.elems.insert(model.conn.elems.begin(), {std::move(text), std::move(password)});
  }

  model.traffic = m_monitor != nullptr && m_monitor->isRunning();
  const std::array<const QSection*, 4> sections{&model.ip, &model.conn, &model.wifi, &model.tailscale};
  model.height = stackHeight(sections, model.traffic);
  model.key = bodyKey(sections, width, model.sel, m_qArmed);
  return model;
}

float NetworkTab::quickPanelHeight(float width, float fallback) const {
  if (!m_quick) {
    return fallback;
  }
  return buildQuickModel(width, readQuickSnapshot()).height;
}

std::string NetworkTab::quickPanelHints() const {
  if (!m_quick) {
    return {};
  }
  if (m_hasPendingSecret && quickTextInputFocused()) {
    return "⏎ connect  ↑↓ cancel";
  }
  const std::string_view sel = m_qSel;
  std::string main;
  if (sel == "ip") {
    main = "copy";
  } else if (sel.starts_with("c:")) {
    main = "switch";
  } else if (sel == "w:radio") {
    main = (networkAvailable() && m_network->state().wirelessEnabled) ? "rescan" : "enable";
  } else if (sel.starts_with("w:")) {
    main = "connect";
  } else if (sel == "t:toggle") {
    main = "toggle";
  } else if (sel == "t:exit") {
    main = "pick";
  } else if (sel.starts_with("p:")) {
    main = "copy ip";
  }
  std::string hints = "↑↓ select";
  if (!main.empty()) {
    hints += "  ⏎ " + main;
  }
  if (m_qSelHasMenu) {
    hints += "  a actions";
  }
  hints += "  r scan";
  return hints;
}

std::string NetworkTab::quickPanelStatus() const {
  if (!networkAvailable()) {
    return "down";
  }
  const NetworkState& s = m_network->state();
  if (s.connected) {
    return "up";
  }
  return s.resolving ? "connecting" : "down";
}

bool NetworkTab::quickTextInputFocused() const {
  const InputDispatcher* dispatcher = inputDispatcher();
  if (dispatcher == nullptr) {
    return false;
  }
  const InputArea* area = dispatcher->focusedArea();
  return area != nullptr && area->textInputClient() != nullptr;
}

bool NetworkTab::handleQuickPanelKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!m_quick || preedit) {
    return false;
  }
  if (!pressed) {
    // A release frees the key, so a held key acts once (a repeat must not be the confirming second press).
    if (m_qHeldKey != 0U && (sym == m_qHeldKey || (sym | 0x20U) == m_qHeldKey)) {
      m_qHeldKey = 0;
    }
    return false;
  }
  if (modifiers != 0U) {
    return false;
  }
  // A credential field owns the printable keys; Up/Down leave it and drop the request.
  if (quickTextInputFocused()) {
    if (sym == XKB_KEY_Up || sym == XKB_KEY_Down) {
      cancelPasswordPrompt();
      quickMoveSelection(sym == XKB_KEY_Up ? -1 : 1);
      return true;
    }
    return false;
  }
  const auto once = [this](std::uint32_t key) {
    if (m_qHeldKey == key) {
      return false;
    }
    m_qHeldKey = key;
    return true;
  };

  // An open menu takes its own letters first.
  if (!m_qMenuId.empty() && m_qMenuId == m_qSel && sym != XKB_KEY_a) {
    const std::uint32_t lower = (sym >= 'A' && sym <= 'Z') ? sym | 0x20U : sym;
    for (const auto& item : m_qMenuKeys) {
      if (item.key != 0U && item.key == lower) {
        if (once(sym)) {
          quickRunItem(m_qSel, item.id, item.destructive);
        }
        return true;
      }
    }
  }

  switch (sym) {
  case XKB_KEY_Up:
  case XKB_KEY_k:
    quickMoveSelection(-1);
    return true;
  case XKB_KEY_Down:
  case XKB_KEY_j:
    quickMoveSelection(1);
    return true;
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
    if (once(sym)) {
      quickEnter();
    }
    return true;
  case XKB_KEY_a:
    if (once(sym)) {
      quickToggleMenu();
    }
    return true;
  case XKB_KEY_r:
    if (once(sym)) {
      if (m_network != nullptr) {
        m_network->requestScan();
      }
      quickRefreshProfiles();
      quickRefreshTailscale();
      m_qPingAt = {};
      quickTick();
      quickRequestUpdate();
    }
    return true;
  default:
    return false;
  }
}

void NetworkTab::quickRequestUpdate() const {
  PanelManager::instance().requestUpdateOnly();
  PanelManager::instance().requestRedraw();
}

void NetworkTab::quickSetNote(
    const std::string& rowId, std::string text, NoteTone tone, std::chrono::milliseconds ttl
) {
  Note note;
  note.text = std::move(text);
  note.tone = tone;
  note.until = std::chrono::steady_clock::now() + ttl;
  m_qNotes[rowId] = std::move(note);
  quickRequestUpdate();
}

void NetworkTab::quickSetBusy(const std::string& rowId, std::string text) {
  quickSetNote(rowId, std::move(text), NoteTone::Dim, kBusyTtl);
  m_qNotes[rowId].busy = true;
}

void NetworkTab::quickClearNote(const std::string& rowId) {
  if (m_qNotes.erase(rowId) > 0U) {
    quickRequestUpdate();
  }
}

void NetworkTab::quickPruneNotes() {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = m_qNotes.begin(); it != m_qNotes.end();) {
    it = it->second.until <= now ? m_qNotes.erase(it) : std::next(it);
  }
  if (!m_qArmed.empty() && now >= m_qArmedUntil) {
    m_qArmed.clear();
  }
}

void NetworkTab::quickDisarm() {
  m_qArmed.clear();
  m_qArmTimer.stop();
}

void NetworkTab::quickMoveSelection(int delta) {
  if (m_qSelectables.empty()) {
    return;
  }
  const auto it = std::ranges::find(m_qSelectables, m_qSel);
  std::ptrdiff_t index = it == m_qSelectables.end() ? 0 : std::distance(m_qSelectables.begin(), it);
  index = std::clamp<std::ptrdiff_t>(index + delta, 0, static_cast<std::ptrdiff_t>(m_qSelectables.size()) - 1);
  m_qSel = m_qSelectables[static_cast<std::size_t>(index)];
  m_qMenuId.clear();
  quickDisarm();
  quickRequestUpdate();
}

void NetworkTab::quickToggleMenu() {
  if (m_qSel.empty() || !m_qSelHasMenu) {
    return;
  }
  m_qMenuId = m_qMenuId == m_qSel ? std::string{} : m_qSel;
  quickDisarm();
  quickRequestUpdate();
}

void NetworkTab::quickEnter() {
  const std::string id = m_qSel;
  if (id.empty()) {
    return;
  }
  m_qMenuId.clear();
  quickDisarm();
  const auto noteIsBusy = [this](const std::string& rowId) {
    const auto it = m_qNotes.find(rowId);
    return it != m_qNotes.end() && it->second.busy && it->second.until > std::chrono::steady_clock::now();
  };
  if (noteIsBusy(id)) {
    return;
  }

  if (id == "ip") {
    quickCopy(id, m_qSnap.ipPlain);
  } else if (id.starts_with("c:")) {
    const auto it = std::ranges::find(m_qProfiles, id.substr(2), &netq::Profile::uuid);
    if (it == m_qProfiles.end()) {
      return;
    }
    if (it->active || it->activating) {
      quickSetNote(id, "already active", NoteTone::Dim, kInfoTtl);
    } else {
      quickConnectProfile(*it);
    }
  } else if (id == "w:radio") {
    if (networkAvailable() && m_network->state().wirelessEnabled) {
      m_network->requestScan();
      quickSetNote(id, "scanning", NoteTone::Dim, kInfoTtl);
    } else {
      quickToggleWifi();
    }
  } else if (id.starts_with("w:")) {
    if (m_network == nullptr) {
      return;
    }
    const std::string ssid = id.substr(2);
    for (const auto& ap : m_network->accessPoints()) {
      if (ap.ssid == ssid) {
        quickConnectAccessPoint(ap);
        return;
      }
    }
  } else if (id == "t:toggle") {
    quickTailscaleToggle();
  } else if (id == "t:exit") {
    m_qMenuId = id;
    quickRequestUpdate();
  } else if (id.starts_with("p:")) {
    const std::string name = id.substr(2);
    for (const auto& peer : m_qTs.peers) {
      if (peer.name == name) {
        quickCopy(id, peer.ip);
        return;
      }
    }
  }
}

void NetworkTab::quickCopy(const std::string& rowId, const std::string& text) {
  if (text.empty()) {
    quickSetNote(rowId, "failed: no address", NoteTone::Error, kFailureTtl);
    return;
  }
  if (m_clipboard == nullptr) {
    quickSetNote(rowId, "failed: no clipboard", NoteTone::Error, kFailureTtl);
    return;
  }
  m_clipboard->setClipboardText(text);
  quickSetNote(rowId, "copied", NoteTone::Accent, kInfoTtl);
}

void NetworkTab::quickToggleWifi() {
  if (m_network == nullptr || m_wifiTogglePending) {
    return;
  }
  requestWirelessEnabled(!m_network->state().wirelessEnabled);
  PanelManager::instance().refresh();
}

void NetworkTab::quickEditConnections(const std::string& uuid, const std::string& name) {
  // NetworkManager's own editor covers what this panel does not: static addresses, DNS, proxies, secrets.
  bool started = false;
  if (process::commandExists("nm-connection-editor")) {
    started = uuid.empty() ? process::runAsync({"uwsm-app", "--", "nm-connection-editor"})
                           : process::runAsync({"uwsm-app", "--", "nm-connection-editor", "--edit", uuid.c_str()});
  } else if (uuid.empty()) {
    started = process::runAsync({"uwsm-app", "--", "ghostty", "-e", "nmtui"});
  } else {
    started = process::runAsync({"uwsm-app", "--", "ghostty", "-e", "nmtui", "edit", name.c_str()});
  }
  const std::string rowId = uuid.empty() ? std::string{} : "c:" + uuid;
  if (started) {
    PanelManager::instance().close();
  } else {
    quickSetNote(rowId, "failed: no editor found", NoteTone::Error, kFailureTtl);
  }
}

// Runs `args` for a row: success clears its note, failure shows the reason; `refresh` re-reads the state either way.
void NetworkTab::quickRunRowCommand(
    const std::string& rowId, std::vector<std::string> args, std::chrono::seconds timeout, void (NetworkTab::*refresh)()
) {
  asynccmd::run(std::move(args), timeout, m_alive, [this, rowId, refresh](process::RunResult result) {
    if (result) {
      quickClearNote(rowId);
    } else {
      quickSetNote(rowId, "failed: " + netq::failureReason(result), NoteTone::Error, kFailureTtl);
    }
    (this->*refresh)();
  });
}

void NetworkTab::quickConnectProfile(const netq::Profile& profile) {
  const std::string rowId = "c:" + profile.uuid;
  quickSetBusy(rowId, "connecting…");
  quickRunRowCommand(
      rowId, {"nmcli", "connection", "up", "uuid", profile.uuid}, kProfileUpTimeout, &NetworkTab::quickRefreshProfiles
  );
}

void NetworkTab::quickDisconnectProfile(const netq::Profile& profile) {
  const std::string rowId = "c:" + profile.uuid;
  quickSetBusy(rowId, "disconnecting…");
  quickRunRowCommand(
      rowId, {"nmcli", "connection", "down", "uuid", profile.uuid}, kProfileDownTimeout,
      &NetworkTab::quickRefreshProfiles
  );
}

void NetworkTab::quickCopyProfileIp(const std::string& rowId, const netq::Profile& profile) {
  if (!m_qSnap.iface.empty() && profile.device == m_qSnap.iface && !m_qSnap.ipPlain.empty()) {
    quickCopy(rowId, m_qSnap.ipPlain);
    return;
  }
  asynccmd::run(
      {"nmcli", "-g", "IP4.ADDRESS", "connection", "show", "uuid", profile.uuid}, std::chrono::seconds(5), m_alive,
      [this, rowId](process::RunResult result) {
        if (!result) {
          quickSetNote(rowId, "failed: " + netq::failureReason(result), NoteTone::Error, kFailureTtl);
          return;
        }
        std::string address = asynccmd::firstLine(result.out);
        if (const auto cut = address.find_first_of("/|"); cut != std::string::npos) {
          address.resize(cut);
        }
        quickCopy(rowId, address);
      }
  );
}

void NetworkTab::quickConnectAccessPoint(const AccessPointInfo& ap) {
  if (m_network == nullptr) {
    return;
  }
  const std::string rowId = "w:" + ap.ssid;
  if (ap.active) {
    quickSetNote(rowId, "already connected", NoteTone::Dim, kInfoTtl);
    return;
  }
  const bool saved = m_network->hasSavedConnection(ap.ssid);
  if (!saved && ap.isEnterprise()) {
    quickSetNote(rowId, "failed: 802.1x needs the full view (f)", NoteTone::Error, kFailureTtl);
    return;
  }
  if (!saved && ap.requiresCredentials()) {
    showPasswordPrompt(ap);
    m_qFocusPassword = true;
    quickRequestUpdate();
    return;
  }
  if (!m_network->activateAccessPoint(ap)) {
    quickSetNote(rowId, "failed: could not start", NoteTone::Error, kFailureTtl);
    return;
  }
  m_qAttempt = WifiAttempt{.ssid = ap.ssid, .since = std::chrono::steady_clock::now()};
  quickSetBusy(rowId, "connecting…");
}

void NetworkTab::quickSyncAttempts() {
  if (!m_qAttempt.has_value() || m_network == nullptr) {
    return;
  }
  WifiAttempt& attempt = *m_qAttempt;
  const std::string rowId = "w:" + attempt.ssid;
  const NetworkState& state = m_network->state();
  const auto now = std::chrono::steady_clock::now();
  const auto fail = [&](const std::string& reason) {
    quickSetNote(rowId, "failed: " + reason, NoteTone::Error, kFailureTtl);
    m_qAttempt.reset();
  };
  if (state.connected && state.kind == NetworkConnectivity::Wireless && state.ssid == attempt.ssid) {
    quickClearNote(rowId);
    m_qAttempt.reset();
    return;
  }
  if (m_hasPendingSecret && !m_pendingAccessPoint.has_value() && m_pendingSsid == attempt.ssid) {
    // NetworkManager asks for the secret again.
    if (attempt.withPassword) {
      fail("wrong password");
    } else {
      quickClearNote(rowId);
      m_qAttempt.reset();
    }
    return;
  }
  attempt.sawResolving = attempt.sawResolving || state.resolving;
  if (attempt.sawResolving && !state.resolving && !state.connected) {
    fail("could not connect");
  } else if (!attempt.sawResolving && now - attempt.since > kAttemptGrace && !state.resolving) {
    fail("no response");
  } else if (now - attempt.since > kAttemptTimeout) {
    fail("timeout");
  }
}

void NetworkTab::quickTailscaleToggle() {
  const bool up = !m_qTs.running;
  const std::string rowId = "t:toggle";
  quickSetBusy(rowId, up ? "connecting…" : "disconnecting…");
  asynccmd::run(
      std::vector<std::string>{std::string("tailscale"), std::string(up ? "up" : "down")},
      std::chrono::seconds(up ? 12 : 8), m_alive, [this, rowId, up](process::RunResult result) {
        if (result) {
          quickClearNote(rowId);
        } else if (up && result.timedOut) {
          quickSetNote(rowId, "failed: needs login (tailscale up)", NoteTone::Error, kFailureTtl);
        } else {
          quickSetNote(rowId, "failed: " + netq::failureReason(result), NoteTone::Error, kFailureTtl);
        }
        m_qTsAt = {};
        quickRefreshTailscale();
      }
  );
}

void NetworkTab::quickTailscaleExit(const std::string& target, const std::string& label) {
  const std::string rowId = "t:exit";
  quickSetBusy(rowId, label.empty() ? "clearing…" : "using " + label + "…");
  quickRunRowCommand(
      rowId, {"tailscale", "set", "--exit-node=" + target}, kTailscaleExitTimeout, &NetworkTab::quickRefreshTailscale
  );
}

// Destructive items ask for a second press within kArmTtl; returns true once the press is confirmed.
bool NetworkTab::quickConfirmDestructive(const std::string& rowId, const std::string& itemId) {
  const std::string key = rowId + "|" + itemId;
  if (m_qArmed == key) {
    return true;
  }
  m_qArmed = key;
  m_qArmedUntil = std::chrono::steady_clock::now() + kArmTtl;
  m_qArmTimer.start(std::chrono::duration_cast<std::chrono::milliseconds>(kArmTtl), [this]() {
    m_qArmed.clear();
    quickRequestUpdate();
  });
  quickRequestUpdate();
  return false;
}

void NetworkTab::quickRunIpItem(const std::string& rowId, const std::string& itemId) {
  if (itemId == "copy") {
    quickCopy(rowId, m_qSnap.ipPlain);
    return;
  }
  if (itemId != "ext") {
    return;
  }
  const std::string external = m_externalIpService != nullptr ? m_externalIpService->externalIp() : std::string{};
  if (external.empty()) {
    quickSetNote(rowId, "ext ip unavailable", NoteTone::Warning, kFailureTtl);
    return;
  }
  if (m_clipboard != nullptr) {
    m_clipboard->setClipboardText(external);
  }
  quickSetNote(
      rowId, "ext " + external + (m_clipboard != nullptr ? " · copied" : ""), NoteTone::Accent, kExternalIpTtl
  );
}

void NetworkTab::quickRunProfileItem(const std::string& rowId, const std::string& itemId) {
  const auto it = std::ranges::find(m_qProfiles, rowId.substr(2), &netq::Profile::uuid);
  if (it == m_qProfiles.end()) {
    return;
  }
  const netq::Profile profile = *it; // the list may be replaced while the command runs
  if (itemId == "connect") {
    quickConnectProfile(profile);
  } else if (itemId == "disconnect") {
    quickDisconnectProfile(profile);
  } else if (itemId == "copyip") {
    quickCopyProfileIp(rowId, profile);
  } else if (itemId == "auto") {
    quickSetBusy(rowId, "updating…");
    quickRunRowCommand(
        rowId,
        {"nmcli", "connection", "modify", "uuid", profile.uuid, "connection.autoconnect",
         profile.autoconnect ? "no" : "yes"},
        kProfileEditTimeout, &NetworkTab::quickRefreshProfiles
    );
  } else if (itemId == "forget") {
    quickSetBusy(rowId, "forgetting…");
    quickRunRowCommand(
        rowId, {"nmcli", "connection", "delete", "uuid", profile.uuid}, kProfileDeleteTimeout,
        &NetworkTab::quickRefreshProfiles
    );
  } else if (itemId == "edit") {
    quickEditConnections(profile.uuid, profile.name);
  }
}

void NetworkTab::quickRunTailscaleItem(const std::string& rowId, const std::string& itemId) {
  if (rowId == "t:exit") {
    if (itemId == "none") {
      quickTailscaleExit({}, {});
    } else if (itemId.size() > 1 && itemId.front() == 'x') {
      // "x<N>": the N-th exit-capable peer, in the order the menu listed them.
      const std::size_t wanted = static_cast<std::size_t>(std::atoi(itemId.c_str() + 1));
      std::size_t number = 0;
      for (const auto& peer : m_qTs.peers) {
        if (!peer.exitOption) {
          continue;
        }
        if (number++ == wanted) {
          quickTailscaleExit(peer.ip.empty() ? peer.name : peer.ip, peer.name);
          break;
        }
      }
    }
  } else if (itemId == "copy") { // "p:<peer>"
    const std::string name = rowId.substr(2);
    for (const auto& peer : m_qTs.peers) {
      if (peer.name == name) {
        quickCopy(rowId, peer.ip);
        break;
      }
    }
  }
}

void NetworkTab::quickRunItem(const std::string& rowId, const std::string& itemId, bool destructive) {
  if (destructive && !quickConfirmDestructive(rowId, itemId)) {
    return;
  }
  quickDisarm();
  m_qMenuId.clear();

  if (rowId == "ip") {
    quickRunIpItem(rowId, itemId);
  } else if (rowId.starts_with("c:")) {
    quickRunProfileItem(rowId, itemId);
  } else if (rowId == "w:radio") {
    if (itemId == "off") {
      quickToggleWifi();
    }
  } else if (rowId.starts_with("w:")) {
    if (itemId == "forget" && m_network != nullptr) {
      m_network->forgetSsid(rowId.substr(2));
      quickSetNote(rowId, "forgotten", NoteTone::Dim, kInfoTtl);
      quickRefreshProfiles();
      PanelManager::instance().refresh();
    }
  } else if (rowId == "t:exit" || (rowId.starts_with("p:") && itemId == "copy")) {
    quickRunTailscaleItem(rowId, itemId);
  }
  quickRequestUpdate();
}

void NetworkTab::quickRefreshProfiles() {
  if (!m_qHasNmcli) {
    return;
  }
  if (m_qProfilesInFlight) {
    m_qProfilesAt = {}; // read again on the next tick: the answer in flight may predate the change
    return;
  }
  m_qProfilesInFlight = true;
  m_qProfilesAt = std::chrono::steady_clock::now();
  asynccmd::run(
      {"nmcli", "-t", "-f", netq::kProfileFields, "connection", "show"}, std::chrono::seconds(6), m_alive,
      [this](process::RunResult result) {
        m_qProfilesInFlight = false;
        if (result) {
          m_qProfiles = netq::parseProfiles(result.out);
          m_qProfilesLoaded = true;
        } else if (!m_qProfilesLoaded) {
          m_qProfiles.clear();
        }
        quickRequestUpdate();
      }
  );
}

void NetworkTab::quickRefreshTailscale() {
  if (!m_qHasTailscale) {
    return;
  }
  if (m_qTsInFlight) {
    m_qTsAt = {};
    return;
  }
  m_qTsInFlight = true;
  m_qTsAt = std::chrono::steady_clock::now();
  asynccmd::run({"tailscale", "status", "--json"}, std::chrono::seconds(5), m_alive, [this](process::RunResult result) {
    m_qTsInFlight = false;
    if (result) {
      m_qTs = netq::parseTailscaleStatus(result.out);
    } else {
      m_qTs = netq::TsState{};
      m_qTs.loaded = true;
      m_qTs.problem = (netq::contains(result.err, "tailscaled") || netq::contains(result.err, "connect"))
          ? "daemon down"
          : netq::failureReason(result);
    }
    quickRequestUpdate();
  });
}

// One ping into `slot`. A reply that arrives after the slot's target changed (generation bumped) is dropped.
void NetworkTab::quickProbe(const std::string& host, netq::Probe& slot) {
  if (host.empty() || slot.inFlight) {
    return;
  }
  slot.inFlight = true;
  asynccmd::run(
      {"ping", "-c1", "-W1", host}, kPingTimeout, m_alive,
      [this, &slot, generation = slot.generation](process::RunResult result) {
        if (generation != slot.generation) {
          return;
        }
        slot.inFlight = false;
        if (result.exitCode < 0 && !result.timedOut) {
          slot.known = false; // no ping binary: nothing to show
          return;
        }
        slot.known = true;
        slot.ms = result ? netq::parsePingMs(result.out) : std::nullopt;
        quickRequestUpdate();
      }
  );
}

void NetworkTab::quickProbeLatency() {
  m_qPingAt = std::chrono::steady_clock::now();
  if (!m_qSnap.connected) {
    m_qPingGateway.known = false;
    m_qPingInternet.known = false;
    return;
  }
  quickProbe(m_qSnap.gateway, m_qPingGateway);
  quickProbe(kInternetProbeHost, m_qPingInternet);
}

void NetworkTab::quickTick() {
  if (!m_quick || !m_active) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (m_qHasNmcli && !m_qProfilesInFlight && now - m_qProfilesAt >= kProfilesEvery) {
    quickRefreshProfiles();
  }
  if (m_qHasTailscale && !m_qTsInFlight && now - m_qTsAt >= kTailscaleEvery) {
    quickRefreshTailscale();
  }
  if (m_qSnap.gateway != m_qPingGatewayHost) {
    m_qPingGatewayHost = m_qSnap.gateway;
    m_qPingGateway.known = false;
    m_qPingGateway.inFlight = false;
    ++m_qPingGateway.generation;
    m_qPingAt = {};
  }
  if (now - m_qPingAt >= kPingEvery) {
    quickProbeLatency();
  }
}

void NetworkTab::settleWifiToggle(const NetworkState& state) {
  if (!m_wifiTogglePending) {
    return;
  }
  if (state.wirelessEnabled == m_wifiToggleTarget) {
    m_wifiToggleTargetObserved = true;
  }
  if (m_wifiToggleWriteComplete && m_wifiToggleTargetObserved) {
    m_wifiTogglePending = false;
    m_wifiToggleWriteComplete = false;
    m_wifiToggleTargetObserved = false;
  }
}

void NetworkTab::resetQuick() {
  m_qPollTimer.stop();
  m_qArmTimer.stop();
  m_qHeroMain = nullptr;
  m_qHeroDim = nullptr;
  m_qLatency = nullptr;
  m_qIpHolder = nullptr;
  m_qConnHolder = nullptr;
  m_qWifiHolder = nullptr;
  m_qTsHolder = nullptr;
  m_qTrafficBox = nullptr;
  m_qTrafficTitle = nullptr;
  m_qAxisTop = nullptr;
  m_qAxisBottom = nullptr;
  m_qGraphBox = nullptr;
  m_qGraph = nullptr;
  m_qTrafficText = nullptr;
  m_qPwInput = nullptr;
  m_qKeywords.clear();
  m_qBodyKey.clear();
  m_qHeightKey.clear();
  m_qMenuId.clear();
  m_qArmed.clear();
  m_qNotes.clear();
  m_qWinStart.clear();
  m_qSelHasMenu = false;
  m_qMenuKeys.clear();
  m_qPwOwner.clear();
  m_qAttempt.reset();
  m_qTrafficDirty = true;
  m_qFocusPassword = false;
  m_qGraphWidth = 0.0F;
  m_qHeldKey = 0;
  m_qLastPoll = {};
}

std::unique_ptr<Flex> NetworkTab::buildQuickHero(float scale) {
  const float font = kMono * scale;
  auto hero = ui::column({.align = FlexAlign::Stretch});
  hero->addChild(
      ui::row(
          {.align = FlexAlign::Center,
           .gap = Style::spaceMd * scale,
           .minHeight = kQuickHeroHeight * scale,
           .fillWidth = true},
          ui::label({
              .out = &m_qHeroMain,
              .fontSize = Style::fontSizeTitle * scale,
              .fontFamily = std::string(term_frame::kFont),
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
          }),
          ui::label({
              .out = &m_qHeroDim,
              .fontSize = Style::fontSizeCaption * scale,
              .fontFamily = std::string(term_frame::kFont),
              .color = term_frame::dimColor(),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
              .flexGrow = 1.0F,
          })
      )
  );
  hero->addChild(
      ui::row(
          {.align = FlexAlign::Center, .minHeight = kQuickRow * scale, .fillWidth = true},
          ui::label({
              .out = &m_qLatency,
              .fontSize = font,
              .fontFamily = std::string(term_frame::kFont),
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
              .configure = [](Label& label) { label.setUseMarkup(true); },
          })
      )
  );
  return hero;
}

// The scale on the left of the traffic graph: peak on top, zero at the bottom.
std::unique_ptr<Flex> NetworkTab::buildQuickGraphAxis(float scale) {
  auto axis = ui::column({
      .justify = FlexJustify::SpaceBetween,
      .width = kAxisWidth * scale,
      .height = kQuickGraphHeight * scale,
  });
  axis->addChild(
      ui::label({
          .out = &m_qAxisTop,
          .text = "",
          .fontSize = Style::fontSizeMini * scale,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
          .width = kAxisWidth * scale,
          .maxLines = 1,
          .textAlign = TextAlign::End,
      })
  );
  axis->addChild(
      ui::label({
          .out = &m_qAxisBottom,
          .text = "0",
          .fontSize = Style::fontSizeMini * scale,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
          .width = kAxisWidth * scale,
          .maxLines = 1,
          .textAlign = TextAlign::End,
      })
  );
  return axis;
}

// Holder box and the graph in it; doLayoutQuick resizes both once the width is known.
std::unique_ptr<Box> NetworkTab::buildQuickGraphBox(float scale) {
  auto graphBox = ui::box();
  m_qGraphBox = graphBox.get();
  m_qGraphBox->setSize(1.0F, kQuickGraphHeight * scale);
  auto graph = std::make_unique<Graph>();
  graph->setLineWidth(1.5F * scale);
  graph->setFillOpacity(0.15F);
  graph->setColor(colorSpecFromRole(ColorRole::Primary));
  graph->setColor2(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_qGraph = static_cast<Graph*>(graphBox->addChild(std::move(graph)));
  return graphBox;
}

// Traffic: rx/tx graph of the last minute with a scale on the left, and a figures line.
std::unique_ptr<Flex> NetworkTab::buildQuickTrafficBox(float scale) {
  auto traffic = quickBox("", "traffic", scale, &m_qTrafficTitle);
  m_qTrafficBox = traffic.get();
  auto axis = buildQuickGraphAxis(scale);
  auto graphBox = buildQuickGraphBox(scale);
  traffic->addChild(
      ui::row({.align = FlexAlign::Start, .gap = Style::spaceXs * scale}, std::move(axis), std::move(graphBox))
  );
  traffic->addChild(
      ui::row(
          {.align = FlexAlign::Center, .minHeight = kQuickRow * scale, .fillWidth = true},
          ui::label({
              .out = &m_qTrafficText,
              .fontSize = kMono * scale,
              .fontFamily = std::string(term_frame::kFont),
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
              .configure = [](Label& label) { label.setUseMarkup(true); },
          })
      )
  );
  return traffic;
}

std::unique_ptr<Flex> NetworkTab::createQuick() {
  const float scale = contentScale();

  m_qSnap = readQuickSnapshot();
  m_qBodyKey.clear();
  m_qHeightKey.clear();

  auto root = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = kQuickGap * scale,
  });
  root->addChild(buildQuickHero(scale));

  // Filled by rebuildQuickBody().
  for (Flex** holder : {&m_qIpHolder, &m_qConnHolder, &m_qWifiHolder, &m_qTsHolder}) {
    root->addChild(ui::column({.out = holder, .align = FlexAlign::Stretch}));
  }

  root->addChild(buildQuickTrafficBox(scale));
  return root;
}

void NetworkTab::doLayoutQuick(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }
  const float scale = contentScale();
  m_qWidth = contentWidth / std::max(scale, 0.01F);
  m_qGraphWidth =
      std::max(1.0F, contentWidth - 2.0F * term_frame::boxInset(scale) - (kAxisWidth + Style::spaceXs) * scale);
  if (m_qGraphBox != nullptr && m_qGraph != nullptr) {
    m_qGraphBox->setSize(m_qGraphWidth, kQuickGraphHeight * scale);
    m_qGraph->setPosition(0.0F, 0.0F);
    m_qGraph->setSize(m_qGraphWidth, kQuickGraphHeight * scale);
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  syncQuick(renderer, true);
  if (m_rootLayout != nullptr) {
    m_rootLayout->layout(renderer);
  }
}

// Drops the old rows and returns the password typed so far; keeps the field focused across the rebuild.
std::string NetworkTab::quickClearBody() {
  std::string carried;
  if (m_qPwInput != nullptr) {
    carried = m_qPwInput->value();
    const InputDispatcher* dispatcher = inputDispatcher();
    if (dispatcher != nullptr && dispatcher->focusedArea() == m_qPwInput->inputArea()) {
      m_qFocusPassword = true;
    }
  }
  m_qPwInput = nullptr;
  m_qKeywords.clear();
  for (Flex* holder : {m_qIpHolder, m_qConnHolder, m_qWifiHolder, m_qTsHolder}) {
    while (!holder->children().empty()) {
      holder->removeChild(holder->children().front().get());
    }
  }
  return carried;
}

std::unique_ptr<Button> NetworkTab::quickRowButton(const QRow& row, bool selected, float scale) {
  std::string markup = selected ? colorSpan(ColorRole::Primary, "▸") + " " : "  ";
  markup += row.dimName ? dimSpan(row.name) : escapeMarkup(row.name);
  term_frame::ToggleRow toggle;
  std::function<void()> onRight;
  if (!row.menu.empty()) {
    onRight = [this, id = row.id]() {
      m_qSel = id;
      m_qMenuId = m_qMenuId == id ? std::string{} : id;
      quickDisarm();
      quickRequestUpdate();
    };
  }
  auto button = term_frame::toggleRow(
      std::string{}, scale, toggle,
      [this, id = row.id]() {
        m_qSel = id;
        m_qMenuId.clear();
        quickDisarm();
        quickEnter();
        quickRequestUpdate();
      },
      std::move(onRight)
  );
  if (button->label() != nullptr) {
    button->label()->setUseMarkup(true);
  }
  button->setText(markup);
  if (toggle.detail != nullptr) {
    toggle.detail->setUseMarkup(true);
  }
  term_frame::setToggleRow(toggle, true, row.lit, row.badge, row.detail);
  setShown(toggle.stateBox, !row.badge.empty());
  return button;
}

// An open action menu: its items wrapped into lines, the armed destructive one reading "confirm?".
std::unique_ptr<Flex> NetworkTab::quickMenuBlock(const QElem& elem, float scale) {
  auto block = ui::column({.align = FlexAlign::Stretch});
  for (const auto& line : elem.lines) {
    auto lineRow = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .minHeight = kQuickRow * scale,
        .fillWidth = true,
    });
    lineRow->addChild(
        ui::label({
            .text = "  ",
            .fontSize = kMono * scale,
            .fontFamily = std::string(term_frame::kFont),
            .color = term_frame::dimColor(),
        })
    );
    for (const auto& item : line) {
      auto keyword = std::make_unique<term_frame::Keyword>();
      const bool armed = item.destructive && m_qArmed == elem.row.id + "|" + item.id;
      lineRow->addChild(
          term_frame::makeKeyword(
              *keyword, item.word, item.hot, scale,
              [this, rowId = elem.row.id, itemId = item.id, destructive = item.destructive]() {
                quickRunItem(rowId, itemId, destructive);
              }
          )
      );
      if (armed) {
        keyword->block = true;
        keyword->blockText = "confirm?";
        term_frame::refreshKeyword(*keyword);
      }
      m_qKeywords.push_back(std::move(keyword));
    }
    block->addChild(std::move(lineRow));
  }
  return block;
}

std::unique_ptr<Flex> NetworkTab::quickPasswordRow(float scale) {
  return ui::row(
      {.align = FlexAlign::Center,
       .gap = Style::spaceXs * scale,
       .minHeight = kQuickPasswordHeight * scale,
       .fillWidth = true},
      ui::label({
          .text = "  ",
          .fontSize = kMono * scale,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
      }),
      ui::input({
          .out = &m_qPwInput,
          .placeholder = "password",
          .fontSize = Style::fontSizeCaption * scale,
          .controlHeight = (kQuickPasswordHeight - 2.0F) * scale,
          .passwordMode = true,
          .surfaceRole = ColorRole::Surface,
          .flexGrow = 1.0F,
          .onSubmit = [this](const std::string& value) {
            if (value.empty()) {
              return;
            }
            if (!m_pendingSsid.empty()) {
              m_qAttempt =
                  WifiAttempt{.ssid = m_pendingSsid, .since = std::chrono::steady_clock::now(), .withPassword = true};
              quickSetBusy("w:" + m_pendingSsid, "connecting…");
            }
            submitPasswordPrompt(value);
          },
      })
  );
}

namespace {

  std::unique_ptr<Flex> quickTextRow(const std::string& text, float scale) {
    return ui::row(
        {.align = FlexAlign::Center, .minHeight = kQuickRow * scale, .fillWidth = true},
        ui::label({
            .text = text,
            .fontSize = kMono * scale,
            .fontFamily = std::string(term_frame::kFont),
            .color = term_frame::dimColor(),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
        })
    );
  }

} // namespace

void NetworkTab::quickRenderElems(Flex& parent, const QSection& section, const std::string& selectedId, float scale) {
  for (const auto& elem : section.elems) {
    switch (elem.kind) {
    case ElemKind::Row:
      parent.addChild(quickRowButton(elem.row, elem.row.id == selectedId, scale));
      break;
    case ElemKind::Menu:
      parent.addChild(quickMenuBlock(elem, scale));
      break;
    case ElemKind::Password:
      parent.addChild(quickPasswordRow(scale));
      break;
    case ElemKind::Text:
      parent.addChild(quickTextRow(elem.text, scale));
      break;
    }
  }
}

// A bordered, numbered section in `holder`; an empty section hides its holder.
void NetworkTab::quickFillBox(
    Flex* holder, const QSection& section, std::size_t& numeral, const std::string& selectedId, float scale
) {
  const bool show = !section.elems.empty();
  setShown(holder, show);
  if (!show) {
    return;
  }
  Label* chip = nullptr;
  auto box = quickBox(kNumerals[std::min(numeral++, kNumerals.size() - 1)], section.title, scale, nullptr, &chip);
  if (chip != nullptr) {
    term_frame::setChip(*chip, section.chip);
  }
  quickRenderElems(*box, section, selectedId, scale);
  holder->addChild(std::move(box));
}

void NetworkTab::rebuildQuickBody(const QuickModel& model, float scale) {
  if (m_qIpHolder == nullptr || m_qConnHolder == nullptr || m_qWifiHolder == nullptr || m_qTsHolder == nullptr) {
    return;
  }
  const std::string carried = quickClearBody();

  std::size_t numeral = 0;
  setShown(m_qIpHolder, !model.ip.elems.empty());
  quickRenderElems(*m_qIpHolder, model.ip, model.sel, scale);
  quickFillBox(m_qConnHolder, model.conn, numeral, model.sel, scale);
  quickFillBox(m_qWifiHolder, model.wifi, numeral, model.sel, scale);
  quickFillBox(m_qTsHolder, model.tailscale, numeral, model.sel, scale);

  setShown(m_qTrafficBox, model.traffic);
  if (m_qTrafficTitle != nullptr) {
    m_qTrafficTitle->setText(
        std::string(model.traffic ? kNumerals[std::min(numeral, kNumerals.size() - 1)] : "") + "traffic"
    );
  }

  if (m_qPwInput != nullptr) {
    if (!carried.empty()) {
      m_qPwInput->setValue(carried);
    }
  } else {
    m_qFocusPassword = false;
  }
}

void NetworkTab::syncQuickTraffic(Renderer& renderer) {
  m_qTrafficDirty = false;
  if (m_monitor == nullptr || m_qGraph == nullptr || m_qTrafficText == nullptr) {
    return;
  }
  const auto history = m_monitor->history(kTrafficSamples);
  std::vector<double> rxRaw;
  std::vector<double> txRaw;
  double peakRx = 0.0;
  double peakTx = 0.0;
  for (const auto& sample : history) {
    const auto [r, t] = throughputOf(sample, m_qSnap.iface);
    rxRaw.push_back(r);
    txRaw.push_back(t);
    peakRx = std::max(peakRx, r);
    peakTx = std::max(peakTx, t);
  }
  // Both series share one axis, scaled to the window peak and rounded up to a round number; the floor keeps an idle
  // link from rendering noise as full scale.
  const double axis = netq::niceCeiling(std::max({peakRx, peakTx, 10240.0}));
  std::vector<float> rx;
  std::vector<float> tx;
  rx.reserve(rxRaw.size());
  tx.reserve(txRaw.size());
  for (std::size_t i = 0; i < rxRaw.size(); ++i) {
    rx.push_back(static_cast<float>(rxRaw[i] / axis));
    tx.push_back(static_cast<float>(txRaw[i] / axis));
  }
  if (rx.size() < 2) {
    rx.clear();
    tx.clear();
  }
  m_qGraph->setValues(std::move(rx));
  m_qGraph->setValues2(std::move(tx));
  m_qGraph->sync(renderer);
  if (m_qAxisTop != nullptr) {
    m_qAxisTop->setText(netq::axisLabel(axis));
  }

  const double curRx = rxRaw.empty() ? 0.0 : rxRaw.back();
  const double curTx = txRaw.empty() ? 0.0 : txRaw.back();
  const std::string rxTag = "<span foreground=\"" + term_frame::paletteHex(ColorRole::Primary) + "\">rx</span>";
  m_qTrafficText->setText(
      rxTag
      + " "
      + rateText(curRx)
      + "  "
      + dimSpan("peak")
      + " "
      + rateText(peakRx)
      + "   "
      + dimSpan("tx")
      + " "
      + rateText(curTx)
      + "  "
      + dimSpan("peak")
      + " "
      + rateText(peakTx)
  );
}

void NetworkTab::quickHeroText(std::string& main, std::string& dim) const {
  if (!networkAvailable()) {
    main = "unavailable";
    dim = "network service is not running";
  } else if (!m_qSnap.connected) {
    main = "offline";
    dim = m_qSnap.hasWifiDevice ? (m_network->state().wirelessEnabled ? "wi-fi on" : "wi-fi off") : "no link";
  } else if (m_qSnap.medium == "wi-fi") {
    main = m_qSnap.ssid.empty() ? m_qSnap.iface : m_qSnap.ssid;
    dim = "wi-fi";
    if (m_qSnap.signal > 0) {
      dim += std::format(" · {}%", m_qSnap.signal);
    }
    if (!m_qSnap.band.empty()) {
      dim += " · " + m_qSnap.band;
    }
  } else {
    main = m_qSnap.iface.empty() ? m_qSnap.medium : m_qSnap.iface;
    dim = m_qSnap.medium;
    if (!m_qSnap.speed.empty()) {
      dim += " · " + m_qSnap.speed;
    }
  }
}

std::string NetworkTab::quickLatencyMarkup() const {
  if (!m_qSnap.connected) {
    return {};
  }
  std::string latency;
  if (!m_qSnap.gateway.empty()) {
    latency = dimSpan("gateway") + " " + probeMarkup(m_qPingGateway, true) + "  ";
  }
  return latency + dimSpan("internet") + " " + probeMarkup(m_qPingInternet, false);
}

// A credential request that just arrived (the secret agent, or Enter on a locked network) selects its network and
// focuses the field. A request with no SSID ("-") has no row; it only focuses the field once.
void NetworkTab::quickTrackPasswordOwner() {
  const std::string owner = quickPasswordRowId();
  if (owner != m_qPwOwner) {
    m_qPwOwner = owner;
    if (!owner.empty()) {
      m_qSel = owner;
      m_qMenuId.clear();
      m_qFocusPassword = true;
    }
  } else if (m_hasPendingSecret && m_pendingSsid.empty() && m_qPwOwner != "-") {
    m_qPwOwner = "-";
    m_qFocusPassword = true;
  }
}

void NetworkTab::syncQuick(Renderer& renderer, bool force) {
  if (m_rootLayout == nullptr) {
    return;
  }
  const float scale = contentScale();
  syncPasswordCard();

  const auto now = std::chrono::steady_clock::now();
  const bool polled = force || now - m_qLastPoll >= std::chrono::milliseconds(250);
  if (polled) {
    m_qLastPoll = now;
    m_qSnap = readQuickSnapshot();
  }
  if (m_network != nullptr) {
    settleWifiToggle(m_network->state());
  }
  quickPruneNotes();
  quickSyncAttempts();

  quickTrackPasswordOwner();

  QuickModel model = buildQuickModel(m_qWidth, m_qSnap);
  m_qSel = model.sel;
  if (m_qMenuId != m_qSel) {
    m_qMenuId.clear();
  }
  m_qSelectables = std::move(model.selectables);
  m_qMenuKeys = std::move(model.menuKeys);
  m_qSelHasMenu = model.selHasMenu;

  bool relayout = false;

  std::string heroMain;
  std::string heroDim;
  quickHeroText(heroMain, heroDim);
  if (m_qHeroMain != nullptr && m_qHeroMain->setText(heroMain)) {
    relayout = true;
  }
  if (m_qHeroDim != nullptr && m_qHeroDim->setText(heroDim)) {
    relayout = true;
  }
  if (m_qLatency != nullptr) {
    const std::string latency = quickLatencyMarkup();
    if (m_qLatency->text() != latency) {
      m_qLatency->setText(latency);
      relayout = true;
    }
  }

  if (model.key != m_qBodyKey) {
    m_qBodyKey = model.key;
    rebuildQuickBody(model, scale);
    relayout = true;
  }
  if (relayout) {
    m_rootLayout->layout(renderer);
    relayout = false;
  }
  if (m_qFocusPassword && m_qPwInput != nullptr && m_qPwInput->inputArea() != nullptr) {
    m_qFocusPassword = false;
    PanelManager::instance().focusArea(m_qPwInput->inputArea());
  }

  if (model.traffic && (polled || m_qTrafficDirty)) {
    syncQuickTraffic(renderer);
    relayout = true;
  }
  if (relayout) {
    m_rootLayout->layout(renderer);
  }

  // Structural change in what the panel shows: the host reopens at the new height when it differs. Last, as a
  // reopen may tear this tab's nodes down.
  const std::string heightKey = std::format("{:.1f}", model.height);
  if (heightKey != m_qHeightKey) {
    m_qHeightKey = heightKey;
    requestQuickPanelResize();
  }
}
