#include "shell/control_center/tabs/bluetooth_tab.h"

#include "core/process/process.h"
#include "core/ui_phase.h"
#include "dbus/bluetooth/bluetooth_uuid_names.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "shell/control_center/tabs/async_command.h"
#include "shell/panel/panel_manager.h"
#include "system/health.h"
#include "ui/builders.h"
#include "ui/controls/collapsible.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

using namespace control_center;

namespace {

  constexpr float kRowMinHeight = Style::controlHeightLg;

  // Bounds an explicit Rescan: BlueZ discovery is stopped again when this window elapses.
  constexpr auto kDiscoveryTimeout = std::chrono::seconds(10);

  const char* glyphFor(BluetoothDeviceKind kind) {
    switch (kind) {
    case BluetoothDeviceKind::Headset:
      return "bluetooth-device-headset";
    case BluetoothDeviceKind::Headphones:
      return "bluetooth-device-headphones";
    case BluetoothDeviceKind::Earbuds:
      return "bluetooth-device-earbuds";
    case BluetoothDeviceKind::Speaker:
      return "bluetooth-device-speaker";
    case BluetoothDeviceKind::Microphone:
      return "bluetooth-device-microphone";
    case BluetoothDeviceKind::Mouse:
      return "bluetooth-device-mouse";
    case BluetoothDeviceKind::Keyboard:
      return "bluetooth-device-keyboard";
    case BluetoothDeviceKind::Phone:
      return "bluetooth-device-phone";
    case BluetoothDeviceKind::Computer:
      return "device-laptop";
    case BluetoothDeviceKind::Gamepad:
      return "bluetooth-device-gamepad";
    case BluetoothDeviceKind::Watch:
      return "bluetooth-device-watch";
    case BluetoothDeviceKind::Tv:
      return "bluetooth-device-tv";
    case BluetoothDeviceKind::Unknown:
    default:
      return "bluetooth-device-generic";
    }
  }

  enum class DeviceBucket : std::uint8_t {
    Connected,
    Paired,
    Available,
  };

  DeviceBucket bucketFor(const BluetoothDeviceInfo& d) {
    if (d.connected) {
      return DeviceBucket::Connected;
    }
    if (d.paired) {
      return DeviceBucket::Paired;
    }
    return DeviceBucket::Available;
  }

  int signalPercentFromRssi(std::int16_t rssi) {
    constexpr int kWeakRssi = -100;
    constexpr int kStrongRssi = -40;
    constexpr int kRange = kStrongRssi - kWeakRssi;
    const int clamped = std::clamp(static_cast<int>(rssi), kWeakRssi, kStrongRssi);
    return ((clamped - kWeakRssi) * 100 + kRange / 2) / kRange;
  }

  // Coarse signal band (0 weakest .. 4 strongest). Available devices order by band,
  // not by raw RSSI: every advertisement moves the RSSI a little, and ordering on it
  // reshuffles rows under the pointer mid-scan.
  int signalBandFromRssi(std::int16_t rssi) { return std::min(signalPercentFromRssi(rssi) / 20, 4); }

  std::string percentText(int percent) { return std::to_string(percent) + "%"; }

  // Bucket first, then strongest band, then a stable alias/path tiebreak so equally
  // named devices keep their order across rebuilds.
  std::vector<BluetoothDeviceInfo> sortedDevices(std::vector<BluetoothDeviceInfo> devices) {
    std::ranges::sort(devices, [](const BluetoothDeviceInfo& a, const BluetoothDeviceInfo& b) {
      const auto ba = bucketFor(a);
      const auto bb = bucketFor(b);
      if (ba != bb) {
        return static_cast<int>(ba) < static_cast<int>(bb);
      }
      if (ba == DeviceBucket::Available) {
        if (a.hasRssi != b.hasRssi) {
          return a.hasRssi;
        }
        if (a.hasRssi) {
          const int bandA = signalBandFromRssi(a.rssi);
          const int bandB = signalBandFromRssi(b.rssi);
          if (bandA != bandB) {
            return bandA > bandB;
          }
        }
      }
      if (a.alias != b.alias) {
        return a.alias < b.alias;
      }
      return a.path < b.path;
    });
    return devices;
  }

  std::unique_ptr<Flex> makeMetricPill(const char* glyphName, std::string text, float scale, Label** valueOut) {
    return ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceXs * 0.5F * scale},
        ui::glyph({
            .glyph = glyphName,
            .glyphSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        }),
        ui::label({
            .out = valueOut,
            .text = std::move(text),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
  }

  // ---- Quick (QuickPanel) mode ----
  constexpr float kQuickRow = term_frame::kRowHeight;
  constexpr float kQuickGap = Style::spaceSm;
  // Border + padding of a term_frame box, both sides.
  constexpr float kQuickBoxInset = 2.0F * (Style::spaceSm + Style::borderWidth);
  // ¹adapter box: title row and the two toggle rows.
  constexpr float kQuickAdapterHeight = kQuickBoxInset + 3.0F * kQuickRow + 2.0F * Style::spaceXs;
  constexpr float kQuickInputRow = Style::controlHeightSm;
  constexpr int kQuickMaxDevLines = 7;
  constexpr int kQuickMaxAvailLines = 6;
  // Selectable rows ahead of the devices: power and discoverable.
  constexpr std::size_t kQuickAdapterRows = 2;
  // The selection marker and its padding, in monospace cells; every selectable row reserves it.
  constexpr std::size_t kQuickGutterChars = 2;
  // Cells of a battery or signal meter.
  constexpr std::size_t kQuickMeterCells = 5;
  constexpr std::size_t kFailureReasonChars = 40;
  // A panel that closes (or reopens at a new height) keeps a running scan this long before it is stopped.
  constexpr auto kQuickDiscoveryGrace = std::chrono::milliseconds(1500);
  constexpr auto kQuickArmTimeout = std::chrono::seconds(4);
  constexpr auto kQuickFailureTimeout = std::chrono::seconds(10);
  // Failures expire by the clock, so a panel showing one updates at this interval.
  constexpr auto kQuickFailurePoll = std::chrono::seconds(1);
  constexpr auto kPactlListTimeout = std::chrono::milliseconds(3000);
  constexpr auto kPactlSwitchTimeout = std::chrono::milliseconds(5000);
  // Ids of the selectable rows that are not devices.
  const std::string kQuickPower = "power";
  const std::string kQuickDiscoverable = "discoverable";
  const std::string kQuickPair = "pair";

  // A box of `lines` kQuickRow lines under its title rule.
  constexpr float quickListBoxHeight(int lines) {
    return kQuickBoxInset + kQuickRow + Style::spaceXs + static_cast<float>(lines) * kQuickRow;
  }

  using term_frame::escapeMarkup;
  using term_frame::setShown;

  // For changes that arrive outside the normal refresh (key presses, timers).
  void requestQuickRefresh() {
    PanelManager::instance().requestUpdateOnly();
    PanelManager::instance().requestRedraw();
  }

  // A term_frame box whose title row is pinned to one grid row, so the heights above hold.
  std::unique_ptr<Flex> quickBox(
      std::string_view numeral, std::string_view title, float scale, Flex** out = nullptr, Label** chipOut = nullptr
  ) {
    auto box = term_frame::frame(scale, Style::spaceSm * scale, out);
    auto titleRow = term_frame::titleRow(numeral, title, scale, chipOut);
    titleRow->setMinHeight(kQuickRow * scale);
    box->addChild(std::move(titleRow));
    return box;
  }

  // One fixed-height line holding a single ellipsised label.
  std::unique_ptr<Flex>
  quickLabelRow(Label** out, ColorSpec color, float scale, FontWeight weight = FontWeight::Normal) {
    return ui::row(
        {.align = FlexAlign::Center, .minHeight = kQuickRow * scale, .maxHeight = kQuickRow * scale, .fillWidth = true},
        ui::label({
            .out = out,
            .fontSize = Style::fontSizeCaption * scale,
            .fontWeight = weight,
            .fontFamily = std::string(term_frame::kFont),
            .color = std::move(color),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
            .flexGrow = 1.0F,
        })
    );
  }

  // A dim line in a list (empty-state text).
  void addQuickNote(Flex& list, const std::string& text, float scale) {
    Label* label = nullptr;
    auto line = quickLabelRow(&label, term_frame::dimColor(), scale);
    label->setText(text);
    list.addChild(std::move(line));
  }

  // Pins a ²/³ box and its scroll list to `lines` rows plus `extra` px, so the panel height follows
  // quickPanelHeight().
  void pinQuickList(Flex* box, ScrollView* scroll, int lines, float extra, float scale) {
    if (box == nullptr || scroll == nullptr) {
      return;
    }
    const float boxHeight = (quickListBoxHeight(lines) + extra) * scale;
    box->setMinHeight(boxHeight);
    box->setMaxHeight(boxHeight);
    const float listHeight = (static_cast<float>(lines) * kQuickRow + extra) * scale;
    scroll->setMinHeight(listHeight);
    scroll->setMaxHeight(listHeight);
  }

  // Shows a power / discoverable toggle row; true when its text or layout-affecting state changed.
  bool applyToggleRow(
      term_frame::ToggleRow& row, bool enabled, bool lit, std::string_view text,
      const std::optional<std::string>& failure
  ) {
    const bool wasKnown = row.known;
    const bool wasLit = row.lit;
    const bool textChanged = row.state != nullptr && row.state->text() != text;
    const std::string detail = failure.has_value() ? "failed: " + *failure : std::string{};
    const bool detailChanged = row.detail != nullptr && row.detail->text() != detail;
    term_frame::setToggleRow(row, enabled, lit, text, detail);
    if (row.detail != nullptr) {
      row.detail->setColor(failure.has_value() ? colorSpecFromRole(ColorRole::Error) : term_frame::dimColor());
    }
    return textChanged || detailChanged || !wasKnown || wasLit != lit;
  }

  ColorRole toneRole(health::Level level) {
    return level == health::Level::Error ? ColorRole::Error : ColorRole::Warning;
  }

  std::string spanMarkup(ColorRole role, std::string_view text) {
    return "<span foreground=\"" + term_frame::paletteHex(role) + "\">" + std::string(text) + "</span>";
  }

  std::string repeated(std::string_view glyph, std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
      out += glyph;
    }
    return out;
  }

