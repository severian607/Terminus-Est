// Weather panel body: ¹now, ²hours (next 12) and ³days (7) boxes over an optional weather animation.
// Hot path: doUpdate -> sync() runs on every panel update and rewrites each label from the snapshot.

#include "shell/control_center/tabs/weather_tab.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/effect_node.h"
#include "shell/panel/panel_manager.h"
#include "system/weather_service.h"
#include "time/time_format.h"
#include "ui/builders.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

namespace {

  constexpr float kMaxWidth = 680.0F;
  constexpr float kRowHeight = 18.0F;
  constexpr float kNowHeight = 40.0F;
  constexpr float kTempFontSize = 36.0F;
  constexpr float kGlyphSize = 14.0F;
  constexpr float kNowGlyphSize = 28.0F;
  constexpr float kRainHeight = 4.0F;
  constexpr float kDayBarHeight = 6.0F;
  constexpr float kBoxGap = 6.0F;
  constexpr float kInnerGap = 2.0F;
  constexpr float kHourColumnGap = 1.0F;
  constexpr float kBarHeight = 22.0F;
  constexpr float kMinDayTrackWidth = 40.0F;
  constexpr float kUvScale = 11.0F;
  constexpr float kBoxFillAlpha = 0.94F;
  constexpr float kVeilAlpha = 0.25F;
  constexpr float kTrackAlpha = 0.5F;
  constexpr int kBehindContentZ = -2; // effect, then its veil, both under the boxes
  constexpr std::size_t kKeyChars = 10;
  constexpr std::size_t kDayChars = 4;
  constexpr std::size_t kConditionChars = 10;
  constexpr std::size_t kDayTempChars = 4;
  constexpr std::size_t kTempChars = 5; // width reserved for the big temperature
  constexpr std::size_t kIsoDateLength = 10;
  constexpr std::size_t kIsoMinuteLength = 16; // yyyy-mm-ddThh:mm

  // Hour bars: height runs from kMinBarFraction (coldest) to 1 (warmest); flat days sit at kFlatBarFraction.
  constexpr double kMinBarFraction = 0.3;
  constexpr double kFlatTemperatureSpan = 0.01;
  constexpr float kFlatBarFraction = 0.6F;
  constexpr float kHourBarWidthFraction = 0.4F;
  constexpr float kMinBarThickness = 3.0F; // px
  constexpr float kMinBarLength = 2.0F;    // px

  constexpr double kKmhToMph = 0.621371;
  constexpr float kSecondsPerMillisecond = 0.001F;

  // Open-Meteo wind bearings are where the wind comes from: eight 45° sectors.
  constexpr int kDegreesPerCircle = 360;
  constexpr double kDegreesPerSector = 45.0;
  constexpr std::size_t kCompassPoints = 8;

  [[nodiscard]] std::size_t compassSector(int degrees) {
    const int normalized = ((degrees % kDegreesPerCircle) + kDegreesPerCircle) % kDegreesPerCircle;
    return static_cast<std::size_t>(std::lround(normalized / kDegreesPerSector)) % kCompassPoints;
  }

  const char* windDirectionLabel(int degrees) {
    static constexpr std::array<const char*, kCompassPoints> kDirs = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return kDirs[compassSector(degrees)];
  }

  // Arrow for the direction the wind blows toward (opposite of the meteorological "from" bearing).
  const char* windArrow(int degrees) {
    static constexpr std::array<const char*, kCompassPoints> kArrows = {"↓", "↙", "←", "↖", "↑", "↗", "→", "↘"};
    return kArrows[compassSector(degrees)];
  }

  int roundedInt(double value) { return static_cast<int>(std::lround(value)); }

  ColorSpec accent() { return colorSpecFromRole(ColorRole::Primary); }
  ColorSpec text() { return colorSpecFromRole(ColorRole::OnSurface); }
  ColorSpec dim() { return term_frame::dimColor(); }
  ColorSpec trackColor() { return colorSpecFromRole(ColorRole::Outline, kTrackAlpha); }

  std::unique_ptr<Label> monoLabel(float size, const ColorSpec& color, Label** out = nullptr, std::string value = {}) {
    return ui::label({
        .out = out,
        .text = std::move(value),
        .fontSize = size,
        .fontFamily = std::string(term_frame::kFont),
        .color = color,
        .maxLines = 1,
        .ellipsize = TextEllipsize::End,
    });
  }

