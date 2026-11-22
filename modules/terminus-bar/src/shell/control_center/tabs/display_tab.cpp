#include "shell/control_center/tabs/display_tab.h"

#include "config/config_service.h"
#include "core/input/key_modifiers.h"
#include "render/core/renderer.h"
#include "system/brightness_service.h"
#include "system/gamma_service.h"
#include "system/weather_service.h"
#include "shell/panel/panel_manager.h"
#include "time/time_format.h"
#include "ui/builders.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string_view>
#include <utility>

#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  constexpr float kSyncEpsilon = 0.005F;
  constexpr auto kBrightnessCommitInterval = std::chrono::milliseconds(16);
  constexpr auto kBrightnessStateHoldoff = std::chrono::milliseconds(180);
  constexpr auto kWarmthCommitDelay = std::chrono::milliseconds(200);
  constexpr std::int32_t kWarmthMin = 1500;
  constexpr std::int32_t kWarmthMax = 6500;
  constexpr std::int32_t kWarmthStep = 100;

  std::string formatPercent(float value) { return std::format("{}%", static_cast<int>(std::lround(value * 100.0F))); }

  std::string buildDisplayListKey(const std::vector<BrightnessDisplay>& displays) {
    std::string key;
    for (const auto& display : displays) {
      key += display.id;
      key += display.controllable ? ":1;" : ":0;";
    }
    return key;
  }

  constexpr std::size_t kTagCols = 9;
  constexpr std::size_t kValueCols = 6;
  constexpr float kBoxChrome = 2.0F * (Style::spaceSm + Style::borderWidth) + 20.0F + Style::spaceXs;
  constexpr float kSectionGap = Style::spaceSm;

  // Sizes a tag | meter | value row to `innerWidth` on fixed monospace columns, so the value column is always
  // reserved and never pushed out of the row by the slider.
  float layoutMeterRow(const term_frame::MeterRow& meter, float innerWidth, float scale) {
    const float font = Style::fontSizeCaption * scale;
    const float tagWidth = std::min(term_frame::monoWidth(kTagCols, font), innerWidth * 0.34F);
    const float valueWidth = term_frame::monoWidth(kValueCols, font);
    meter.value->setVisible(true);
    return term_frame::layoutMeterCells(
        *meter.row, *meter.tag, *meter.slider, *meter.value, nullptr, innerWidth, tagWidth, valueWidth, 0.0F, scale
    );
  }

  std::unique_ptr<Label> mutedLabel(float scale, std::string text, bool visible = true) {
    return ui::label({
        .text = std::move(text),
        .fontSize = Style::fontSizeCaption * scale,
        .fontFamily = std::string(term_frame::kFont),
        .color = term_frame::dimColor(),
        .visible = visible,
    });
  }

} // namespace

DisplayTab::DisplayTab(
    BrightnessService* brightness, GammaService* nightLight, ConfigService* config, WeatherService* weather
)
    : m_brightness(brightness), m_nightLight(nightLight), m_config(config), m_weather(weather) {}