  // Cells filled in proportion to `percent`; an abnormal battery fills in the warning or error tone.
  std::string quickMeterMarkup(int percent, health::Level level) {
    const double fraction = std::clamp(static_cast<double>(percent) / 100.0, 0.0, 1.0);
    if (level == health::Level::Ok) {
      return term_frame::dottedMeterMarkup(kQuickMeterCells, fraction, true);
    }
    const std::size_t filled = std::min(
        kQuickMeterCells, static_cast<std::size_t>(std::lround(fraction * static_cast<double>(kQuickMeterCells)))
    );
    std::string out;
    if (filled > 0) {
      out += spanMarkup(toneRole(level), repeated("■", filled));
    }
    if (filled < kQuickMeterCells) {
      out += spanMarkup(ColorRole::Outline, repeated("·", kQuickMeterCells - filled));
    }
    return out;
  }

  std::string quickBatteryMarkup(const std::string& deviceKey, int percent) {
    // Warning at 20% or below, error at 10% or below, with hysteresis per device (system/health.h).
    const health::Level level = health::battery(deviceKey, static_cast<double>(percent), false);
    const bool abnormal = level != health::Level::Ok;
    const std::string text = abnormal ? std::format(" low {}%", percent) : std::format(" {:3}%", percent);
    return quickMeterMarkup(percent, level) + (abnormal ? spanMarkup(toneRole(level), text) : text);
  }

  std::string quickSignalMarkup(int percent) {
    return term_frame::dottedMeterMarkup(kQuickMeterCells, static_cast<double>(percent) / 100.0, true)
        + std::format(" {:3}%", percent);
  }

  std::string upperAscii(std::string value) {
    for (char& c : value) {
      if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
      }
    }
    return value;
  }

  // First line of a command's stderr, lower-case and short; `fallback` when empty.
  std::string firstLineLower(const std::string& text, const char* fallback) {
    std::string line = asynccmd::firstLine(text);
    if (line.empty()) {
      return fallback;
    }
    line = term_frame::lowerAscii(std::move(line));
    if (line.size() > kFailureReasonChars) {
      line.resize(kFailureReasonChars);
    }
    return line;
  }

  // Devices whose card exposes switchable profiles (a2dp / headset).
  bool isHeadphones(BluetoothDeviceKind kind) {
    return kind == BluetoothDeviceKind::Headphones
        || kind == BluetoothDeviceKind::Headset
        || kind == BluetoothDeviceKind::Earbuds;
  }

  // Paths of the connected headphones; a change means their pactl cards have to be read again.
  std::string connectedHeadphonesKey(const std::vector<BluetoothDeviceInfo>& devices) {
    std::string key;
    for (const auto& device : devices) {
      if (device.connected && isHeadphones(device.kind)) {
        key += device.path + ';';
      }
    }
    return key;
  }

  // "a2dp-sink" -> "a2dp", "headset-head-unit" -> "headset".
  std::string profileShort(const std::string& profile) { return profile.substr(0, profile.find_first_of("-_")); }

  // The short state marker before the name: an accent dot for connected, a dim ring for paired, a half circle while
  // connecting; unpaired scan results carry no marker. The name keeps every remaining cell.
  std::string deviceNameMarkup(const BluetoothDeviceInfo& device) {
    std::string marker = " ";
    if (device.connecting) {
      marker = spanMarkup(ColorRole::OnSurfaceVariant, "\u25d0");
    } else if (device.connected) {
      marker = spanMarkup(ColorRole::Primary, std::string(term_frame::kMarkerOn));
    } else if (device.paired) {
      marker = spanMarkup(ColorRole::OnSurfaceVariant, std::string(term_frame::kMarkerOff));
    }
    return marker + " " + escapeMarkup(device.alias);
  }

  // The pairing prompt kinds that read a code typed into the panel.
  bool pairingNeedsInput(BluetoothPairingKind kind) {
    return kind == BluetoothPairingKind::PinCode || kind == BluetoothPairingKind::Passkey;
  }

  bool pairingShowsCode(BluetoothPairingKind kind) {
    return kind == BluetoothPairingKind::Confirm
        || kind == BluetoothPairingKind::DisplayPasskey
        || kind == BluetoothPairingKind::DisplayPinCode;
  }

  // The code a prompt shows: the PIN, or the six-digit passkey.
  std::string pairingCodeText(const BluetoothPairingRequest& req) {
    return req.kind == BluetoothPairingKind::DisplayPinCode ? req.pin : std::format("{:06}", req.passkey);
  }

  // A passkey is a decimal number that fits 32 bits; anything else cancels the pairing instead of sending a wrong code.
  void submitPasskeyText(BluetoothAgent& agent, const std::string& text) {
    std::optional<std::uint32_t> passkey;
    try {
      if (const unsigned long value = std::stoul(text); value <= std::numeric_limits<std::uint32_t>::max()) {
        passkey = static_cast<std::uint32_t>(value);
      }
    } catch (const std::logic_error&) {
      // std::invalid_argument / std::out_of_range: not a number.
    }
    if (!passkey.has_value()) {
      agent.cancelPending();
      return;
    }
    try {
      agent.submitPasskey(*passkey);
    } catch (const std::exception&) {
      // A failed D-Bus reply (sdbus::Error) must not escape the key handler; the pairing is cancelled instead.
      agent.cancelPending();
    }
  }

  std::unique_ptr<Label> quickText(std::string value, ColorSpec color, FontWeight weight, bool grow, float font) {
    return ui::label({
        .text = std::move(value),
        .fontSize = font,
        .fontWeight = weight,
        .fontFamily = std::string(term_frame::kFont),
        .color = std::move(color),
        .maxLines = 1,
        .ellipsize = TextEllipsize::End,
        .flexGrow = grow ? 1.0F : 0.0F,
    });
  }

  // The text after the title of a pairing row: the code to compare, what to type, or what is being asked.
  std::unique_ptr<Label> pairingDetailLabel(const BluetoothPairingRequest& req, float font) {
    if (pairingShowsCode(req.kind)) {
      return quickText(
          "  code " + pairingCodeText(req), colorSpecFromRole(ColorRole::Primary), FontWeight::Bold, false, font
      );
    }
    std::string hint = "  incoming";
    if (pairingNeedsInput(req.kind)) {
      hint = req.kind == BluetoothPairingKind::PinCode ? "  enter pin" : "  enter passkey";
    } else if (req.kind == BluetoothPairingKind::AuthorizeService) {
      hint = "  " + bluetoothServiceLabel(req.uuid);
    }
    return quickText(std::move(hint), term_frame::dimColor(), FontWeight::Normal, false, font);
  }
} // namespace

class BluetoothDeviceRow : public Collapsible {
public:
  BluetoothDeviceRow(BluetoothDeviceInfo device, BluetoothService* service, float scale)
      : m_device(std::move(device)), m_service(service) {
    setScale(scale);
    setRadius(Style::scaledRadiusMd(scale));
    setFill(colorSpecFromRole(ColorRole::Surface));
    clearBorder();

    auto header = ui::row(
        {.align = FlexAlign::Center,
         .gap = Style::spaceSm * scale,
         .padding = Style::spaceSm * scale,
         .minHeight = kRowMinHeight * scale},
        ui::glyph({
            .glyph = glyphFor(m_device.kind),
            .glyphSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        }),
        ui::label({
            .text = m_device.alias,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = m_device.connected ? FontWeight::Bold : FontWeight::Normal,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
    header->setPadding(Style::spaceSm * scale, Style::spaceMd * scale);

    auto metrics = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
    });

    if (m_device.hasBattery) {
      metrics->addChild(
          makeMetricPill("battery", percentText(static_cast<int>(m_device.batteryPercent)), scale, &m_batteryValue)
      );
    }
    if (m_device.hasRssi && bucketFor(m_device) == DeviceBucket::Available) {
      metrics->addChild(
          makeMetricPill("antenna-bars-5", percentText(signalPercentFromRssi(m_device.rssi)), scale, &m_signalValue)
      );
    }
    if (!metrics->children().empty()) {
      header->addChild(std::move(metrics));
    }

    const auto bucket = bucketFor(m_device);

    if (m_device.connecting) {
      header->addChild(
          ui::spinner({
              .out = &m_connectingSpinner,
              .color = colorSpecFromRole(ColorRole::Primary),
              .spinnerSize = Style::baseGlyphSize * scale,
          })
      );
    } else {
      ButtonVariant primaryVariant = ButtonVariant::Default;
      std::string primaryGlyph;
      switch (bucket) {
      case DeviceBucket::Connected:
        primaryVariant = ButtonVariant::Destructive;
        primaryGlyph = "plug-off";
        break;
      case DeviceBucket::Paired:
        primaryGlyph = "plug";
        break;
      case DeviceBucket::Available:
        primaryGlyph = "bluetooth";
        break;
      }
      auto primary = ui::button({
          .glyph = std::move(primaryGlyph),
          .glyphSize = Style::fontSizeBody * scale,
          .variant = primaryVariant,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusSm(scale),
          .onClick = [this]() {
            if (m_service == nullptr) {
              return;
            }
            switch (bucketFor(m_device)) {
            case DeviceBucket::Connected:
              m_service->disconnectDevice(m_device.path);
              break;
            case DeviceBucket::Paired:
              m_service->connect(m_device.path);
              break;
            case DeviceBucket::Available:
              m_service->pair(m_device.path);
              break;
            }
            PanelManager::instance().refresh();
          },
      });
      header->addChild(std::move(primary));
    }

    if (m_device.paired) {
      header->addChild(
          ui::button({
              .glyph = "trash",
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = [this]() {
                if (m_service != nullptr) {
                  m_service->forget(m_device.path);
                }
                PanelManager::instance().refresh();
              },
          })
      );
    }

    setHeader(std::move(header));

    auto bodyColumn = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .configure = [scale](Flex& col) {
          col.setPadding(
              Style::spaceXs * scale, Style::spaceMd * scale, Style::spaceSm * scale, Style::spaceMd * scale
          );
        },
    });