  // A fixed-size slot whose single absolutely positioned Box is sized from data in positionBars().
  std::unique_ptr<Flex> barTrack(Flex** trackOut, Box** barOut, const ColorSpec& trackFill, const ColorSpec& barFill) {
    auto track = ui::row({.out = trackOut, .fill = trackFill, .radius = 0.0F});
    track->addChild(
        ui::box({
            .out = barOut,
            .fill = barFill,
            .radius = 0.0F,
            .participatesInLayout = false,
        })
    );
    return track;
  }

  // A report box: the shared terminal frame with the panel's near-opaque fill.
  std::unique_ptr<Flex> reportBox(std::string_view numeral, std::string_view title, float scale, Flex** out) {
    auto box = term_frame::box(numeral, title, scale, out);
    box->setFill(colorSpecFromRole(ColorRole::Surface, kBoxFillAlpha));
    return box;
  }

  std::string timeFormatOf(const ConfigService* config) {
    return config != nullptr && !config->config().shell.timeFormat.empty() ? config->config().shell.timeFormat
                                                                           : std::string("%H:%M");
  }

  [[nodiscard]] bool isTwelveHourFormat(const std::string& format) {
    return format.find("%I") != std::string::npos
        || format.find("%l") != std::string::npos
        || format.find("%p") != std::string::npos
        || format.find("%r") != std::string::npos;
  }

} // namespace

WeatherTab::WeatherTab(WeatherService* weather, ConfigService* config) : m_weather(weather), m_config(config) {}

std::unique_ptr<Flex> WeatherTab::create() {
  const float scale = contentScale();

  auto root = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = kBoxGap * scale,
  });
  addEffectLayer();
  root->addChild(buildStateBox(scale));

  auto data = ui::column({
      .out = &m_data,
      .align = FlexAlign::Stretch,
      .gap = kBoxGap * scale,
      .visible = false,
  });
  data->addChild(buildNowBox(scale));
  data->addChild(buildHoursBox(scale));
  data->addChild(buildDaysBox(scale));
  root->addChild(std::move(data));
  return root;
}

// Weather animation behind the whole report, then a surface veil so the text on top stays legible.
void WeatherTab::addEffectLayer() {
  auto effect = std::make_unique<EffectNode>();
  effect->setParticipatesInLayout(false);
  effect->setZIndex(kBehindContentZ);
  effect->setVisible(false);
  effect->setRadius(0.0F);
  m_effectNode = static_cast<EffectNode*>(m_root->addChild(std::move(effect)));
  m_root->addChild(
      ui::box({
          .out = &m_veil,
          .fill = colorSpecFromRole(ColorRole::Surface, kVeilAlpha),
          .radius = 0.0F,
          .participatesInLayout = false,
      })
  );
  m_veil->setZIndex(kBehindContentZ + 1);
}

// Shown instead of the report while there is no data: disabled, no location, loading, unavailable.
std::unique_ptr<Flex> WeatherTab::buildStateBox(float scale) {
  const float font = Style::fontSizeCaption * scale;
  auto box = reportBox("¹", "weather", scale, &m_stateBox);
  box->addChild(monoLabel(font, text(), &m_stateTitle));
  box->addChild(monoLabel(font, dim(), &m_stateBody));
  m_stateBody->setMaxLines(4);
  m_stateBody->setEllipsize(TextEllipsize::End);
  return box;
}

std::unique_ptr<Flex> WeatherTab::buildNowBox(float scale) {
  const float font = Style::fontSizeCaption * scale;
  auto box = reportBox("¹", "now", scale, &m_nowBox);
  auto row = ui::row({
      .out = &m_nowRow,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });
  row->addChild(
      ui::label({
          .out = &m_temp,
          .text = "--",
          .fontSize = kTempFontSize * scale,
          .fontWeight = FontWeight::Bold,
          .fontFamily = std::string(term_frame::kFont),
          .color = accent(),
          .maxLines = 1,
      })
  );
  row->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "weather-cloud",
          .glyphSize = kNowGlyphSize * scale,
          .color = accent(),
      })
  );
  row->addChild(monoLabel(font, text(), &m_condition));
  m_condition->setFlexGrow(1.0F);
  box->addChild(std::move(row));
  box->addChild(monoLabel(font, dim(), &m_feels));
  box->addChild(term_frame::keyValueRow("wind", scale, m_wind, kKeyChars));
  box->addChild(term_frame::keyValueRow("hum", scale, m_humidity, kKeyChars));
  box->addChild(term_frame::dottedMeterRow("uv", scale, m_uv));
  box->addChild(term_frame::keyValueRow("sun", scale, m_sun, kKeyChars));
  return box;
}