std::unique_ptr<Flex> DisplayTab::create() {
  const float scale = contentScale();

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });

  const auto tileRow = [&]() {
    return ui::row({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .fillWidth = true});
  };

  // --- ¹brightness ---
  auto brightnessBox = term_frame::box("¹", "brightness", scale, &m_brightnessBox, &m_brightnessChip);
  brightnessBox->addChild(
      ui::column({
          .out = &m_brightnessPanel,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
          .fillWidth = true,
      })
  );
  {
    auto unavailable = mutedLabel(scale, "unavailable", false);
    m_brightnessUnavailable = unavailable.get();
    brightnessBox->addChild(std::move(unavailable));
  }
  {
    auto row = tileRow();
    row->addChild(term_frame::makeKeyword(m_rescanKey, "rescan displays", 0, scale, [this]() { rescanDisplays(); }));
    brightnessBox->addChild(std::move(row));
  }
  tab->addChild(std::move(brightnessBox));

  // --- ²night light ---
  auto nightBox = term_frame::box("²", "night light", scale, &m_nightBox, &m_nightChip);
  {
    auto unavailable = mutedLabel(scale, "unavailable", false);
    m_nightUnavailable = unavailable.get();
    nightBox->addChild(std::move(unavailable));
  }
  auto controls = ui::column({
      .out = &m_nightControls,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
  });
  controls->addChild(term_frame::toggleRow("schedule", scale, m_scheduleRow, [this]() { toggleSchedule(); }));
  controls->addChild(term_frame::toggleRow("force on now", scale, m_forceRow, [this]() { toggleForce(); }));
  controls->addChild(term_frame::meterRow("warmth", scale, m_warmthMeter));
  if (Slider* slider = m_warmthMeter.slider; slider != nullptr) {
    slider->setRange(static_cast<double>(kWarmthMin), static_cast<double>(kWarmthMax));
    slider->setStep(static_cast<double>(kWarmthStep));
    slider->setValue(static_cast<double>(kWarmthMin));
    slider->setReadOnly(false);
    slider->setWheelAdjustEnabled(true);
    slider->setAccentColor(colorSpecFromRole(ColorRole::Primary));
    slider->setOnValueChanged([this](double value) {
      if (m_syncing) {
        return;
      }
      queueWarmth(static_cast<std::int32_t>(std::lround(value)));
    });
    slider->setOnDragEnd([this]() { flushPendingWarmth(); });
  }
  {
    auto status = mutedLabel(scale, "");
    m_nightStatus = status.get();
    controls->addChild(std::move(status));
  }
  nightBox->addChild(std::move(controls));
  tab->addChild(std::move(nightBox));


  m_rowsBuilt = false;
  m_rowsKey.clear();
  m_reportedRows = -1;
  m_resizePending = false;
  return tab;
}

void DisplayTab::setActive(bool active) {
  const bool becameActive = active && !m_active;
  m_active = active;
  if (becameActive && m_brightness != nullptr) {
    m_brightness->requestDdcRefresh();
  }
}

void DisplayTab::onClose() {
  flushPendingBrightness();
  flushPendingWarmth();
  m_debounceTimer.stop();
  m_warmthTimer.stop();
  m_rootLayout = nullptr;
  m_brightnessBox = nullptr;
  m_brightnessChip = nullptr;
  m_brightnessPanel = nullptr;
  m_brightnessUnavailable = nullptr;
  m_rescanKey = {};
  m_nightBox = nullptr;
  m_nightChip = nullptr;
  m_nightControls = nullptr;
  m_nightUnavailable = nullptr;
  m_scheduleRow = {};
  m_forceRow = {};
  m_warmthMeter = {};
  m_nightStatus = nullptr;
  m_rows.clear();
  m_rowsKey.clear();
  m_rowsBuilt = false;
  m_reportedRows = -1;
  m_resizePending = false;
  m_pendingValid = false;
  m_pendingKelvin = -1;
}

float DisplayTab::minimumBrightness() const {
  return std::clamp(m_config != nullptr ? m_config->config().brightness.minimumBrightness : 0.0F, 0.0F, 1.0F);
}

bool DisplayTab::brightnessDragging() const {
  for (const auto& row : m_rows) {
    if (row->meter.slider != nullptr && row->meter.slider->dragging()) {
      return true;
    }
  }
  return false;
}

std::int32_t DisplayTab::warmthCeiling() const {
  std::int32_t ceiling = kWarmthMax;
  if (m_config != nullptr) {
    ceiling = std::min(ceiling, m_config->config().nightlight.dayTemperature - NightLightConfig::kTemperatureGap);
  }
  return std::max(kWarmthMin, ceiling);
}