    if (m_device.paired) {
      bodyColumn->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
              ui::label({
                  .text = i18n::tr("control-center.bluetooth.auto-reconnect"),
                  .fontSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .flexGrow = 1.0F,
              }),
              ui::toggle({
                  .checkedImmediate = m_device.trusted,
                  .toggleSize = ToggleSize::Small,
                  .scale = scale,
                  .onChange = [this](bool checked) {
                    if (m_service != nullptr) {
                      m_service->setTrusted(m_device.path, checked);
                    }
                  },
              })
          )
      );
    }

    bodyColumn->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
            ui::label({
                .text = i18n::tr("control-center.bluetooth.address"),
                .fontSize = Style::fontSizeCaption * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .flexGrow = 1.0F,
            }),
            ui::label(
                {.text = m_device.address,
                 .fontSize = Style::fontSizeCaption * scale,
                 .color = colorSpecFromRole(ColorRole::OnSurface)}
            )
        )
    );

    setBody(std::move(bodyColumn));
  }

  void startConnectingSpinner() {
    if (m_connectingSpinner != nullptr) {
      m_connectingSpinner->start();
    }
  }

  [[nodiscard]] const std::string& devicePath() const noexcept { return m_device.path; }

  // Refresh the values that move while a scan runs. Which pills exist is fixed at
  // construction (it belongs to the list structure key); only their text is live.
  // Returns true when a value actually changed, i.e. the row needs relayout.
  bool syncLiveMetrics(const BluetoothDeviceInfo& device) {
    bool changed = false;
    if (m_batteryValue != nullptr && m_batteryValue->setText(percentText(static_cast<int>(device.batteryPercent)))) {
      changed = true;
    }
    if (m_signalValue != nullptr && m_signalValue->setText(percentText(signalPercentFromRssi(device.rssi)))) {
      changed = true;
    }
    m_device.batteryPercent = device.batteryPercent;
    m_device.rssi = device.rssi;
    return changed;
  }

private:
  BluetoothDeviceInfo m_device;
  BluetoothService* m_service = nullptr;
  Spinner* m_connectingSpinner = nullptr;
  Label* m_batteryValue = nullptr;
  Label* m_signalValue = nullptr;
};

BluetoothTab::BluetoothTab(BluetoothService* service, BluetoothAgent* agent, bool quick)
    : m_service(service), m_agent(agent), m_quick(quick) {}

BluetoothTab::~BluetoothTab() {
  *m_alive = false;
  // m_service outlives every tab (Application declares it before m_panelManager), so this is safe at teardown.
  // Also covers a quick-panel grace timer that never got to fire.
  stopRequestedDiscovery();
}

std::unique_ptr<Flex> BluetoothTab::create() {
  if (m_quick) {
    return createQuick();
  }
  const float scale = contentScale();

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto pairingCard = ui::column({
      .out = &m_pairingCard,
      .visible = false,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) { applySectionCardStyle(card, scale, opacity); },
  });

  pairingCard->addChild(
      ui::label({
          .out = &m_pairingTitle,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );

  pairingCard->addChild(
      ui::label({
          .out = &m_pairingDetail,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );

  pairingCard->addChild(
      ui::label({
          .out = &m_pairingCode,
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::Primary),
      })
  );

  auto pairingInputRow = ui::row(
      {.out = &m_pairingInputRow, .align = FlexAlign::Center, .gap = Style::spaceSm * scale, .visible = false},
      ui::input({
          .out = &m_pairingInput,
          .placeholder = i18n::tr("control-center.bluetooth.enter-code"),
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0F,
          .onSubmit = [this](const std::string& value) { submitPairingInput(value); },
      })
  );
  pairingCard->addChild(std::move(pairingInputRow));

  auto pairingButtonRow = ui::row(
      {.out = &m_pairingButtonRow, .align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .out = &m_pairingAccept,
          .text = i18n::tr("control-center.bluetooth.accept"),
          .variant = ButtonVariant::Default,
          .onClick = [this]() { acceptPairing(); },
      }),
      ui::button({
          .out = &m_pairingTrustAndAllow,
          .text = i18n::tr("control-center.bluetooth.trust-and-allow"),
          .variant = ButtonVariant::Default,
          .onClick = [this]() { trustAndAllowPairing(); },
      }),
      ui::button({
          .out = &m_pairingReject,
          .text = i18n::tr("control-center.bluetooth.reject"),
          .variant = ButtonVariant::Ghost,
          .onClick = [this]() { rejectPairing(); },
      })
  );
  pairingCard->addChild(std::move(pairingButtonRow));

  tab->addChild(std::move(pairingCard));

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
  m_list = listScroll->content();
  m_list->setDirection(FlexDirection::Vertical);
  m_list->setAlign(FlexAlign::Stretch);
  m_list->setGap(Style::spaceMd * scale);

  tab->addChild(std::move(listScroll));
  return tab;
}

std::unique_ptr<Flex> BluetoothTab::createHeaderActions() { return nullptr; }

void BluetoothTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_quick) {
    doLayoutQuick(renderer, contentWidth, bodyHeight);
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  syncPairingCard();
  rebuildDeviceList(renderer);
  syncDeviceRows();
  syncHeader();
  m_rootLayout->layout(renderer);
}

void BluetoothTab::doUpdate(Renderer& renderer) {
  if (m_quick) {
    syncQuick(renderer, false);
    return;
  }
  syncDiscoveryLease();
  syncPairingCard();
  rebuildDeviceList(renderer);
  // A metric pill's text changes its width, so the list has to be laid out again.
  if (syncDeviceRows() && m_list != nullptr) {
    m_list->layout(renderer);
  }
  syncHeader();
}

void BluetoothTab::setActive(bool active) {
  if (!active) {
    // The tab object outlives the panel: a reopen starts without an open menu or a remembered selection, unless
    // it only changes the height.
    if (!quickPanelResizing()) {
      m_qMenuFor.clear();
      m_qSelected.clear();
    }
    m_qArmed = nullptr;
    m_qArmTimer.stop();
    m_qListKey.clear();
    deferDiscoveryStop();
    return;
  }
  // A reopen (quick panel resized) keeps the scan it interrupted running for a fresh window.
  if (m_quick && m_discoveryLease != DiscoveryLease::None) {
    m_discoveryTimer.start(kDiscoveryTimeout, [this]() { stopRequestedDiscovery(); });
  }
}

void BluetoothTab::onClose() {
  deferDiscoveryStop();
  if (m_quick && quickPanelResizing()) {
    // The panel reopens at another height: take the half-typed code along and keep the selection in view.
    if (m_pairingInput != nullptr) {
      m_qResumeTyped = m_pairingInput->value();
      m_qResumeInput = true;
    }
    m_qScrollToSelected = true;
  } else {
    forgetQuick();
  }
  resetQuick();
  m_rootLayout = nullptr;
  m_pairingCard = nullptr;
  m_pairingTitle = nullptr;
  m_pairingDetail = nullptr;
  m_pairingCode = nullptr;
  m_pairingInputRow = nullptr;
  m_pairingInput = nullptr;
  m_pairingButtonRow = nullptr;
  m_pairingAccept = nullptr;
  m_pairingTrustAndAllow = nullptr;
  m_pairingReject = nullptr;
  m_listScroll = nullptr;
  m_list = nullptr;
  m_powerToggle = nullptr;
  m_discoverableToggle = nullptr;
  m_rescanButton = nullptr;
  m_scanSpinner = nullptr;
  m_deviceRows.clear();
  m_lastStructureKey.clear();
  m_lastListWidth = -1.0F;
}

void BluetoothTab::syncDiscoveryLease() {
  if (m_discoveryLease == DiscoveryLease::None || m_service == nullptr) {
    return;
  }

  const BluetoothState& s = m_service->state();
  if (!s.adapterPresent || !s.powered) {
    // A powered-down adapter cannot be discovering, so BlueZ already dropped the session.
    releaseDiscoveryLease();
    return;
  }
  if (m_discoveryLease == DiscoveryLease::Pending) {
    // StartDiscovery is async; the lease is only confirmed once BlueZ reports Discovering.
    if (s.discovering) {
      m_discoveryLease = DiscoveryLease::Active;
    }
    return;
  }
  if (!s.discovering) {
    // Discovery ended outside the tab: drop the lease so the next Rescan starts a new one.
    releaseDiscoveryLease();
  }
}

void BluetoothTab::releaseDiscoveryLease() {
  m_discoveryLease = DiscoveryLease::None;
  m_discoveryTimer.stop();
}

void BluetoothTab::stopRequestedDiscovery() {
  if (m_discoveryLease == DiscoveryLease::None) {
    return;
  }

  releaseDiscoveryLease();
  if (m_service != nullptr) {
    m_service->stopDiscovery();
  }
}

void BluetoothTab::syncHeader() {
  if (m_service == nullptr) {
    return;
  }
  const BluetoothState& s = m_service->state();
  if (m_powerToggle != nullptr) {
    m_powerToggle->setChecked(s.powered);
    m_powerToggle->setEnabled(s.adapterPresent);
  }
  if (m_discoverableToggle != nullptr) {
    m_discoverableToggle->setChecked(s.discoverable);
    m_discoverableToggle->setEnabled(s.adapterPresent && s.powered);
  }
  if (m_scanSpinner != nullptr) {
    const bool spinnerVisible = s.discovering && s.powered && s.adapterPresent;
    m_scanSpinner->setVisible(spinnerVisible);
    if (spinnerVisible && !m_scanSpinner->spinning()) {
      m_scanSpinner->start();
    } else if (!spinnerVisible && m_scanSpinner->spinning()) {
      m_scanSpinner->stop();
    }
  }
  if (m_rescanButton != nullptr) {
    m_rescanButton->setEnabled(s.adapterPresent && s.powered);
  }
}