std::unique_ptr<Flex> WeatherTab::buildHoursBox(float scale) {
  const float mini = Style::fontSizeMini * scale;
  auto box = reportBox("²", "hours", scale, &m_hoursBox);
  auto strip = ui::row({
      .out = &m_hoursStrip,
      .align = FlexAlign::Start,
      .justify = FlexJustify::Center,
      .fillWidth = true,
  });
  for (auto& hour : m_hours) {
    auto column = ui::column({
        .out = &hour.column,
        .align = FlexAlign::Center,
        .gap = kHourColumnGap * scale,
    });
    column->addChild(monoLabel(mini, dim(), &hour.hour));
    column->addChild(
        ui::glyph({
            .out = &hour.glyph,
            .glyph = "weather-cloud",
            .glyphSize = kGlyphSize * scale,
            .color = dim(),
        })
    );
    column->addChild(barTrack(&hour.barTrack, &hour.bar, clearColorSpec(), dim()));
    column->addChild(monoLabel(mini, text(), &hour.temp));
    column->addChild(barTrack(&hour.rainTrack, &hour.rain, trackColor(), accent()));
    column->addChild(monoLabel(mini, dim(), &hour.percent));
    strip->addChild(std::move(column));
  }
  box->addChild(std::move(strip));
  box->addChild(monoLabel(mini, dim(), &m_dryLabel, "dry next 12 h"));
  m_dryLabel->setVisible(false);
  return box;
}

std::unique_ptr<Flex> WeatherTab::buildDaysBox(float scale) {
  const float font = Style::fontSizeCaption * scale;
  auto box = reportBox("³", "days", scale, &m_daysBox);
  for (auto& day : m_days) {
    auto row = ui::row({
        .out = &day.row,
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
    });
    row->addChild(monoLabel(font, text(), &day.day));
    row->addChild(
        ui::glyph({
            .out = &day.glyph,
            .glyph = "weather-cloud",
            .glyphSize = kGlyphSize * scale,
            .color = dim(),
        })
    );
    row->addChild(monoLabel(font, dim(), &day.condition));
    row->addChild(monoLabel(font, text(), &day.low));
    row->addChild(barTrack(&day.track, &day.bar, trackColor(), dim()));
    row->addChild(monoLabel(font, text(), &day.high));
    day.low->setTextAlign(TextAlign::End);
    box->addChild(std::move(row));
  }
  return box;
}

void WeatherTab::onClose() {
  m_root = nullptr;
  m_data = nullptr;
  m_stateBox = nullptr;
  m_stateTitle = nullptr;
  m_stateBody = nullptr;
  m_nowBox = nullptr;
  m_effectNode = nullptr;
  m_veil = nullptr;
  m_activeEffect = EffectType::None;
  m_shaderTime = 0.0F;
  m_dryLabel = nullptr;
  m_nowRow = nullptr;
  m_temp = nullptr;
  m_glyph = nullptr;
  m_condition = nullptr;
  m_feels = nullptr;
  m_wind = {};
  m_humidity = {};
  m_uv = {};
  m_sun = {};
  m_hoursBox = nullptr;
  m_hoursStrip = nullptr;
  m_hours.fill({});
  m_daysBox = nullptr;
  m_days.fill({});
  m_shape = {};
  m_laidOutWidth = 0.0F;
  m_hourCount = 0;
  m_dayCount = 0;
  m_todayRow = kDayCount;
}

void WeatherTab::doLayout(Renderer& renderer, float contentWidth, float /*bodyHeight*/) {
  if (m_root == nullptr) {
    return;
  }
  const float scale = contentScale();
  // The control center hands over its full body; the report keeps its compact width, left-aligned.
  const float width = std::max(1.0F, std::min(contentWidth, kMaxWidth * scale));
  const float inner = std::max(1.0F, width - 2.0F * term_frame::boxInset(scale));

  m_root->setGap(kBoxGap * scale);
  m_data->setGap(kBoxGap * scale);
  for (Flex* box : {m_stateBox, m_nowBox, m_hoursBox}) {
    box->setGap(kInnerGap * scale);
  }
  m_daysBox->setGap(0.0F);
  m_stateTitle->setMaxWidth(inner);
  m_stateBody->setMaxWidth(inner);

  layoutNow(inner, scale);
  layoutHours(inner, scale);
  layoutDays(inner, scale);

  m_laidOutWidth = width;
  relayoutReport(renderer);
}