void DisplayTab::rebuildBrightnessRows(float scale) {
  if (m_brightnessPanel == nullptr) {
    return;
  }
  const std::vector<BrightnessDisplay> none;
  const auto& displays = m_brightness != nullptr ? m_brightness->displays() : none;
  const std::string key = buildDisplayListKey(displays);
  if (m_rowsBuilt && key == m_rowsKey) {
    return;
  }
  // Keep the rows (and their pointer capture) alive until the drag releases.
  if (brightnessDragging()) {
    return;
  }
  m_rowsBuilt = true;
  m_rowsFresh = true;
  m_rowsKey = key;

  for (auto& row : m_rows) {
    if (row->meter.row != nullptr) {
      (void)m_brightnessPanel->removeChild(row->meter.row);
    }
  }
  m_rows.clear();

  const auto addRow = [&](const std::string& id, std::string_view tag, bool controllable) {
    auto control = std::make_unique<BrightnessRow>();
    BrightnessRow* const state = control.get();
    state->displayId = id;
    state->lastControllable = controllable;
    m_brightnessPanel->addChild(term_frame::meterRow(tag, scale, state->meter));
    Slider* slider = state->meter.slider;
    slider->setRange(static_cast<double>(minimumBrightness()), 1.0);
    slider->setStep(0.01);
    slider->setReadOnly(false);
    slider->setWheelAdjustEnabled(true);
    slider->setAccentColor(colorSpecFromRole(ColorRole::Primary));
    slider->setEnabled(controllable);
    slider->setOnValueChanged([this, state](double value) {
      if (m_syncing || m_brightness == nullptr) {
        return;
      }
      const float brightness = std::clamp(static_cast<float>(value), minimumBrightness(), 1.0F);
      state->lastValue = brightness;
      state->meter.value->setText(formatPercent(brightness));
      queueBrightness(state->displayId, brightness);
    });
    slider->setOnDragEnd([this]() { flushPendingBrightness(); });
    if (!controllable) {
      state->meter.tag->setColor(term_frame::dimColor());
      state->meter.value->setColor(term_frame::dimColor());
      state->meter.value->setText("n/a");
    }
    m_rows.push_back(std::move(control));
  };

  bool anyControllable = false;
  for (const auto& display : displays) {
    addRow(display.id, display.id, display.controllable);
    anyControllable = anyControllable || display.controllable;
  }
  if (!displays.empty()) {
    addRow({}, "all", anyControllable);
  }
  const int rowCount = static_cast<int>(m_rows.size());
  if (m_reportedRows != rowCount) {
    // The first build matches what quickPanelHeight() already sized for.
    m_resizePending = m_reportedRows >= 0;
    m_reportedRows = rowCount;
  }
}

void DisplayTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  (void)bodyHeight;
  if (m_rootLayout == nullptr) {
    return;
  }
  const float scale = contentScale();
  const float gap = Style::spaceXs * scale;
  const float width = std::max(1.0F, contentWidth);
  const float innerWidth = std::max(1.0F, width - 2.0F * term_frame::boxInset(scale));

  rebuildBrightnessRows(scale);
  // The rows are built here, after the first update already ran: fill in their values now rather than waiting
  // for an update that nothing would request.
  if (m_rowsFresh) {
    m_rowsFresh = false;
    syncBrightness();
  }

  float rowsHeight = 0.0F;
  for (const auto& row : m_rows) {
    rowsHeight += layoutMeterRow(row->meter, innerWidth, scale);
  }
  if (!m_rows.empty()) {
    rowsHeight += static_cast<float>(m_rows.size() - 1) * gap;
  }
  m_brightnessPanel->setGap(gap);
  m_brightnessPanel->setMinHeight(rowsHeight);
  m_brightnessPanel->setMaxHeight(rowsHeight);
  m_brightnessPanel->setSize(innerWidth, rowsHeight);
  m_brightnessPanel->setVisible(!m_rows.empty());
  m_brightnessPanel->setParticipatesInLayout(!m_rows.empty());

  layoutMeterRow(m_warmthMeter, innerWidth, scale);
  if (m_nightStatus != nullptr) {
    m_nightStatus->setMaxWidth(innerWidth);
    m_nightStatus->setMaxLines(2);
  }

  m_rootLayout->setSize(width, 0.0F);
  m_rootLayout->layout(renderer);
}

void DisplayTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_rootLayout == nullptr) {
    return;
  }
  syncBrightness();
  syncNightLight();
  if (m_resizePending) {
    m_resizePending = false;
    requestQuickPanelResize();
  }
}