void BluetoothTab::syncPairingCard() {
  if (m_pairingCard == nullptr) {
    return;
  }
  const bool hasPending = m_agent != nullptr && m_agent->hasPendingRequest();
  const auto show = [this](auto* node, bool visible) {
    if (node == nullptr) {
      return;
    }
    if (m_quick) {
      setShown(node, visible);
    } else {
      node->setVisible(visible);
    }
  };
  show(m_pairingCard, hasPending);
  if (!hasPending) {
    return;
  }
  const auto req = m_agent->pendingRequest();
  std::string alias = req.devicePath;
  if (m_service != nullptr) {
    for (const auto& d : m_service->devices()) {
      if (d.path == req.devicePath && !d.alias.empty()) {
        alias = d.alias;
        break;
      }
    }
  }

  if (m_pairingTitle != nullptr) {
    m_pairingTitle->setText(i18n::tr("control-center.bluetooth.pair-title", "device", alias));
  }
  const bool needsInput = pairingNeedsInput(req.kind);
  const bool showsCode = pairingShowsCode(req.kind);

  if (m_pairingDetail != nullptr) {
    switch (req.kind) {
    case BluetoothPairingKind::Confirm:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.confirm"));
      break;
    case BluetoothPairingKind::Authorize:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.authorize"));
      break;
    case BluetoothPairingKind::AuthorizeService:
      // The {uuid} placeholder predates the lookup and is kept for the existing locale
      // catalogs; it now carries the SIG service name, falling back to the raw UUID.
      m_pairingDetail->setText(
          i18n::tr("control-center.bluetooth.pairing-detail.authorize-service", "uuid", bluetoothServiceLabel(req.uuid))
      );
      break;
    case BluetoothPairingKind::DisplayPinCode:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.display-pin"));
      break;
    case BluetoothPairingKind::DisplayPasskey:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.display-passkey"));
      break;
    case BluetoothPairingKind::PinCode:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.pin-code"));
      break;
    case BluetoothPairingKind::Passkey:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.passkey"));
      break;
    case BluetoothPairingKind::None:
      break;
    }
  }
  if (m_pairingCode != nullptr) {
    show(m_pairingCode, showsCode);
    if (showsCode) {
      m_pairingCode->setText(pairingCodeText(req));
    }
  }
  if (m_pairingInputRow != nullptr) {
    show(m_pairingInputRow, needsInput);
  }
  if (m_pairingAccept != nullptr) {
    // DisplayPasskey has nothing to accept: the code is entered on the device and there
    // is no Agent1 reply. Show only the Reject action so Accept cannot clear the card.
    show(m_pairingAccept, req.kind != BluetoothPairingKind::DisplayPasskey);
  }
  if (m_pairingTrustAndAllow != nullptr) {
    // Only service authorization is suppressed by the Trusted flag, so trusting the device
    // would not stop the other request kinds from prompting again.
    show(m_pairingTrustAndAllow, req.kind == BluetoothPairingKind::AuthorizeService);
  }
}

// Identity of the built list: which rows exist, in which order, and which controls
// each carries. Live metric values (battery percent, RSSI) are absent by design —
// they refresh in place through syncDeviceRows(), so an advertisement no longer tears
// down the list. Devices arrive sorted, so a change in row order changes the key.
std::string BluetoothTab::structureKey(const std::vector<BluetoothDeviceInfo>& devices) const {
  if (m_service == nullptr) {
    return "empty";
  }
  const auto& s = m_service->state();
  std::string key;
  key += s.adapterPresent ? '1' : '0';
  key += s.powered ? '1' : '0';
  key += s.rfkillSoftBlocked ? '1' : '0';
  key += s.discovering ? '1' : '0';
  key.push_back('|');
  for (const auto& d : devices) {
    key += d.path;
    key.push_back(':');
    key += d.alias;
    key.push_back(':');
    key += std::to_string(static_cast<int>(d.kind));
    key.push_back(':');
    key += d.paired ? '1' : '0';
    key += d.trusted ? '1' : '0';
    key += d.connected ? '1' : '0';
    key += d.connecting ? '1' : '0';
    key += d.hasBattery ? '1' : '0';
    key += d.hasRssi ? '1' : '0';
    key.push_back('\n');
  }
  return key;
}

bool BluetoothTab::syncDeviceRows() {
  if (m_service == nullptr || m_deviceRows.empty()) {
    return false;
  }
  bool changed = false;
  for (const auto& d : m_service->devices()) {
    const auto it = m_deviceRows.find(d.path);
    if (it != m_deviceRows.end() && it->second->syncLiveMetrics(d)) {
      changed = true;
    }
  }
  return changed;
}