void WeatherTab::layoutNow(float inner, float scale) {
  const float gap = Style::spaceSm * scale;
  const float rowHeight = kRowHeight * scale;
  const float tempFont = kTempFontSize * scale;
  m_temp->setFontSize(tempFont);
  m_glyph->setGlyphSize(kNowGlyphSize * scale);
  term_frame::pinSize(*m_nowRow, inner, kNowHeight * scale);
  m_condition->setMaxWidth(
      std::max(1.0F, inner - term_frame::monoWidth(kTempChars, tempFont) - kNowGlyphSize * scale - 2.0F * gap)
  );
  m_feels->setMaxWidth(inner);
  term_frame::layoutKeyValueRowH(m_wind, inner, rowHeight, scale, kKeyChars);
  term_frame::layoutKeyValueRowH(m_sun, inner, rowHeight, scale, kKeyChars);
  term_frame::layoutKeyValueRowH(m_humidity, inner, rowHeight, scale, kKeyChars);
  term_frame::layoutDottedMeterRow(m_uv, inner, scale);
}

void WeatherTab::layoutHours(float inner, float scale) {
  const float mini = Style::fontSizeMini * scale;
  const float gap = Style::spaceSm * scale;
  const float colWidth = std::max(1.0F, std::floor(inner / static_cast<float>(kHourCount)));
  m_dryLabel->setFontSize(mini);
  m_dryLabel->setMaxWidth(inner);
  for (auto& hour : m_hours) {
    hour.column->setMinWidth(colWidth);
    hour.column->setMaxWidth(colWidth);
    for (Label* label : {hour.hour, hour.temp, hour.percent}) {
      label->setFontSize(mini);
      label->setMaxWidth(colWidth);
    }
    term_frame::pinSize(*hour.barTrack, colWidth, kBarHeight * scale);
    term_frame::pinSize(*hour.rainTrack, std::max(1.0F, colWidth - 2.0F * gap), kRainHeight * scale);
  }
}

// One row: day | glyph | condition | low | range track | high, separated by five gaps. The condition column
// shrinks before the range track drops below its minimum.
void WeatherTab::layoutDays(float inner, float scale) {
  constexpr float kRowGaps = 5.0F;
  const float font = Style::fontSizeCaption * scale;
  const float gap = Style::spaceSm * scale;
  const float dayWidth = term_frame::monoWidth(kDayChars, font);
  const float tempWidth = term_frame::monoWidth(kDayTempChars, font);
  const float minTrack = kMinDayTrackWidth * scale;
  float conditionWidth = term_frame::monoWidth(kConditionChars, font);
  const float fixed = dayWidth + kGlyphSize * scale + 2.0F * tempWidth + kRowGaps * gap;
  if (inner - fixed - conditionWidth < minTrack) {
    conditionWidth = std::max(1.0F, inner - fixed - minTrack);
  }
  const float trackWidth = std::max(1.0F, inner - fixed - conditionWidth);
  for (auto& day : m_days) {
    term_frame::pinSize(*day.row, inner, kRowHeight * scale);
    for (auto [label, labelWidth] :
         {std::pair{day.day, dayWidth}, std::pair{day.condition, conditionWidth}, std::pair{day.low, tempWidth},
          std::pair{day.high, tempWidth}}) {
      label->setFontSize(font);
      label->setMinWidth(labelWidth);
      label->setMaxWidth(labelWidth);
    }
    term_frame::pinSize(*day.track, trackWidth, kDayBarHeight * scale);
  }
}

// Lays the report out at the remembered width and records its content height (logical px) for quickPanelHeight().
void WeatherTab::relayoutReport(Renderer& renderer) {
  m_root->setSize(m_laidOutWidth, 0.0F);
  m_root->layout(renderer);
  positionBars();
  updateEffectFrame();
  const float height = m_root->height() / std::max(0.01F, contentScale());
  if (std::abs(height - m_contentHeight) > 0.5F) {
    m_contentHeight = height;
    m_resizePending = true;
  }
}

float WeatherTab::quickPanelHeight(float /*width*/, float fallback) const {
  return m_contentHeight > 0.0F ? m_contentHeight : fallback;
}

// Sizes the weather animation and its veil. The effect is clipped to the hero box for every weather type: the
// boxes below are opaque, so anything drawn further down only showed as pale strips in the gaps between sections.
void WeatherTab::updateEffectFrame() {
  if (m_effectNode == nullptr || m_veil == nullptr || m_root == nullptr) {
    return;
  }
  const float w = std::max(1.0F, m_root->width());
  const float h = (m_nowBox != nullptr && m_data != nullptr && m_nowBox->height() > 0.0F)
      ? std::max(1.0F, m_data->y() + m_nowBox->y() + m_nowBox->height())
      : 1.0F;
  m_effectNode->setPosition(0.0F, 0.0F);
  m_effectNode->setFrameSize(w, h);
  m_veil->setPosition(0.0F, 0.0F);
  m_veil->setSize(w, h);
}