void DisplayTab::syncBrightness() {
  const bool haveService = m_brightness != nullptr;
  const bool haveRows = !m_rows.empty();
  if (m_brightnessUnavailable != nullptr) {
    const bool showUnavailable = !haveService || m_brightness->displays().empty();
    if (m_brightnessUnavailable->visible() != showUnavailable) {
      m_brightnessUnavailable->setVisible(showUnavailable);
      m_brightnessUnavailable->setParticipatesInLayout(showUnavailable);
    }
  }
  if (m_rescanKey.button != nullptr) {
    m_rescanKey.button->setEnabled(haveService);
  }
  if (m_brightnessChip != nullptr) {
    const std::size_t outputs = haveService ? m_brightness->displays().size() : 0;
    term_frame::setChip(*m_brightnessChip, haveService ? std::format("{} outputs", outputs) : std::string());
  }
  if (!haveService || !haveRows) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const float minimum = minimumBrightness();

  // Mean of the controllable outputs drives the `all` row.
  float sum = 0.0F;
  float lowest = 1.0F;
  float highest = 0.0F;
  int count = 0;
  for (const auto& display : m_brightness->displays()) {
    if (display.controllable) {
      sum += display.brightness;
      lowest = std::min(lowest, display.brightness);
      highest = std::max(highest, display.brightness);
      ++count;
    }
  }
  const bool mixed = count > 1 && highest - lowest > 0.02F;
  const float mean = count > 0 ? sum / static_cast<float>(count) : 0.0F;

  for (auto& row : m_rows) {
    Slider* slider = row->meter.slider;
    if (slider == nullptr) {
      continue;
    }
    const bool isAll = row->displayId.empty();
    const BrightnessDisplay* display = isAll ? nullptr : m_brightness->findDisplay(row->displayId);
    const bool controllable = isAll ? count > 0 : (display != nullptr && display->controllable);
    if (!isAll && display == nullptr) {
      continue;
    }
    if (!controllable) {
      row->meter.value->setText("n/a");
      row->lastControllable = false;
      continue;
    }

    const float actual = isAll ? mean : display->brightness;
    const bool dragging = slider->dragging();
    const bool isPending = m_pendingValid && m_pendingDisplayId == row->displayId;
    const bool holdState = dragging && m_lastSentBrightness >= 0.0F && m_lastSentId == row->displayId
        && now < m_ignoreStateUntil && std::abs(actual - m_lastSentBrightness) > 0.02F;

    if (dragging) {
      row->lastControllable = true;
      continue;
    }
    const float shown = std::clamp(isPending ? m_pendingBrightness : (holdState ? m_lastSentBrightness : actual), minimum, 1.0F);
    const bool changed = !row->lastControllable || std::abs(shown - row->lastValue) >= kSyncEpsilon;
    m_syncing = true;
    slider->setRange(static_cast<double>(minimum), 1.0);
    slider->setEnabled(true);
    if (changed) {
      slider->setValue(shown);
      row->lastValue = shown;
    }
    m_syncing = false;
    if (changed || !row->lastControllable) {
      row->meter.tag->setColor(colorSpecFromRole(ColorRole::OnSurface));
      row->meter.value->setColor(colorSpecFromRole(ColorRole::OnSurface));
    }
    // Always (re)write the value: the row may have been rebuilt or laid out since the last change.
    const std::string percent = isAll && mixed ? std::string("mixed") : formatPercent(shown);
    if (row->meter.value->text() != percent) {
      row->meter.value->setText(percent);
      PanelManager::instance().requestLayout();
    }
    row->lastControllable = true;
  }
}