void BluetoothTab::rebuildDeviceList(Renderer& renderer) {
  uiAssertNotRendering("BluetoothTab::rebuildDeviceList");
  if (m_list == nullptr || m_listScroll == nullptr) {
    return;
  }
  const float listWidth = m_listScroll->contentViewportWidth();
  if (listWidth <= 0.0F) {
    return;
  }
  std::vector<BluetoothDeviceInfo> devices;
  if (m_service != nullptr) {
    devices = sortedDevices(m_service->devices());
  }
  const std::string nextKey = structureKey(devices);
  const bool structureChanged = nextKey != m_lastStructureKey;
  if (listWidth == m_lastListWidth && !structureChanged) {
    return;
  }
  m_lastListWidth = listWidth;
  m_lastStructureKey = nextKey;

  if (!structureChanged) {
    m_list->layout(renderer);
    return;
  }

  const float scale = contentScale();

  std::unordered_set<std::string> expandedPaths;
  for (const auto& [path, row] : m_deviceRows) {
    if (row->expanded()) {
      expandedPaths.insert(path);
    }
  }

  m_powerToggle = nullptr;
  m_discoverableToggle = nullptr;
  m_scanSpinner = nullptr;
  m_rescanButton = nullptr;
  m_deviceRows.clear();

  while (!m_list->children().empty()) {
    m_list->removeChild(m_list->children().front().get());
  }

  if (m_service == nullptr) {
    m_list->addChild(
        ui::label({
            .text = i18n::tr("control-center.bluetooth.unavailable"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    m_list->layout(renderer);
    return;
  }

  const auto& s = m_service->state();

  // Adapter card: power + discoverable toggles
  {
    auto adapterCard = ui::column({
        .configure = [scale, opacity = panelCardOpacity()](Flex& card) { applySectionCardStyle(card, scale, opacity); },
    });

    auto powerRow = ui::row(
        {.align = FlexAlign::Center,
         .gap = Style::spaceSm * scale,
         .minHeight = Style::controlHeightSm * scale,
         .maxHeight = Style::controlHeightSm * scale},
        ui::label({
            .text = i18n::tr("control-center.bluetooth.bluetooth"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );

    powerRow->addChild(
        ui::spinner({
            .out = &m_scanSpinner,
            .color = colorSpecFromRole(ColorRole::Primary),
            .spinnerSize = Style::baseGlyphSize * scale,
            .visible = false,
        })
    );

    powerRow->addChild(
        ui::button({
            .out = &m_rescanButton,
            .glyph = "refresh",
            .glyphSize = Style::baseGlyphSize * scale,
            .enabled = s.adapterPresent && s.powered,
            .variant = ButtonVariant::Ghost,
            .minHeight = Style::fontSizeBody * scale,
            .padding = Style::spaceXs * scale,
            .radius = Style::scaledRadiusSm(scale),
            .onClick = [this]() { requestScan(); },
        })
    );

    powerRow->addChild(
        ui::toggle({
            .out = &m_powerToggle,
            .checkedImmediate = s.powered,
            .enabled = s.adapterPresent,
            .toggleSize = ToggleSize::Medium,
            .scale = scale,
            .onChange = [this](bool checked) {
              if (m_service != nullptr) {
                m_service->setPowered(checked);
              }
            },
        })
    );

    adapterCard->addChild(std::move(powerRow));

    auto visibleRow = ui::row(
        {.align = FlexAlign::Center,
         .gap = Style::spaceSm * scale,
         .minHeight = Style::controlHeightSm * scale,
         .maxHeight = Style::controlHeightSm * scale},
        ui::label({
            .text = i18n::tr("control-center.bluetooth.visible"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        }),
        ui::toggle({
            .out = &m_discoverableToggle,
            .checkedImmediate = s.discoverable,
            .enabled = s.adapterPresent && s.powered,
            .toggleSize = ToggleSize::Medium,
            .scale = scale,
            .onChange = [this](bool checked) {
              if (m_service != nullptr) {
                m_service->setDiscoverable(checked);
              }
            },
        })
    );

    adapterCard->addChild(std::move(visibleRow));

    m_list->addChild(std::move(adapterCard));
  }

  if (!s.powered) {
    m_list->addChild(
        ui::label({
            .text = s.rfkillSoftBlocked ? i18n::tr("control-center.bluetooth.rfkill-blocked")
                                        : i18n::tr("control-center.bluetooth.off"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    m_list->layout(renderer);
    return;
  }

  if (devices.empty()) {
    m_list->addChild(
        ui::label({
            .text = i18n::tr("control-center.bluetooth.no-devices"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    m_list->layout(renderer);
    return;
  }

  Flex* bucketCard = nullptr;
  DeviceBucket currentBucket = DeviceBucket::Connected;
  bool first = true;
  for (const auto& device : devices) {
    const auto bucket = bucketFor(device);
    if (first || bucket != currentBucket) {
      std::string sectionText;
      switch (bucket) {
      case DeviceBucket::Connected:
        sectionText = i18n::tr("control-center.bluetooth.sections.connected");
        break;
      case DeviceBucket::Paired:
        sectionText = i18n::tr("control-center.bluetooth.sections.paired");
        break;
      case DeviceBucket::Available:
        sectionText = i18n::tr("control-center.bluetooth.sections.available");
        break;
      }
      auto card = ui::column({
          .configure = [scale, opacity = panelCardOpacity()](Flex& c) { applySectionCardStyle(c, scale, opacity); },
      });
      card->addChild(makeCardHeaderRow(sectionText, scale));
      bucketCard = card.get();
      m_list->addChild(std::move(card));
      currentBucket = bucket;
      first = false;
    }
    auto row = std::make_unique<BluetoothDeviceRow>(device, m_service, scale);
    auto* rowPtr = row.get();
    bucketCard->addChild(std::move(row));
    rowPtr->setExpandedImmediate(expandedPaths.contains(device.path));
    rowPtr->startConnectingSpinner();
    m_deviceRows.emplace(rowPtr->devicePath(), rowPtr);
  }
  m_list->layout(renderer);
}

void BluetoothTab::onPanelCardOpacityChanged(float opacity) {
  if (m_pairingInput != nullptr) {
    m_pairingInput->setSurfaceOpacity(opacity);
  }
}

// ---- shared actions -------------------------------------------------------------------------------------------

void BluetoothTab::requestScan() {
  if (m_service == nullptr) {
    return;
  }
  const BluetoothState& s = m_service->state();
  if (!s.adapterPresent || !s.powered) {
    return;
  }
  // Repeated requests renew the scan window instead of restarting discovery.
  if (m_discoveryLease == DiscoveryLease::None) {
    m_discoveryLease = DiscoveryLease::Pending;
    m_service->startDiscovery();
  }
  m_discoveryTimer.start(kDiscoveryTimeout, [this]() { stopRequestedDiscovery(); });
}

void BluetoothTab::submitPairingInput(const std::string& value) {
  if (m_agent == nullptr) {
    return;
  }
  const auto req = m_agent->pendingRequest();
  if (req.kind == BluetoothPairingKind::PinCode) {
    m_agent->submitPin(value);
  } else if (req.kind == BluetoothPairingKind::Passkey) {
    submitPasskeyText(*m_agent, value);
  }
  PanelManager::instance().refresh();
}

void BluetoothTab::acceptPairing() {
  if (m_agent == nullptr) {
    return;
  }
  const auto req = m_agent->pendingRequest();
  switch (req.kind) {
  case BluetoothPairingKind::Confirm:
  case BluetoothPairingKind::Authorize:
  case BluetoothPairingKind::AuthorizeService:
  case BluetoothPairingKind::DisplayPinCode:
    m_agent->acceptConfirm();
    break;
  case BluetoothPairingKind::PinCode:
    if (m_pairingInput != nullptr) {
      m_agent->submitPin(m_pairingInput->value());
    }
    break;
  case BluetoothPairingKind::Passkey:
    if (m_pairingInput != nullptr) {
      submitPasskeyText(*m_agent, m_pairingInput->value());
    }
    break;
  case BluetoothPairingKind::DisplayPasskey:
    // Informational only: the code is typed on the device, and there is no
    // Agent1 reply to send. Do not fall through to the canceling default.
    break;
  default:
    m_agent->cancelPending();
    break;
  }
  PanelManager::instance().refresh();
}

void BluetoothTab::trustAndAllowPairing() {
  if (m_agent == nullptr) {
    return;
  }
  // Trusting the device is what stops BlueZ from asking again: it
  // auto-authorizes services for trusted devices without calling the agent.
  const auto req = m_agent->pendingRequest();
  if (m_service != nullptr && !req.devicePath.empty()) {
    m_service->setTrusted(req.devicePath, true);
  }
  m_agent->acceptConfirm();
  PanelManager::instance().refresh();
}

void BluetoothTab::rejectPairing() {
  if (m_agent != nullptr) {
    m_agent->rejectConfirm();
  }
  PanelManager::instance().refresh();
}

// A quick panel that closes, or reopens at a new height, keeps its scan alive for a short grace window so a
// resize mid-scan does not cut the scan off; the control-center tab stops it at once.
void BluetoothTab::deferDiscoveryStop() {
  if (!m_quick) {
    stopRequestedDiscovery();
    return;
  }
  if (m_discoveryLease == DiscoveryLease::None) {
    return;
  }
  m_discoveryTimer.start(kQuickDiscoveryGrace, [this]() { stopRequestedDiscovery(); });
}

// ---- quick (QuickPanel) mode ----------------------------------------------------------------------------------

std::optional<BluetoothDeviceInfo> BluetoothTab::findDevice(const std::string& path) const {
  if (m_service == nullptr) {
    return std::nullopt;
  }
  for (const auto& d : m_service->devices()) {
    if (d.path == path) {
      return d;
    }
  }
  return std::nullopt;
}

bool BluetoothTab::textInputFocused() const {
  const InputDispatcher* dispatcher = inputDispatcher();
  if (dispatcher == nullptr) {
    return false;
  }
  const InputArea* area = dispatcher->focusedArea();
  return area != nullptr && area->textInputClient() != nullptr;
}

std::optional<std::string> BluetoothTab::quickFailure(const std::string& key) const {
  const auto now = std::chrono::steady_clock::now();
  const BluetoothFailure* best = nullptr;
  const auto consider = [&](const BluetoothFailure* failure) {
    if (failure != nullptr && now - failure->at < kQuickFailureTimeout && (best == nullptr || failure->at > best->at)) {
      best = failure;
    }
  };
  if (m_service != nullptr) {
    consider(m_service->failure(key));
  }
  if (const auto it = m_qLocalFailures.find(key); it != m_qLocalFailures.end()) {
    consider(&it->second);
  }
  if (best == nullptr) {
    return std::nullopt;
  }
  return best->reason;
}

void BluetoothTab::setLocalFailure(const std::string& key, std::string reason) {
  m_qLocalFailures[key] = BluetoothFailure{.reason = std::move(reason), .at = std::chrono::steady_clock::now()};
  requestQuickRefresh();
}

void BluetoothTab::clearQuickFailure(const std::string& key) {
  m_qLocalFailures.erase(key);
  if (m_service != nullptr) {
    m_service->clearFailure(key);
  }
}

const BluetoothTab::QuickProfiles* BluetoothTab::quickProfilesFor(const BluetoothDeviceInfo& device) const {
  const auto it = m_qProfiles.find(upperAscii(device.address));
  return it == m_qProfiles.end() ? nullptr : &it->second;
}

BluetoothTab::QuickPlan BluetoothTab::quickPlan() const {
  QuickPlan plan;
  int listed = 0;
  int avail = 0;
  bool menuListed = false;
  bool menuAvail = false;
  bool powered = false;
  bool discovering = false;
  if (m_service != nullptr) {
    const BluetoothState& s = m_service->state();
    powered = s.adapterPresent && s.powered;
    discovering = powered && s.discovering;
    if (powered) {
      for (const auto& d : m_service->devices()) {
        const bool isAvail = bucketFor(d) == DeviceBucket::Available;
        ++(isAvail ? avail : listed);
        if (d.path == m_qMenuFor) {
          (isAvail ? menuAvail : menuListed) = true;
        }
      }
    }
  }
  if (powered && m_agent != nullptr && m_agent->hasPendingRequest()) {
    plan.pairing = true;
    plan.pairingInput = pairingNeedsInput(m_agent->pendingRequest().kind);
  }
  plan.devLines = std::clamp(listed + (menuListed ? 1 : 0), 1, kQuickMaxDevLines);
  plan.availBox = powered && (discovering || avail > 0 || plan.pairing);
  plan.availLines = std::clamp(avail + (menuAvail ? 1 : 0) + (plan.pairing ? 1 : 0), 1, kQuickMaxAvailLines);
  plan.key = std::to_string(plan.devLines)
      + ':'
      + (plan.availBox ? '1' : '0')
      + ':'
      + std::to_string(plan.availLines)
      + ':'
      + (plan.pairingInput ? '1' : '0');
  return plan;
}

float BluetoothTab::quickPanelHeight(float width, float fallback) const {
  (void)width;
  if (!m_quick) {
    return fallback;
  }
  const QuickPlan plan = quickPlan();
  float total = kQuickAdapterHeight + quickListBoxHeight(plan.devLines);
  int boxes = 2; // adapter + devices
  if (plan.availBox) {
    total += quickListBoxHeight(plan.availLines) + (plan.pairingInput ? kQuickInputRow : 0.0F);
    ++boxes;
  }
  return total + kQuickGap * static_cast<float>(boxes - 1);
}

std::string BluetoothTab::quickPanelHints() const {
  if (m_agent != nullptr && m_agent->hasPendingRequest()) {
    const auto req = m_agent->pendingRequest();
    if (pairingNeedsInput(req.kind)) {
      return "⏎ submit";
    }
    if (req.kind == BluetoothPairingKind::DisplayPasskey) {
      return "r reject";
    }
    return req.kind == BluetoothPairingKind::AuthorizeService ? "a accept  t trust  r reject" : "a accept  r reject";
  }
  const char* enter = "connect";
  if (m_qSelected == kQuickPower || m_qSelected == kQuickDiscoverable) {
    enter = "toggle";
  } else if (
      const auto device = findDevice(m_qSelected); device.has_value() && bucketFor(*device) == DeviceBucket::Available
  ) {
    enter = "pair";
  }
  return std::string("↑↓ select  ⏎ ") + enter + "  a actions  r scan";
}

std::string BluetoothTab::quickPanelStatus() const {
  if (m_service == nullptr) {
    return "n/a";
  }
  const BluetoothState& s = m_service->state();
  if (!s.adapterPresent) {
    return "no adapter";
  }
  if (!s.powered) {
    return s.rfkillSoftBlocked ? "blocked" : "";
  }
  return s.discovering ? "scanning" : "";
}

// Drops everything that points into the panel's widgets, so the next create() builds from scratch.
void BluetoothTab::resetQuick() {
  m_qArmTimer.stop();
  m_qFailTimer.stop();
  m_qArmed = nullptr;
  m_qPower = term_frame::ToggleRow{};
  m_qDiscoverable = term_frame::ToggleRow{};
  m_qPairKeys = {};
  m_qMenuKeys = {};
  m_qPowerGutter = nullptr;
  m_qDiscoverableGutter = nullptr;
  m_qPairGutter = nullptr;
  m_qPairNode = nullptr;
  m_pairingInput = nullptr;
  m_qDevBox = nullptr;
  m_qAvailBox = nullptr;
  m_qDevScroll = nullptr;
  m_qAvailScroll = nullptr;
  m_qDevList = nullptr;
  m_qAvailList = nullptr;
  m_qAvailChip = nullptr;
  m_qRows.clear();
  m_qOrder.clear();
  m_qConnectedKey.clear();
  m_qFocusInput = false;
  m_qPlanKey.clear();
  m_qListKey.clear();
  m_qHeightKey.clear();
}

// What the user was in the middle of; kept across a resize reopen, dropped when the panel really closes.
void BluetoothTab::forgetQuick() {
  m_qMenuFor.clear();
  m_qSelected.clear();
  m_qScrollToSelected = false;
  m_qHadInput = false;
  m_qResumeTyped.clear();
  m_qResumeInput = false;
}

std::unique_ptr<Flex> BluetoothTab::makeQuickLine(float scale, Label** gutterOut, Flex** nodeOut) {
  const float font = term_frame::kBtopFontSize * scale;
  return ui::row(
      {.out = nodeOut,
       .align = FlexAlign::Center,
       .minHeight = kQuickRow * scale,
       .maxHeight = kQuickRow * scale,
       .fillWidth = true},
      ui::label({
          .out = gutterOut,
          .text = " ",
          .fontSize = font,
          .fontFamily = std::string(term_frame::kFont),
          .color = colorSpecFromRole(ColorRole::Primary),
          .minWidth = term_frame::monoWidth(kQuickGutterChars, font),
          .maxLines = 1,
      })
  );
}

std::unique_ptr<Flex> BluetoothTab::createQuick() {
  const float scale = contentScale();
  resetQuick();
  m_qHeightKey = quickPlan().key;

  auto root = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = kQuickGap * scale,
  });

  // ¹adapter: power and discoverable, selectable like every other row.
  {
    auto box = quickBox("¹", "adapter", scale);
    const auto addToggle = [&](Label** gutterOut, std::string id, std::string name, term_frame::ToggleRow& row,
                               std::function<void()> action) {
      auto line = makeQuickLine(scale, gutterOut);
      auto button =
          term_frame::toggleRow(std::move(name), scale, row, [this, id = std::move(id), action = std::move(action)]() {
            quickSelectId(id);
            action();
          });
      button->setFlexGrow(1.0F);
      line->addChild(std::move(button));
      box->addChild(std::move(line));
    };
    addToggle(&m_qPowerGutter, kQuickPower, "bluetooth", m_qPower, [this]() { quickTogglePower(); });
    addToggle(&m_qDiscoverableGutter, kQuickDiscoverable, "discoverable", m_qDiscoverable, [this]() {
      quickToggleDiscoverable();
    });
    root->addChild(std::move(box));
  }

  const auto makeList = [scale](ScrollView** scrollOut, Flex** listOut) {
    auto scroll = ui::scrollView({
        .out = scrollOut,
        .contentScale = scale,
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0F,
        .viewportPaddingV = 0.0F,
        .configure = [](ScrollView& scrollView) {
          scrollView.clearFill();
          scrollView.clearBorder();
        },
    });
    Flex* content = scroll->content();
    content->setDirection(FlexDirection::Vertical);
    content->setAlign(FlexAlign::Stretch);
    content->setGap(0.0F);
    *listOut = content;
    return scroll;
  };

  // ²devices: connected first, then paired.
  {
    auto box = quickBox("²", "devices", scale, &m_qDevBox);
    box->addChild(makeList(&m_qDevScroll, &m_qDevList));
    root->addChild(std::move(box));
  }

  // ³available: scan results and the pairing prompt.
  {
    auto box = quickBox("³", "available", scale, &m_qAvailBox, &m_qAvailChip);
    box->addChild(makeList(&m_qAvailScroll, &m_qAvailList));
    setShown(m_qAvailBox, false);
    root->addChild(std::move(box));
  }
  return root;
}

void BluetoothTab::doLayoutQuick(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  syncQuick(renderer, true);
  if (m_rootLayout != nullptr) {
    m_rootLayout->layout(renderer);
  }
}

void BluetoothTab::quickTogglePower() {
  if (m_service == nullptr || !m_service->state().adapterPresent) {
    return;
  }
  clearQuickFailure("adapter");
  m_service->setPowered(!m_service->state().powered);
  PanelManager::instance().refresh();
}

void BluetoothTab::quickToggleDiscoverable() {
  if (m_service == nullptr) {
    return;
  }
  const BluetoothState& s = m_service->state();
  if (!s.adapterPresent || !s.powered) {
    setLocalFailure("adapter.discoverable", "bluetooth is off");
    return;
  }
  clearQuickFailure("adapter.discoverable");
  m_service->setDiscoverable(!s.discoverable);
  PanelManager::instance().refresh();
}

void BluetoothTab::quickActivate(const std::string& path) {
  const auto device = findDevice(path);
  if (!device.has_value() || m_service == nullptr || device->connecting) {
    return;
  }
  clearQuickFailure(path);
  bool started = true;
  switch (bucketFor(*device)) {
  case DeviceBucket::Connected:
    started = m_service->disconnectDevice(path);
    break;
  case DeviceBucket::Paired:
    started = m_service->connect(path);
    break;
  case DeviceBucket::Available:
    started = m_service->pair(path);
    break;
  }
  if (!started) {
    setLocalFailure(path, "request failed");
  }
  PanelManager::instance().refresh();
}

void BluetoothTab::quickActivateSelected() {
  if (m_qSelected.empty()) {
    return;
  }
  if (m_qSelected == kQuickPower) {
    quickTogglePower();
  } else if (m_qSelected == kQuickDiscoverable) {
    quickToggleDiscoverable();
  } else if (m_qSelected == kQuickPair) {
    acceptPairing();
  } else {
    quickActivate(m_qSelected);
  }
}

void BluetoothTab::quickToggleTrust(const std::string& path) {
  const auto device = findDevice(path);
  if (!device.has_value() || m_service == nullptr) {
    return;
  }
  clearQuickFailure(path);
  m_service->setTrusted(path, !device->trusted);
  PanelManager::instance().refresh();
}

void BluetoothTab::quickForget(const std::string& path) {
  if (m_service == nullptr) {
    return;
  }
  clearQuickFailure(path);
  m_service->forget(path);
  if (m_qMenuFor == path) {
    m_qMenuFor.clear();
  }
  PanelManager::instance().refresh();
}

void BluetoothTab::quickToggleMenu() {
  const auto device = findDevice(m_qSelected);
  if (!device.has_value()) {
    return;
  }
  if (m_qMenuFor == m_qSelected) {
    m_qMenuFor.clear();
  } else {
    m_qMenuFor = m_qSelected;
    if (isHeadphones(device->kind) && device->connected) {
      refreshProfiles();
    }
  }
  m_qScrollToSelected = true;
  PanelManager::instance().refresh();
}

void BluetoothTab::quickSelectId(const std::string& id) {
  m_qSelected = id;
  if (m_qMenuFor != id) {
    m_qMenuFor.clear();
  }
  m_qScrollToSelected = true;
  requestQuickRefresh();
}

void BluetoothTab::quickSelect(int delta) {
  if (m_qOrder.empty()) {
    return;
  }
  const int last = static_cast<int>(m_qOrder.size()) - 1;
  const auto it = std::ranges::find(m_qOrder, m_qSelected);
  int index = 0;
  if (it == m_qOrder.end()) {
    index = delta > 0 ? 0 : last;
  } else {
    index = std::clamp(static_cast<int>(it - m_qOrder.begin()) + delta, 0, last);
  }
  quickSelectId(m_qOrder[static_cast<std::size_t>(index)]);
}

// First press: the keyword turns into an accent block asking "confirm?". Second press runs the action.
void BluetoothTab::quickPressConfirmed(term_frame::Keyword& keyword, const std::function<void()>& run) {
  if (m_qArmed != &keyword) {
    quickClearArmed();
    m_qArmed = &keyword;
    keyword.block = true;
    keyword.blockText = "confirm?";
    term_frame::refreshKeyword(keyword);
    m_qArmTimer.start(kQuickArmTimeout, [this]() {
      quickClearArmed();
      requestQuickRefresh();
    });
    requestQuickRefresh();
    return;
  }
  m_qArmTimer.stop();
  quickClearArmed();
  run();
}

void BluetoothTab::quickClearArmed() {
  if (m_qArmed != nullptr) {
    m_qArmed->block = false;
    term_frame::refreshKeyword(*m_qArmed);
    m_qArmed = nullptr;
  }
}

// Runs the keyword of the open action menu whose highlighted letter is `sym`.
bool BluetoothTab::quickPressMenuKey(std::uint32_t sym) {
  if (m_qMenuFor.empty() || m_qMenuFor != m_qSelected) {
    return false;
  }
  for (term_frame::Keyword& keyword : m_qMenuKeys) {
    if (keyword.button == nullptr || !keyword.activate || keyword.hot >= keyword.word.size()) {
      continue;
    }
    const auto letter = static_cast<std::uint32_t>(std::tolower(static_cast<unsigned char>(keyword.word[keyword.hot])));
    if (letter == sym) {
      keyword.activate();
      return true;
    }
  }
  return false;
}

// `r`: scan for devices, or say why that is not possible.
void BluetoothTab::quickRescan() {
  const bool ready = m_service != nullptr && m_service->state().adapterPresent && m_service->state().powered;
  if (!ready) {
    setLocalFailure("adapter", "bluetooth is off");
    return;
  }
  clearQuickFailure("adapter");
  requestScan();
  PanelManager::instance().refresh();
}

// Keys while a pairing prompt is up: it owns the keyboard, except that a code field takes every key itself.
bool BluetoothTab::handlePairingKey(const BluetoothPairingRequest& req, std::uint32_t sym) {
  if (pairingNeedsInput(req.kind)) {
    return false;
  }
  switch (sym) {
  case XKB_KEY_a:
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
    if (req.kind == BluetoothPairingKind::DisplayPasskey) {
      return false;
    }
    acceptPairing();
    return true;
  case XKB_KEY_t:
    if (req.kind != BluetoothPairingKind::AuthorizeService) {
      return false;
    }
    trustAndAllowPairing();
    return true;
  case XKB_KEY_r:
    rejectPairing();
    return true;
  default:
    return false;
  }
}

bool BluetoothTab::handleQuickPanelKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!m_quick || !pressed || preedit || modifiers != 0) {
    return false;
  }
  // A focused text field (the PIN input) owns every key: letters type and trigger no shortcuts.
  if (textInputFocused()) {
    return false;
  }
  if (m_agent != nullptr && m_agent->hasPendingRequest()) {
    return handlePairingKey(m_agent->pendingRequest(), sym);
  }

  switch (sym) {
  case XKB_KEY_Up:
  case XKB_KEY_k:
    quickSelect(-1);
    return true;
  case XKB_KEY_Down:
  case XKB_KEY_j:
    quickSelect(1);
    return true;
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
    if (m_qSelected.empty()) {
      return false;
    }
    quickActivateSelected();
    return true;
  case XKB_KEY_a:
    quickToggleMenu();
    return true;
  case XKB_KEY_r:
    quickRescan();
    return true;
  default:
    return quickPressMenuKey(sym);
  }
}

// The action menu line under a device row: connect/disconnect/pair, trust, profile (headphones) and forget.
std::unique_ptr<Flex> BluetoothTab::buildQuickMenu(const BluetoothDeviceInfo& device, float scale) {
  const float font = Style::fontSizeCaption * scale;
  const std::string path = device.path;
  const auto bucket = bucketFor(device);
  const bool connected = bucket == DeviceBucket::Connected;
  const QuickProfiles* profiles = isHeadphones(device.kind) && connected ? quickProfilesFor(device) : nullptr;

  auto row = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceMd * scale,
      .paddingH = term_frame::monoWidth(kQuickGutterChars, term_frame::kBtopFontSize * scale),
      .minHeight = kQuickRow * scale,
      .maxHeight = kQuickRow * scale,
      .fillWidth = true,
  });

  m_qMenuKeys = {};
  const std::string primary = connected ? "disconnect" : (bucket == DeviceBucket::Paired ? "connect" : "pair");
  row->addChild(term_frame::makeKeyword(m_qMenuKeys[0], primary, 0, scale, [this, path]() { quickActivate(path); }));

  if (device.paired) {
    // State-aware mnemonic: the action reads `untrust` once trusted; the `t` stays the hot letter for both.
    const bool untrust = device.trusted;
    row->addChild(
        term_frame::makeKeyword(m_qMenuKeys[1], untrust ? "untrust" : "trust", untrust ? 2 : 0, scale, [this, path]() {
          quickToggleTrust(path);
        })
    );
    if (profiles != nullptr && profiles->profiles.size() > 1) {
      row->addChild(term_frame::makeKeyword(m_qMenuKeys[2], "profile", 0, scale, [this, path]() {
        quickCycleProfile(path);
      }));
    }
    // Destructive: only here, and only after a second press.
    row->addChild(term_frame::makeKeyword(m_qMenuKeys[3], "forget", 3, scale, [this, path]() {
      quickPressConfirmed(m_qMenuKeys[3], [this, path]() { quickForget(path); });
    }));
  }

  // `connecting` is shown once, in the row's state text; the menu status stays quiet meanwhile.
  std::string status = device.connecting ? "" : (connected ? "connected" : (device.paired ? "paired" : "not paired"));
  if (!device.connecting && profiles != nullptr && !profiles->active.empty()) {
    status += " · " + profiles->active;
  }
  row->addChild(
      ui::label({
          .text = status,
          .fontSize = font,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
          .maxLines = 1,
          .textAlign = TextAlign::End,
          .ellipsize = TextEllipsize::End,
          .flexGrow = 1.0F,
      })
  );
  return row;
}

// The pairing prompt as a row of ³available: `pair <name>  code 123456  accept / reject`, plus a PIN input line.
void BluetoothTab::buildQuickPairRow(Flex& list, float scale) {
  if (m_agent == nullptr || !m_agent->hasPendingRequest()) {
    return;
  }
  const auto req = m_agent->pendingRequest();
  std::string alias = req.deviceAlias;
  if (const auto device = findDevice(req.devicePath); device.has_value() && !device->alias.empty()) {
    alias = device->alias;
  }
  if (alias.empty()) {
    alias = "device";
  }
  const float font = term_frame::kBtopFontSize * scale;

  Flex* node = nullptr;
  auto row = makeQuickLine(scale, &m_qPairGutter, &node);
  m_qPairNode = node;
  row->addChild(quickText("pair " + alias, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold, true, font));
  row->addChild(pairingDetailLabel(req, font));

  m_qPairKeys = {};
  const bool needsInput = pairingNeedsInput(req.kind);
  const bool canAccept = !needsInput && req.kind != BluetoothPairingKind::DisplayPasskey;
  if (canAccept) {
    row->addChild(term_frame::makeKeyword(m_qPairKeys[0], "accept", 0, scale, [this]() { acceptPairing(); }));
    if (req.kind == BluetoothPairingKind::AuthorizeService) {
      row->addChild(term_frame::makeKeyword(m_qPairKeys[1], "trust+allow", 0, scale, [this]() {
        trustAndAllowPairing();
      }));
    }
    row->addChild(quickText("/", term_frame::dimColor(), FontWeight::Normal, false, font));
  }
  row->addChild(term_frame::makeKeyword(m_qPairKeys[2], "reject", 0, scale, [this]() { rejectPairing(); }));
  list.addChild(std::move(row));
  m_qOrder.push_back(kQuickPair);

  if (needsInput) {
    list.addChild(
        ui::row(
            {.align = FlexAlign::Center,
             .paddingH = term_frame::monoWidth(kQuickGutterChars, font),
             .minHeight = kQuickInputRow * scale,
             .maxHeight = kQuickInputRow * scale,
             .fillWidth = true},
            ui::input({
                .out = &m_pairingInput,
                .placeholder = i18n::tr("control-center.bluetooth.enter-code"),
                .surfaceOpacity = panelCardOpacity(),
                .flexGrow = 1.0F,
                .onSubmit = [this](const std::string& value) { submitPairingInput(value); },
            })
        )
    );
  }
}

void BluetoothTab::buildQuickRows(Flex& list, const std::vector<BluetoothDeviceInfo>& devices, float scale) {
  for (const auto& device : devices) {
    const std::string path = device.path;
    QuickRow quickRow;
    quickRow.avail = bucketFor(device) == DeviceBucket::Available;

    auto row = makeQuickLine(scale, &quickRow.gutter, &quickRow.node);
    auto button = term_frame::toggleRow(device.alias, scale, quickRow.row, [this, path]() {
      quickSelectId(path);
      quickActivate(path);
    });
    button->setFlexGrow(1.0F);
    // The name carries its state marker as markup; the wide state badge of the toggle row stays hidden.
    if (quickRow.row.button != nullptr && quickRow.row.button->label() != nullptr) {
      quickRow.row.button->label()->setUseMarkup(true);
    }
    if (quickRow.row.detail != nullptr) {
      quickRow.row.detail->setUseMarkup(true);
    }
    setShown(quickRow.row.stateBox, false);
    quickRow.name = deviceNameMarkup(device);
    if (quickRow.row.button != nullptr) {
      quickRow.row.button->setText(quickRow.name);
    }
    row->addChild(std::move(button));
    list.addChild(std::move(row));
    if (path == m_qMenuFor) {
      list.addChild(buildQuickMenu(device, scale));
    }
    m_qOrder.push_back(path);
    m_qRows.emplace(path, std::move(quickRow));
  }
}

// Identity of the built lists; they are rebuilt exactly when it changes.
std::string BluetoothTab::quickListKey(const std::vector<BluetoothDeviceInfo>& devices, float listWidth) const {
  std::string key = structureKey(devices);
  key += '|';
  key += m_qMenuFor;
  key += '|';
  key += std::to_string(m_qProfilesRev);
  key += '|';
  if (m_agent != nullptr && m_agent->hasPendingRequest()) {
    const auto req = m_agent->pendingRequest();
    key += std::to_string(static_cast<int>(req.kind))
        + '/'
        + req.devicePath
        + '/'
        + std::to_string(req.passkey)
        + '/'
        + req.pin
        + '/'
        + req.uuid;
  }
  key += '|';
  key += std::to_string(listWidth);
  return key;
}

// Fills the emptied ²/³ lists from the service state; true when a pairing prompt took a row of ³available.
bool BluetoothTab::fillQuickLists(const std::vector<BluetoothDeviceInfo>& devices, float scale) {
  if (m_service == nullptr) {
    addQuickNote(*m_qDevList, "bluetooth service unavailable", scale);
    return false;
  }
  const BluetoothState& s = m_service->state();
  if (!s.adapterPresent) {
    addQuickNote(*m_qDevList, "no bluetooth adapter", scale);
    return false;
  }
  if (!s.powered) {
    addQuickNote(*m_qDevList, s.rfkillSoftBlocked ? "blocked by rfkill" : "off   enable bluetooth above", scale);
    return false;
  }
  std::vector<BluetoothDeviceInfo> listed;
  std::vector<BluetoothDeviceInfo> available;
  for (const auto& device : devices) {
    (bucketFor(device) == DeviceBucket::Available ? available : listed).push_back(device);
  }
  if (listed.empty()) {
    addQuickNote(*m_qDevList, "no paired devices   r to scan", scale);
  } else {
    buildQuickRows(*m_qDevList, listed, scale);
  }
  const bool pairing = m_agent != nullptr && m_agent->hasPendingRequest();
  if (pairing) {
    buildQuickPairRow(*m_qAvailList, scale);
  }
  if (!available.empty()) {
    buildQuickRows(*m_qAvailList, available, scale);
  } else if (!pairing) {
    addQuickNote(*m_qAvailList, "scanning...", scale);
  }
  return pairing;
}

void BluetoothTab::rebuildQuickLists(Renderer& renderer) {
  uiAssertNotRendering("BluetoothTab::rebuildQuickLists");
  if (m_qDevList == nullptr || m_qAvailList == nullptr || m_qDevScroll == nullptr || m_qAvailScroll == nullptr) {
    return;
  }
  const float listWidth = m_qDevScroll->contentViewportWidth();
  if (listWidth <= 0.0F) {
    return;
  }
  std::vector<BluetoothDeviceInfo> devices;
  if (m_service != nullptr) {
    devices = sortedDevices(m_service->devices());
  }
  std::string key = quickListKey(devices, listWidth);
  if (key == m_qListKey) {
    return;
  }
  m_qListKey = std::move(key);

  const float scale = contentScale();
  const float devOffset = m_qDevScroll->scrollOffset();
  const float availOffset = m_qAvailScroll->scrollOffset();

  quickClearArmed();
  m_qArmTimer.stop();
  // A half-typed code survives the rebuild.
  const bool hadInput = m_pairingInput != nullptr || m_qResumeInput;
  const std::string typed = m_pairingInput != nullptr ? m_pairingInput->value() : std::move(m_qResumeTyped);
  m_qResumeTyped.clear();
  m_qResumeInput = false;
  m_pairingInput = nullptr;
  m_qPairGutter = nullptr;
  m_qPairNode = nullptr;
  m_qPairKeys = {};
  m_qMenuKeys = {};
  m_qRows.clear();
  m_qOrder = {kQuickPower, kQuickDiscoverable};
  for (Flex* list : {m_qDevList, m_qAvailList}) {
    while (!list->children().empty()) {
      list->removeChild(list->children().front().get());
    }
  }

  const bool pairing = fillQuickLists(devices, scale);

  if (!m_qMenuFor.empty() && !m_qRows.contains(m_qMenuFor)) {
    m_qMenuFor.clear();
  }
  // The prompt takes the selection; without a valid selection the first device (or the first row) gets it.
  if (pairing && m_qPairNode != nullptr) {
    m_qSelected = kQuickPair;
  } else if (std::ranges::find(m_qOrder, m_qSelected) == m_qOrder.end()) {
    m_qSelected = m_qOrder.size() > kQuickAdapterRows ? m_qOrder[kQuickAdapterRows] : m_qOrder.front();
  }

  if (m_pairingInput != nullptr) {
    if (!typed.empty()) {
      m_pairingInput->setValue(typed);
    }
    if (hadInput) {
      m_qFocusInput = true;
    }
  }

  m_qDevList->layout(renderer);
  m_qAvailList->layout(renderer);
  m_qDevScroll->requestScrollToOffset(devOffset);
  m_qAvailScroll->requestScrollToOffset(availOffset);
}

void BluetoothTab::syncQuickGutters() {
  const auto mark = [this](Label* gutter, const std::string& id) {
    if (gutter != nullptr) {
      gutter->setText(m_qSelected == id ? std::string_view(term_frame::kMarkerNeutral) : std::string_view(" "));
    }
  };
  mark(m_qPowerGutter, kQuickPower);
  mark(m_qDiscoverableGutter, kQuickDiscoverable);
  mark(m_qPairGutter, kQuickPair);
  for (const auto& [path, quickRow] : m_qRows) {
    mark(quickRow.gutter, path);
  }
}

// Dim detail at the end of a row; progress and failures show here, so the name keeps the width.
std::string BluetoothTab::quickDetail(const BluetoothDeviceInfo& device) const {
  const auto bucket = bucketFor(device);
  if (device.connecting) {
    return bucket == DeviceBucket::Available ? "pairing…" : "connecting…";
  }
  if (const auto failure = quickFailure(device.path); failure.has_value()) {
    return spanMarkup(ColorRole::Error, "failed: " + escapeMarkup(*failure));
  }
  if (bucket == DeviceBucket::Available) {
    return device.hasRssi ? quickSignalMarkup(signalPercentFromRssi(device.rssi)) : std::string{};
  }
  std::string detail;
  if (isHeadphones(device.kind) && bucket == DeviceBucket::Connected) {
    if (const QuickProfiles* profiles = quickProfilesFor(device); profiles != nullptr && !profiles->active.empty()) {
      detail = escapeMarkup(profileShort(profiles->active));
    }
  }
  if (device.hasBattery) {
    if (!detail.empty()) {
      detail += "  ";
    }
    detail += quickBatteryMarkup(device.address, static_cast<int>(device.batteryPercent));
  }
  return detail;
}

bool BluetoothTab::syncQuickRows() {
  if (m_service == nullptr || m_qRows.empty()) {
    return false;
  }
  bool changed = false;
  for (const auto& device : m_service->devices()) {
    const auto it = m_qRows.find(device.path);
    if (it == m_qRows.end()) {
      continue;
    }
    QuickRow& quickRow = it->second;
    if (quickRow.row.button == nullptr || quickRow.row.state == nullptr) {
      continue;
    }

    std::string name = deviceNameMarkup(device);
    if (quickRow.name != name) {
      quickRow.row.button->setText(name);
      quickRow.name = std::move(name);
      changed = true;
    }

    const std::string detail = quickDetail(device);
    if (quickRow.detail != detail) {
      changed = true;
      quickRow.detail = detail;
    }
    term_frame::setToggleRow(quickRow.row, true, false, "", detail);
    setShown(quickRow.row.stateBox, false);
  }
  return changed;
}

void BluetoothTab::scrollSelectedIntoView() {
  Flex* node = nullptr;
  ScrollView* scroll = nullptr;
  if (m_qSelected == kQuickPair) {
    node = m_qPairNode;
    scroll = m_qAvailScroll;
  } else if (const auto it = m_qRows.find(m_qSelected); it != m_qRows.end()) {
    node = it->second.node;
    scroll = it->second.avail ? m_qAvailScroll : m_qDevScroll;
  }
  if (node != nullptr && scroll != nullptr) {
    scroll->requestScrollIntoView(node);
  }
}

// Failures clear themselves after kQuickFailureTimeout: update on a timer while one is on screen.
void BluetoothTab::pollQuickFailures(bool adapterFailing) {
  const bool anyFailure = adapterFailing
      || std::ranges::any_of(m_qRows, [this](const auto& entry) { return quickFailure(entry.first).has_value(); });
  if (anyFailure && !m_qFailTimer.active()) {
    m_qFailTimer.start(kQuickFailurePoll, []() { requestQuickRefresh(); });
  }
}

void BluetoothTab::syncQuick(Renderer& renderer, bool force) {
  if (m_rootLayout == nullptr) {
    return;
  }
  syncDiscoveryLease();
  if (force) {
    m_qListKey.clear();
  }

  const QuickPlan plan = quickPlan();
  const BluetoothState none;
  const BluetoothState& s = m_service != nullptr ? m_service->state() : none;

  // The cards of the connected headphones follow the connected set.
  std::string connected = m_service != nullptr ? connectedHeadphonesKey(m_service->devices()) : std::string{};
  if (connected != m_qConnectedKey) {
    m_qConnectedKey = std::move(connected);
    if (!m_qConnectedKey.empty()) {
      refreshProfiles();
    }
  }

  const auto powerFailure = quickFailure("adapter");
  const auto discoverableFailure = quickFailure("adapter.discoverable");
  const bool powerChanged =
      applyToggleRow(m_qPower, s.adapterPresent, s.powered, s.powered ? "on" : "off", powerFailure);
  const bool discoverableChanged = applyToggleRow(
      m_qDiscoverable, s.adapterPresent && s.powered, s.discoverable && s.powered, s.discoverable ? "on" : "off",
      discoverableFailure
  );
  bool relayout = powerChanged || discoverableChanged;
  if (m_qAvailChip != nullptr) {
    term_frame::setChip(*m_qAvailChip, s.adapterPresent && s.powered && s.discovering ? "scanning" : "");
  }

  // Shape: which boxes exist and how tall their lists are. Pinned so the panel height follows quickPanelHeight().
  if (plan.key != m_qPlanKey) {
    m_qPlanKey = plan.key;
    const float scale = contentScale();
    pinQuickList(m_qDevBox, m_qDevScroll, plan.devLines, 0.0F, scale);
    setShown(m_qAvailBox, plan.availBox);
    if (plan.availBox) {
      pinQuickList(m_qAvailBox, m_qAvailScroll, plan.availLines, plan.pairingInput ? kQuickInputRow : 0.0F, scale);
    }
    relayout = true;
  }
  if (relayout) {
    m_rootLayout->layout(renderer);
  }

  rebuildQuickLists(renderer);
  if (syncQuickRows()) {
    if (m_qDevList != nullptr) {
      m_qDevList->layout(renderer);
    }
    if (m_qAvailList != nullptr) {
      m_qAvailList->layout(renderer);
    }
  }
  syncQuickGutters();

  if (m_qScrollToSelected) {
    m_qScrollToSelected = false;
    scrollSelectedIntoView();
  }
  pollQuickFailures(powerFailure.has_value() || discoverableFailure.has_value());

  // A code prompt takes the keyboard as soon as it appears.
  if (plan.pairingInput && !m_qHadInput) {
    m_qFocusInput = true;
  }
  m_qHadInput = plan.pairingInput;
  if (m_qFocusInput && m_pairingInput != nullptr) {
    m_qFocusInput = false;
    PanelManager::instance().focusArea(m_pairingInput->inputArea());
  }

  // Structural change in what the panel shows: the host reopens at the new height when it differs. Last, as a
  // reopen may tear this tab's nodes down.
  if (plan.key != m_qHeightKey) {
    m_qHeightKey = plan.key;
    requestQuickPanelResize();
  }
}

// ---- headphone card profiles (pactl) --------------------------------------------------------------------------

void BluetoothTab::refreshProfiles() {
  if (m_qProfilesBusy || !process::commandExists("pactl")) {
    return;
  }
  m_qProfilesBusy = true;
  asynccmd::run(
      {"pactl", "-f", "json", "list", "cards"}, kPactlListTimeout, m_alive, [this](process::RunResult result) {
        m_qProfilesBusy = false;
        if (result) {
          applyProfiles(result.out);
        }
      }
  );
}

// Selectable profiles per upper-case address of every bluez card in `pactl -f json list cards`; nullopt when the
// output is not a JSON array.
std::optional<std::unordered_map<std::string, BluetoothTab::QuickProfiles>>
BluetoothTab::parseProfiles(const std::string& json) {
  const auto doc = nlohmann::json::parse(json, nullptr, false);
  if (!doc.is_array()) {
    return std::nullopt;
  }
  constexpr std::string_view kCardPrefix = "bluez_card.";
  std::unordered_map<std::string, QuickProfiles> byAddress;
  for (const auto& card : doc) {
    if (!card.is_object()) {
      continue;
    }
    const std::string name = card.value("name", std::string{});
    if (!name.starts_with(kCardPrefix)) {
      continue;
    }
    std::string address;
    if (const auto props = card.find("properties"); props != card.end() && props->is_object()) {
      address = props->value("api.bluez5.address", std::string{});
    }
    if (address.empty()) {
      address = name.substr(kCardPrefix.size());
      std::ranges::replace(address, '_', ':');
    }
    QuickProfiles profiles;
    profiles.card = name;
    profiles.active = card.value("active_profile", std::string{});
    if (const auto list = card.find("profiles"); list != card.end() && list->is_object()) {
      for (const auto& item : list->items()) {
        if (item.key() == "off") {
          continue;
        }
        const auto& info = item.value();
        const auto available = info.is_object() ? info.find("available") : info.end();
        if (info.is_object() && available != info.end() && available->is_boolean() && !available->get<bool>()) {
          continue;
        }
        profiles.profiles.push_back(item.key());
      }
    }
    byAddress[upperAscii(address)] = std::move(profiles);
  }
  return byAddress;
}

void BluetoothTab::applyProfiles(const std::string& json) {
  auto next = parseProfiles(json);
  if (next && *next != m_qProfiles) {
    m_qProfiles = std::move(*next);
    ++m_qProfilesRev;
    requestQuickRefresh();
  }
}

// Switches the headphone to its next card profile (a2dp <-> headset ...).
void BluetoothTab::quickCycleProfile(const std::string& path) {
  const auto device = findDevice(path);
  if (!device.has_value()) {
    return;
  }
  clearQuickFailure(path);
  const QuickProfiles* profiles = quickProfilesFor(*device);
  if (profiles == nullptr || profiles->profiles.size() < 2) {
    setLocalFailure(path, "no other profile");
    return;
  }
  const auto current = std::ranges::find(profiles->profiles, profiles->active);
  const std::size_t next = current == profiles->profiles.end()
      ? 0
      : static_cast<std::size_t>(current - profiles->profiles.begin() + 1) % profiles->profiles.size();
  const std::string card = profiles->card;
  const std::string profile = profiles->profiles[next];

  asynccmd::run(
      {"pactl", "set-card-profile", card, profile}, kPactlSwitchTimeout, m_alive,
      [this, path](process::RunResult result) {
        if (result) {
          refreshProfiles();
          return;
        }
        setLocalFailure(path, result.timedOut ? "timeout" : firstLineLower(result.err, "profile switch failed"));
      }
  );
}