void WeatherTab::doUpdate(Renderer& renderer) {
  sync();
  if (m_root == nullptr) {
    return;
  }
  // The visible rows changed (state box <-> report, fewer hours or days): measure again.
  const ShapeKey shape{
      .reportVisible = m_data->visible(),
      .dryLabelVisible = m_dryLabel->visible(),
      .stateBodyVisible = m_stateBody->visible(),
      .hourCount = m_hourCount,
      .dayCount = m_dayCount,
  };
  if (shape != m_shape && m_laidOutWidth > 0.0F) {
    m_shape = shape;
    relayoutReport(renderer);
  }
  // Last, as a reopen may tear this tab's nodes down.
  if (m_resizePending) {
    m_resizePending = false;
    requestQuickPanelResize();
  }
}

void WeatherTab::showState(const std::string& title, const std::string& body) {
  hideEffect();
  m_data->setVisible(false);
  m_stateBox->setVisible(true);
  m_stateTitle->setText(term_frame::lowerAscii(title));
  m_stateBody->setText(term_frame::lowerAscii(body));
  m_stateBody->setVisible(!body.empty());
}

void WeatherTab::sync() {
  if (m_root == nullptr) {
    return;
  }
  if (m_weather == nullptr || !m_weather->enabled()) {
    showState(i18n::tr("control-center.weather.disabled"), "");
    return;
  }
  if (!m_weather->locationConfigured()) {
    showState(
        i18n::tr("control-center.weather.no-location-title"), i18n::tr("control-center.weather.no-location-body")
    );
    return;
  }
  const WeatherSnapshot& snapshot = m_weather->snapshot();
  if (!snapshot.valid) {
    showState(
        m_weather->loading() ? i18n::tr("control-center.weather.fetching")
                             : i18n::tr("control-center.weather.data-unavailable"),
        m_weather->error()
    );
    return;
  }

  m_data->setVisible(true);
  m_stateBox->setVisible(false);
  syncNow(snapshot);
  syncEffect(snapshot);
  syncHours(snapshot);
  syncDays(snapshot);
  positionBars();
}

std::string WeatherTab::temperatureText(double celsius) const {
  return std::format("{}°", roundedInt(m_weather->displayTemperature(celsius)));
}

std::string WeatherTab::hourLabel(const std::string& isoTime, bool twelveHour) {
  if (isoTime.size() < kIsoMinuteLength) {
    return isoTime;
  }
  const std::string label = formatIsoTime(isoTime, twelveHour ? "%I" : "%H");
  return label.empty() ? isoTime : label;
}

void WeatherTab::syncNow(const WeatherSnapshot& snapshot) {
  const auto& current = snapshot.current;
  m_temp->setText(
      std::format(
          "{}{}", roundedInt(m_weather->displayTemperature(current.temperatureC)), m_weather->displayTemperatureUnit()
      )
  );
  m_glyph->setGlyph(WeatherService::glyphForCode(current.weatherCode, current.isDay));
  m_condition->setText(term_frame::lowerAscii(WeatherService::descriptionForCode(current.weatherCode)));
  m_feels->setText(feelsText(snapshot));
  m_wind.value->setText(windText(snapshot));
  m_humidity.value->setText(std::format("{}%", current.relativeHumidityPercent));
  // UV only matters in daylight.
  m_uv.row->setVisible(current.isDay);
  term_frame::setDottedMeterRow(
      m_uv, std::clamp(current.uvIndex / kUvScale, 0.0, 1.0), std::format("{:.1f}", current.uvIndex)
  );

  if (snapshot.forecastDays.empty()) {
    m_sun.value->setText("--");
    return;
  }
  const std::string format = timeFormatOf(m_config);
  const auto& today = snapshot.forecastDays.front();
  m_sun.value->setText(
      "rise "
      + formatIsoTime(today.sunriseIso, format.c_str())
      + "  set "
      + formatIsoTime(today.sunsetIso, format.c_str())
  );
}

// "feels 12°  min 8°  max 15°"; either part is dropped when unknown.
std::string WeatherTab::feelsText(const WeatherSnapshot& snapshot) const {
  std::string feels;
  if (snapshot.current.apparentTemperatureC.has_value()) {
    feels = "feels " + temperatureText(*snapshot.current.apparentTemperatureC);
  }
  if (!snapshot.forecastDays.empty()) {
    const auto& today = snapshot.forecastDays.front();
    feels += std::string(feels.empty() ? "" : "  ")
        + "min "
        + temperatureText(today.temperatureMinC)
        + "  max "
        + temperatureText(today.temperatureMaxC);
  }
  return feels;
}