void DisplayTab::syncNightLight() {
  const bool haveService = m_nightLight != nullptr;
  if (m_nightUnavailable != nullptr && m_nightUnavailable->visible() == haveService) {
    m_nightUnavailable->setVisible(!haveService);
    m_nightUnavailable->setParticipatesInLayout(!haveService);
  }
  if (m_nightControls != nullptr) {
    if (m_nightControls->visible() != haveService) {
      m_nightControls->setVisible(haveService);
      m_nightControls->setParticipatesInLayout(haveService);
    }
  }
  if (!haveService) {
    if (m_nightChip != nullptr) {
      term_frame::setChip(*m_nightChip, "");
    }
    return;
  }

  const bool scheduled = m_nightLight->enabled();
  const bool forced = m_nightLight->forceEnabled();
  const bool active = m_nightLight->active();
  if (m_nightChip != nullptr) {
    term_frame::setChip(*m_nightChip, (active || forced) ? "active" : (scheduled ? "scheduled" : "off"));
  }
  std::string sunset;
  std::string sunrise;
  sunTimeStrings(sunset, sunrise);
  std::string scheduleDetail;
  if (scheduled) {
    const std::string& when = active ? sunrise : sunset;
    if (!when.empty()) {
      scheduleDetail = std::format("· {} {}", active ? "until" : "on at", when);
    }
  }
  term_frame::setToggleRow(m_scheduleRow, true, scheduled, scheduled ? "on" : "off", scheduleDetail);
  term_frame::setToggleRow(m_forceRow, true, forced, forced ? "on" : "off");

  // Warmth only matters while night light is on (scheduled-active or forced); dim it otherwise.
  const bool warmthLive = active || forced;
  if (m_warmthMeter.tag != nullptr && m_warmthMeter.value != nullptr && m_warmthMeter.slider != nullptr) {
    const ColorSpec tone = warmthLive ? colorSpecFromRole(ColorRole::OnSurface) : term_frame::dimColor();
    m_warmthMeter.tag->setColor(term_frame::dimColor());
    m_warmthMeter.value->setColor(tone);
    m_warmthMeter.slider->setOpacity(warmthLive ? 1.0F : 0.45F);
  }

  // Warmth follows the persisted night temperature unless a write is in flight.
  Slider* slider = m_warmthMeter.slider;
  if (slider != nullptr && m_config != nullptr) {
    const std::int32_t ceiling = warmthCeiling();
    const std::int32_t persisted = std::clamp(m_config->config().nightlight.nightTemperature, kWarmthMin, ceiling);
    if (!slider->dragging() && m_pendingKelvin < 0) {
      m_syncing = true;
      slider->setRange(static_cast<double>(kWarmthMin), static_cast<double>(ceiling));
      if (static_cast<std::int32_t>(std::lround(slider->value())) != persisted) {
        slider->setValue(static_cast<double>(persisted));
      }
      m_syncing = false;
      m_warmthMeter.value->setText(std::format("{}K", persisted));
    }
  }

  if (m_nightStatus != nullptr && m_config != nullptr) {
    const auto& location = m_config->config().location;
    std::string text;
    if (!sunset.empty() && !sunrise.empty()) {
      text = std::format("sunset {}  sunrise {}", sunset, sunrise);
    } else if (location.autoLocate || !location.address.empty() || (location.latitude && location.longitude)) {
      text = "location schedule";
    } else {
      text = "no schedule: set a location in settings";
    }
    if (forced) {
      text += "  (forced on)";
    }
    m_nightStatus->setText(text);
  }
}

void DisplayTab::queueBrightness(const std::string& displayId, float value) {
  m_pendingDisplayId = displayId;
  m_pendingBrightness = value;
  m_pendingValid = true;

  const auto now = std::chrono::steady_clock::now();
  if (now - m_lastCommitAt >= kBrightnessCommitInterval) {
    flushPendingBrightness();
    return;
  }
  if (!m_debounceTimer.active()) {
    m_debounceTimer.start(kBrightnessCommitInterval, [this]() { flushPendingBrightness(); });
  }
}

void DisplayTab::flushPendingBrightness() {
  m_debounceTimer.stop();
  if (!m_pendingValid || m_brightness == nullptr) {
    m_pendingValid = false;
    return;
  }
  m_pendingValid = false;

  if (m_pendingDisplayId.empty()) {
    m_brightness->setAllBrightness(m_pendingBrightness);
  } else {
    const auto* display = m_brightness->findDisplay(m_pendingDisplayId);
    if (display == nullptr || !display->controllable) {
      return;
    }
    m_brightness->setBrightness(m_pendingDisplayId, m_pendingBrightness);
  }
  m_lastSentBrightness = m_pendingBrightness;
  m_lastSentId = m_pendingDisplayId;
  m_lastCommitAt = std::chrono::steady_clock::now();
  m_ignoreStateUntil = m_lastCommitAt + kBrightnessStateHoldoff;
}