// "↙ 12 km/h from NE": the arrow shows where the wind blows toward.
std::string WeatherTab::windText(const WeatherSnapshot& snapshot) const {
  const auto& current = snapshot.current;
  const bool imperial = m_weather->useImperial();
  const double speed = imperial ? current.windSpeedKmh * kKmhToMph : current.windSpeedKmh;
  const std::string unit =
      imperial ? "mph" : (snapshot.currentUnits.windSpeed.empty() ? "km/h" : snapshot.currentUnits.windSpeed);
  return std::format(
      "{} {} {} from {}", windArrow(current.windDirectionDeg), roundedInt(speed), unit,
      windDirectionLabel(current.windDirectionDeg)
  );
}

void WeatherTab::syncHours(const WeatherSnapshot& snapshot) {
  m_hourCount = std::min(kHourCount, snapshot.forecastHours.size());
  const auto hours = std::span(snapshot.forecastHours).first(m_hourCount);
  const auto [coldest, warmest] =
      std::minmax_element(hours.begin(), hours.end(), [](const WeatherForecastHour& a, const WeatherForecastHour& b) {
        return a.temperatureC < b.temperatureC;
      });
  const double minTemp = hours.empty() ? 0.0 : coldest->temperatureC;
  const double span = hours.empty() ? 0.0 : warmest->temperatureC - minTemp;
  // Without any chance of rain the rain bars give way to one "dry" line.
  const bool dry = !hours.empty() && std::all_of(hours.begin(), hours.end(), [](const WeatherForecastHour& hour) {
    return hour.precipitationProbabilityPercent <= 0;
  });
  m_dryLabel->setVisible(dry);
  const bool twelveHour = isTwelveHourFormat(timeFormatOf(m_config));

  for (std::size_t i = 0; i < kHourCount; ++i) {
    HourColumn& column = m_hours[i];
    const bool used = i < m_hourCount;
    column.column->setVisible(used);
    if (!used) {
      continue;
    }
    const auto& hour = hours[i];
    const bool now = i == 0;
    const ColorSpec tone = now ? accent() : dim();
    column.hour->setText(hourLabel(hour.timeIso, twelveHour));
    column.hour->setColor(tone);
    column.glyph->setGlyph(WeatherService::glyphForCode(hour.weatherCode, hour.isDay));
    column.glyph->setColor(tone);
    column.temp->setText(temperatureText(hour.temperatureC));
    column.percent->setVisible(!dry);
    column.rainTrack->setVisible(!dry);
    column.percent->setText(std::format("{}%", hour.precipitationProbabilityPercent));
    column.bar->setFill(tone);
    column.barFraction = span > kFlatTemperatureSpan
        ? static_cast<float>(kMinBarFraction + (1.0 - kMinBarFraction) * (hour.temperatureC - minTemp) / span)
        : kFlatBarFraction;
    column.rainFraction = static_cast<float>(std::clamp(hour.precipitationProbabilityPercent, 0, 100)) / 100.0F;
  }
}

void WeatherTab::syncDays(const WeatherSnapshot& snapshot) {
  m_dayCount = std::min(kDayCount, snapshot.forecastDays.size());
  const auto days = std::span(snapshot.forecastDays).first(m_dayCount);
  const auto lowest = [](const WeatherForecastDay& a, const WeatherForecastDay& b) {
    return a.temperatureMinC < b.temperatureMinC;
  };
  const auto highest = [](const WeatherForecastDay& a, const WeatherForecastDay& b) {
    return a.temperatureMaxC < b.temperatureMaxC;
  };
  const double weekMin = days.empty() ? 0.0 : std::min_element(days.begin(), days.end(), lowest)->temperatureMinC;
  const double weekMax = days.empty() ? 0.0 : std::max_element(days.begin(), days.end(), highest)->temperatureMaxC;
  const double span = std::max(kFlatTemperatureSpan, weekMax - weekMin);
  const std::string today = todayIso(snapshot.utcOffsetSeconds);

  m_todayRow = kDayCount;
  for (std::size_t i = 0; i < kDayCount; ++i) {
    DayRow& row = m_days[i];
    const bool used = i < m_dayCount;
    row.row->setVisible(used);
    if (!used) {
      continue;
    }
    const auto& day = days[i];
    const bool isToday = day.dateIso == today;
    if (isToday) {
      m_todayRow = i;
    }
    const ColorSpec label = isToday ? accent() : text();
    const ColorSpec muted = isToday ? accent() : dim();
    row.day->setText(weekdayLabel(day.dateIso));
    row.day->setColor(label);
    row.glyph->setGlyph(WeatherService::glyphForCode(day.weatherCode, true));
    row.glyph->setColor(muted);
    row.condition->setText(term_frame::lowerAscii(WeatherService::shortDescriptionForCode(day.weatherCode)));
    row.condition->setColor(muted);
    row.low->setText(temperatureText(day.temperatureMinC));
    row.low->setColor(label);
    row.high->setText(temperatureText(day.temperatureMaxC));
    row.high->setColor(label);
    row.bar->setFill(isToday ? accent() : dim());
    row.rangeStart = static_cast<float>((day.temperatureMinC - weekMin) / span);
    row.rangeEnd = static_cast<float>((day.temperatureMaxC - weekMin) / span);
  }
}