void DisplayTab::queueWarmth(std::int32_t kelvin) {
  const std::int32_t snapped = static_cast<std::int32_t>(std::lround(static_cast<double>(kelvin) / kWarmthStep)) * kWarmthStep;
  m_pendingKelvin = std::clamp(snapped, kWarmthMin, warmthCeiling());
  if (m_warmthMeter.value != nullptr) {
    m_warmthMeter.value->setText(std::format("{}K", m_pendingKelvin));
  }
  m_warmthTimer.start(kWarmthCommitDelay, [this]() { flushPendingWarmth(); });
}

void DisplayTab::flushPendingWarmth() {
  m_warmthTimer.stop();
  if (m_pendingKelvin < 0) {
    return;
  }
  const std::int32_t kelvin = m_pendingKelvin;
  m_pendingKelvin = -1;
  if (m_config == nullptr) {
    return;
  }
  // Persisting triggers the config reload, which re-applies the gamma ramp live.
  (void)m_config->setOverride({"nightlight", "temperature_night"}, static_cast<std::int64_t>(kelvin));
}

void DisplayTab::sunTimeStrings(std::string& sunset, std::string& sunrise) const {
  sunset.clear();
  sunrise.clear();
  if (m_config != nullptr) {
    const auto& location = m_config->config().location;
    if (location.customSchedule && !location.sunset.empty() && !location.sunrise.empty()) {
      sunset = location.sunset;
      sunrise = location.sunrise;
      return;
    }
  }
  if (m_weather != nullptr) {
    const WeatherSnapshot& snapshot = m_weather->snapshot();
    if (snapshot.valid && !snapshot.forecastDays.empty()) {
      sunset = formatIsoTime(snapshot.forecastDays.front().sunsetIso, "%H:%M");
      sunrise = formatIsoTime(snapshot.forecastDays.front().sunriseIso, "%H:%M");
      if (!sunset.empty() && !sunrise.empty()) {
        return;
      }
    }
  }
  if (m_nightLight != nullptr) {
    if (const auto sun = m_nightLight->sunTimes(); sun.has_value()) {
      sunset = sun->sunset;
      sunrise = sun->sunrise;
      return;
    }
  }
  sunset.clear();
  sunrise.clear();
}

void DisplayTab::rescanDisplays() {
  if (m_brightness != nullptr) {
    m_brightness->requestDdcRescan();
  }
}

void DisplayTab::toggleSchedule() {
  if (m_nightLight != nullptr) {
    m_nightLight->setEnabled(!m_nightLight->enabled());
  }
}

void DisplayTab::toggleForce() {
  if (m_nightLight == nullptr) {
    return;
  }
  if (m_nightLight->forceEnabled()) {
    m_nightLight->clearForceOverride();
  } else {
    m_nightLight->setForceEnabled(true);
  }
}

float DisplayTab::quickPanelHeight(float width, float fallback) const {
  (void)width;
  (void)fallback;
  constexpr float kRow = term_frame::kRowHeight;
  constexpr float gap = Style::spaceXs;

  const std::size_t displays = m_brightness != nullptr ? m_brightness->displays().size() : 0;
  // Output rows plus the `all` row, or the single `unavailable` line.
  const float bodyHeight = displays > 0 ? static_cast<float>(displays + 1) * kRow + static_cast<float>(displays) * gap : kRow;
  const float brightnessBox = kBoxChrome + bodyHeight + gap + kRow; // + rescan keyword row

  // schedule, force on now, warmth, status line.
  const float nightBox = m_nightLight != nullptr ? kBoxChrome + 4.0F * kRow + 3.0F * gap : kBoxChrome + kRow;
  return brightnessBox + kSectionGap + nightBox;
}

bool DisplayTab::handleQuickPanelKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (preedit || (modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) != 0U) {
    return false;
  }
  switch (sym) {
  case XKB_KEY_r:
  case XKB_KEY_R:
    if (m_brightness == nullptr) {
      return false;
    }
    if (pressed) {
      rescanDisplays();
    }
    return true;
  case XKB_KEY_n:
  case XKB_KEY_N:
    if (m_nightLight == nullptr) {
      return false;
    }
    if (pressed) {
      toggleSchedule();
    }
    return true;
  case XKB_KEY_o:
  case XKB_KEY_O:
    if (m_nightLight == nullptr) {
      return false;
    }
    if (pressed) {
      toggleForce();
    }
    return true;
  default:
    return false;
  }
}