std::string WeatherTab::quickPanelHints() const {
  if (m_weather == nullptr
      || !m_weather->enabled()
      || !m_weather->locationConfigured()
      || !m_weather->snapshot().valid) {
    return {};
  }
  const WeatherSnapshot& snapshot = m_weather->snapshot();
  const std::string format = timeFormatOf(m_config);
  const auto fetched = std::chrono::duration_cast<std::chrono::seconds>(snapshot.fetchedAt.time_since_epoch());
  std::string updated = formatLocalUnixTime(fetched.count(), format);
  std::erase_if(updated, [](char c) { return c == '[' || c == ']'; });
  const auto first = updated.find_first_not_of(' ');
  const auto last = updated.find_last_not_of(' ');
  updated = first == std::string::npos ? std::string{} : updated.substr(first, last - first + 1);
  std::string footer = "updated " + updated;

  const auto age =
      std::chrono::duration_cast<std::chrono::minutes>(std::chrono::system_clock::now() - snapshot.fetchedAt);
  const long long minutes = std::max<long long>(0, age.count());
  footer += minutes < 60 ? std::format(" · {}m", minutes) : std::format(" · {}h", minutes / 60);

  if (m_weather->loading()) {
    footer += " · refreshing";
  } else if (!m_weather->error().empty()) {
    footer += " · " + term_frame::lowerAscii(m_weather->error());
  }
  return footer;
}

// Sizes the data-driven bars inside their fixed tracks. Needs only the pinned track sizes, so it runs
// after layout and again whenever the data changes.
void WeatherTab::positionBars() {
  if (m_root == nullptr) {
    return;
  }
  for (std::size_t i = 0; i < m_hourCount; ++i) {
    HourColumn& hour = m_hours[i];
    const float trackWidth = hour.barTrack->width();
    const float trackHeight = hour.barTrack->height();
    if (trackWidth > 0.0F && trackHeight > 0.0F) {
      const float barWidth = std::max(kMinBarThickness, std::floor(trackWidth * kHourBarWidthFraction));
      const float barHeight = std::clamp(std::round(trackHeight * hour.barFraction), kMinBarLength, trackHeight);
      hour.bar->setPosition(std::floor((trackWidth - barWidth) * 0.5F), trackHeight - barHeight);
      hour.bar->setSize(barWidth, barHeight);
    }
    const float rainWidth = hour.rainTrack->width();
    const float rainHeight = hour.rainTrack->height();
    const float filled = std::round(rainWidth * hour.rainFraction);
    hour.rain->setPosition(0.0F, 0.0F);
    hour.rain->setSize(filled, rainHeight);
    hour.rain->setVisible(filled >= 1.0F);
  }
  for (std::size_t i = 0; i < m_dayCount; ++i) {
    DayRow& day = m_days[i];
    const float trackWidth = day.track->width();
    const float trackHeight = day.track->height();
    if (trackWidth <= 0.0F || trackHeight <= 0.0F) {
      continue;
    }
    const float barWidth =
        std::min(trackWidth, std::max(kMinBarThickness, std::round((day.rangeEnd - day.rangeStart) * trackWidth)));
    const float x = std::clamp(std::round(day.rangeStart * trackWidth), 0.0F, trackWidth - barWidth);
    day.bar->setPosition(x, 0.0F);
    day.bar->setSize(barWidth, trackHeight);
  }
}

std::string WeatherTab::todayIso(std::int32_t utcOffsetSeconds) {
  const auto now = std::chrono::system_clock::now() + std::chrono::seconds{utcOffsetSeconds};
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&time, &tm);

  return formatStrftime("%Y-%m-%d", tm);
}

std::string WeatherTab::weekdayLabel(const std::string& isoDate) {
  if (isoDate.size() != kIsoDateLength) {
    return isoDate;
  }
  // yyyy-mm-dd; a malformed field leaves the raw text rather than throwing out of the update loop.
  const auto field = [&isoDate](std::size_t offset, std::size_t length) -> std::optional<int> {
    int value = 0;
    const char* begin = isoDate.data() + offset;
    const auto [end, error] = std::from_chars(begin, begin + length, value);
    return error == std::errc{} && end == begin + length ? std::optional<int>(value) : std::nullopt;
  };
  const auto year = field(0, 4);
  const auto month = field(5, 2);
  const auto day = field(8, 2);
  if (!year || !month || !day) {
    return isoDate;
  }

  std::tm tm{};
  tm.tm_year = *year - 1900;
  tm.tm_mon = *month - 1;
  tm.tm_mday = *day;
  if (std::mktime(&tm) == -1) {
    return isoDate;
  }

  const std::string weekday = term_frame::lowerAscii(formatStrftime("%a", tm));
  return weekday.empty() ? isoDate : weekday;
}

void WeatherTab::syncEffect(const WeatherSnapshot& snapshot) {
  if (m_effectNode == nullptr) {
    return;
  }
  const EffectType effect = m_weather->effectsEnabled()
      ? effectForWeatherCode(snapshot.current.weatherCode, snapshot.current.isDay)
      : EffectType::None;
  if (effect != m_activeEffect) {
    m_activeEffect = effect;
    m_shaderTime = 0.0F;
  }
  const bool wasVisible = m_effectNode->visible();
  m_effectNode->setEffectType(m_activeEffect);
  m_effectNode->setBgColor(colorForRole(ColorRole::Surface));
  m_effectNode->setVisible(m_activeEffect != EffectType::None);
  if (m_activeEffect != EffectType::None && !wasVisible) {
    // Start the tick loop; onFrameTick keeps it going for as long as the effect is visible.
    PanelManager::instance().requestFrameTick();
    PanelManager::instance().requestRedraw();
  }
  if (m_veil != nullptr) {
    m_veil->setVisible(m_activeEffect != EffectType::None);
  }
  // The effect type may just have switched (e.g. to Stars); re-clip right away instead of waiting for a layout.
  updateEffectFrame();
}

void WeatherTab::hideEffect() {
  m_activeEffect = EffectType::None;
  m_shaderTime = 0.0F;
  if (m_effectNode != nullptr) {
    m_effectNode->setEffectType(EffectType::None);
    m_effectNode->setVisible(false);
  }
  if (m_veil != nullptr) {
    m_veil->setVisible(false);
  }
}

void WeatherTab::onFrameTick(float deltaMs) {
  if (m_effectNode == nullptr || !m_effectNode->visible() || m_activeEffect == EffectType::None) {
    return;
  }
  m_shaderTime += deltaMs * kSecondsPerMillisecond;
  m_effectNode->setTime(m_shaderTime);
  // A redraw arms the surface's next frame tick; without it a hosted panel renders one frame and stops.
  PanelManager::instance().requestRedraw();
}

EffectType WeatherTab::effectForWeatherCode(std::int32_t code, bool isDay) {
  // Open-Meteo WMO codes: 0 clear, 1 mainly clear, 2 partly cloudy, 3 overcast, 45/48 fog,
  // 51-67 drizzle and rain, 71-77 snow, 80-82 showers, 85-86 snow showers, 95-99 thunderstorm.
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82) || (code >= 95 && code <= 99)) {
    return EffectType::Rain;
  }
  if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) {
    return EffectType::Snow;
  }
  if (code == 2 && !isDay) {
    return EffectType::Stars;
  }
  if (code == 2 || code == 3) {
    return EffectType::Cloud;
  }
  if (code >= 40 && code <= 49) {
    return EffectType::Fog;
  }
  if (code == 0 || code == 1) {
    return isDay ? EffectType::Sun : EffectType::Stars;
  }
  return EffectType::None;
}

std::string WeatherTab::quickPanelStatus() const {
  if (m_weather == nullptr || !m_weather->enabled() || !m_weather->locationConfigured()) {
    return {};
  }
  if (m_config != nullptr && !m_config->config().shell.showLocation) {
    return {};
  }
  const WeatherSnapshot& snapshot = m_weather->snapshot();
  if (!snapshot.valid) {
    return {};
  }
  return term_frame::lowerAscii(
      snapshot.locationName.empty() ? i18n::tr("location.locations.current") : snapshot.locationName
  );
}
