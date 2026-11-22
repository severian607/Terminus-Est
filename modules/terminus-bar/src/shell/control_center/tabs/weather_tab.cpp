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
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <format>
#include <memory>

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
  constexpr float kBarHeight = 22.0F;
  constexpr float kUvScale = 11.0F;
  constexpr std::size_t kKeyChars = 10;
  constexpr std::size_t kDayChars = 4;
  constexpr std::size_t kConditionChars = 10;
  constexpr std::size_t kDayTempChars = 4;

  const char* windDirectionLabel(int degrees) {
    static constexpr std::array<const char*, 8> kDirs = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const int normalized = ((degrees % 360) + 360) % 360;
    const int index = static_cast<int>(std::lround(normalized / 45.0)) % 8;
    return kDirs[static_cast<std::size_t>(index)];
  }

  // Arrow for the direction the wind blows toward (opposite of the meteorological "from" bearing).
  const char* windArrow(int degrees) {
    static constexpr std::array<const char*, 8> kArrows = {"↓", "↙", "←", "↖", "↑", "↗", "→", "↘"};
    const int normalized = ((degrees % 360) + 360) % 360;
    const int index = static_cast<int>(std::lround(normalized / 45.0)) % 8;
    return kArrows[static_cast<std::size_t>(index)];
  }

  ColorSpec accent() { return colorSpecFromRole(ColorRole::Primary); }
  ColorSpec text() { return colorSpecFromRole(ColorRole::OnSurface); }

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

  std::string timeFormatOf(const ConfigService* config) {
    return config != nullptr && !config->config().shell.timeFormat.empty() ? config->config().shell.timeFormat
                                                                             : std::string("%H:%M");
  }

} // namespace

WeatherTab::WeatherTab(WeatherService* weather, ConfigService* config) : m_weather(weather), m_config(config) {}

std::unique_ptr<Flex> WeatherTab::create() {
  const float scale = contentScale();
  const float font = Style::fontSizeCaption * scale;
  const float mini = Style::fontSizeMini * scale;
  const ColorSpec dim = term_frame::dimColor();
  const ColorSpec track = colorSpecFromRole(ColorRole::Outline, 0.5F);

  auto root = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = kBoxGap * scale,
  });
  {
    // Weather animation behind the whole report, then a surface veil so the text on top stays legible.
    auto effect = std::make_unique<EffectNode>();
    effect->setParticipatesInLayout(false);
    effect->setZIndex(-2);
    effect->setVisible(false);
    effect->setRadius(0.0F);
    m_effectNode = static_cast<EffectNode*>(m_root->addChild(std::move(effect)));
    m_root->addChild(ui::box({
        .out = &m_veil,
        .fill = colorSpecFromRole(ColorRole::Surface, 0.25F),
        .radius = 0.0F,
        .participatesInLayout = false,
    }));
    m_veil->setZIndex(-1);
  }

  // Shown instead of the report while there is no data: disabled, no location, loading, unavailable.
  auto stateBox = term_frame::box("¹", "weather", scale, &m_stateBox);
  stateBox->setFill(colorSpecFromRole(ColorRole::Surface, 0.94F));
  stateBox->addChild(monoLabel(font, text(), &m_stateTitle));
  stateBox->addChild(monoLabel(font, dim, &m_stateBody));
  m_stateBody->setMaxLines(4);
  m_stateBody->setEllipsize(TextEllipsize::End);
  root->addChild(std::move(stateBox));

  auto data = ui::column({
      .out = &m_data,
      .align = FlexAlign::Stretch,
      .gap = kBoxGap * scale,
      .visible = false,
  });

  // --- ¹now ---
  auto nowBox = term_frame::box("¹", "now", scale, &m_nowBox);
  nowBox->setFill(colorSpecFromRole(ColorRole::Surface, 0.94F));
  auto nowRow = ui::row({
      .out = &m_nowRow,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });
  nowRow->addChild(ui::label({
      .out = &m_temp,
      .text = "--",
      .fontSize = kTempFontSize * scale,
      .fontWeight = FontWeight::Bold,
      .fontFamily = std::string(term_frame::kFont),
      .color = accent(),
      .maxLines = 1,
  }));
  nowRow->addChild(ui::glyph({
      .out = &m_glyph,
      .glyph = "weather-cloud",
      .glyphSize = kNowGlyphSize * scale,
      .color = accent(),
  }));
  nowRow->addChild(monoLabel(font, text(), &m_condition));
  m_condition->setFlexGrow(1.0F);
  nowBox->addChild(std::move(nowRow));
  nowBox->addChild(monoLabel(font, dim, &m_feels));
  nowBox->addChild(term_frame::keyValueRow("wind", scale, m_wind, kKeyChars));
  nowBox->addChild(term_frame::keyValueRow("hum", scale, m_humidity, kKeyChars));
  nowBox->addChild(term_frame::dottedMeterRow("uv", scale, m_uv));
  nowBox->addChild(term_frame::keyValueRow("sun", scale, m_sun, kKeyChars));
  data->addChild(std::move(nowBox));

  // --- ²hours ---
  auto hoursBox = term_frame::box("²", "hours", scale, &m_hoursBox);
  hoursBox->setFill(colorSpecFromRole(ColorRole::Surface, 0.94F));
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
        .gap = 1.0F * scale,
    });
    column->addChild(monoLabel(mini, dim, &hour.hour));
    column->addChild(ui::glyph({
        .out = &hour.glyph,
        .glyph = "weather-cloud",
        .glyphSize = kGlyphSize * scale,
        .color = dim,
    }));
    column->addChild(barTrack(&hour.barTrack, &hour.bar, clearColorSpec(), dim));
    column->addChild(monoLabel(mini, text(), &hour.temp));
    column->addChild(barTrack(&hour.rainTrack, &hour.rain, track, accent()));
    column->addChild(monoLabel(mini, dim, &hour.percent));
    strip->addChild(std::move(column));
  }
  hoursBox->addChild(std::move(strip));
  hoursBox->addChild(monoLabel(mini, dim, &m_dryLabel, "dry next 12 h"));
  m_dryLabel->setVisible(false);
  data->addChild(std::move(hoursBox));

  // --- ³days ---
  auto daysBox = term_frame::box("³", "days", scale, &m_daysBox);
  daysBox->setFill(colorSpecFromRole(ColorRole::Surface, 0.94F));
  for (auto& day : m_days) {
    auto row = ui::row({
        .out = &day.row,
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
    });
    row->addChild(monoLabel(font, text(), &day.day));
    row->addChild(ui::glyph({
        .out = &day.glyph,
        .glyph = "weather-cloud",
        .glyphSize = kGlyphSize * scale,
        .color = dim,
    }));
    row->addChild(monoLabel(font, dim, &day.condition));
    row->addChild(monoLabel(font, text(), &day.low));
    row->addChild(barTrack(&day.track, &day.bar, track, dim));
    row->addChild(monoLabel(font, text(), &day.high));
    day.low->setTextAlign(TextAlign::End);
    daysBox->addChild(std::move(row));
  }
  data->addChild(std::move(daysBox));

  root->addChild(std::move(data));
  return root;
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
  m_shapeKey = 0;
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
  const float font = Style::fontSizeCaption * scale;
  const float mini = Style::fontSizeMini * scale;
  const float gap = Style::spaceSm * scale;
  // The control center hands over its full body; the report keeps its compact width, left-aligned.
  const float width = std::max(1.0F, std::min(contentWidth, kMaxWidth * scale));
  const float inner = std::max(1.0F, width - 2.0F * term_frame::boxInset(scale));
  const float rowHeight = kRowHeight * scale;

  m_root->setGap(kBoxGap * scale);
  m_data->setGap(kBoxGap * scale);
  for (Flex* box : {m_stateBox, m_nowBox, m_hoursBox}) {
    box->setGap(kInnerGap * scale);
  }
  m_daysBox->setGap(0.0F);

  m_stateTitle->setMaxWidth(inner);
  m_stateBody->setMaxWidth(inner);

  // ¹now
  const float tempFont = kTempFontSize * scale;
  m_temp->setFontSize(tempFont);
  m_glyph->setGlyphSize(kNowGlyphSize * scale);
  term_frame::pinSize(*m_nowRow, inner, kNowHeight * scale);
  m_condition->setMaxWidth(
      std::max(1.0F, inner - term_frame::monoWidth(5, tempFont) - kNowGlyphSize * scale - 2.0F * gap)
  );
  m_feels->setMaxWidth(inner);
  term_frame::layoutKeyValueRowH(m_wind, inner, rowHeight, scale, kKeyChars);
  term_frame::layoutKeyValueRowH(m_sun, inner, rowHeight, scale, kKeyChars);
  term_frame::layoutKeyValueRowH(m_humidity, inner, rowHeight, scale, kKeyChars);
  term_frame::layoutDottedMeterRow(m_uv, inner, scale);
  m_dryLabel->setFontSize(mini);
  m_dryLabel->setMaxWidth(inner);

  // ²hours
  const float colWidth = std::max(1.0F, std::floor(inner / static_cast<float>(kHourCount)));
  const float barHeight = kBarHeight * scale;
  for (auto& hour : m_hours) {
    hour.column->setMinWidth(colWidth);
    hour.column->setMaxWidth(colWidth);
    for (Label* label : {hour.hour, hour.temp, hour.percent}) {
      label->setFontSize(mini);
      label->setMaxWidth(colWidth);
    }
    term_frame::pinSize(*hour.barTrack, colWidth, barHeight);
    term_frame::pinSize(*hour.rainTrack, std::max(1.0F, colWidth - 2.0F * gap), kRainHeight * scale);
  }

  // ³days
  const float dayWidth = term_frame::monoWidth(kDayChars, font);
  const float tempWidth = term_frame::monoWidth(kDayTempChars, font);
  const float glyphWidth = kGlyphSize * scale;
  const float minTrack = 40.0F * scale;
  float conditionWidth = term_frame::monoWidth(kConditionChars, font);
  const float fixed = dayWidth + glyphWidth + 2.0F * tempWidth + 5.0F * gap;
  if (inner - fixed - conditionWidth < minTrack) {
    conditionWidth = std::max(1.0F, inner - fixed - minTrack);
  }
  const float trackWidth = std::max(1.0F, inner - fixed - conditionWidth);
  for (auto& day : m_days) {
    term_frame::pinSize(*day.row, inner, rowHeight);
    for (auto [label, labelWidth] : {std::pair{day.day, dayWidth}, std::pair{day.condition, conditionWidth},
                                     std::pair{day.low, tempWidth}, std::pair{day.high, tempWidth}}) {
      label->setFontSize(font);
      label->setMinWidth(labelWidth);
      label->setMaxWidth(labelWidth);
    }
    term_frame::pinSize(*day.track, trackWidth, kDayBarHeight * scale);
  }

  m_laidOutWidth = width;
  relayoutReport(renderer);
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

float WeatherTab::quickPanelHeight(float width, float fallback) const {
  (void)width;
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
  const std::size_t shape = (m_data->visible() ? 1U : 0U) | (m_dryLabel->visible() ? 2U : 0U) | (m_hourCount << 2U)
      | (m_dayCount << 10U) | (m_stateBody->visible() ? (1U << 18U) : 0U);
  if (shape != m_shapeKey && m_laidOutWidth > 0.0F) {
    m_shapeKey = shape;
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
  if (m_data->visible()) {
    m_data->setVisible(false);
  }
  if (!m_stateBox->visible()) {
    m_stateBox->setVisible(true);
  }
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

  if (!m_data->visible()) {
    m_data->setVisible(true);
  }
  if (m_stateBox->visible()) {
    m_stateBox->setVisible(false);
  }
  syncNow(snapshot);
  syncEffect(snapshot);
  syncHours(snapshot);
  syncDays(snapshot);
  positionBars();
}

std::string WeatherTab::temperatureText(double celsius) const {
  return std::format("{}°", static_cast<int>(std::lround(m_weather->displayTemperature(celsius))));
}

std::string WeatherTab::hourLabel(const std::string& isoTime) const {
  if (isoTime.size() < 16) {
    return isoTime;
  }
  const std::string format = timeFormatOf(m_config);
  const bool twelveHour = format.find("%I") != std::string::npos
      || format.find("%l") != std::string::npos
      || format.find("%p") != std::string::npos
      || format.find("%r") != std::string::npos;
  const std::string label = formatIsoTime(isoTime, twelveHour ? "%I" : "%H");
  return label.empty() ? isoTime : label;
}

void WeatherTab::syncNow(const WeatherSnapshot& snapshot) {
  const auto& current = snapshot.current;
  m_temp->setText(
      std::format(
          "{}{}", static_cast<int>(std::lround(m_weather->displayTemperature(current.temperatureC))),
          m_weather->displayTemperatureUnit()
      )
  );
  m_glyph->setGlyph(WeatherService::glyphForCode(current.weatherCode, current.isDay));
  m_condition->setText(term_frame::lowerAscii(WeatherService::descriptionForCode(current.weatherCode)));

  std::string feels;
  if (current.apparentTemperatureC.has_value()) {
    feels = "feels " + temperatureText(*current.apparentTemperatureC);
  }
  if (!snapshot.forecastDays.empty()) {
    const auto& today = snapshot.forecastDays.front();
    feels += std::string(feels.empty() ? "" : "  ") + "min " + temperatureText(today.temperatureMinC) + "  max "
        + temperatureText(today.temperatureMaxC);
  }
  m_feels->setText(feels);

  const bool imperial = m_weather->useImperial();
  const double windSpeed = imperial ? current.windSpeedKmh * 0.621371 : current.windSpeedKmh;
  const std::string windUnit =
      imperial ? "mph" : (snapshot.currentUnits.windSpeed.empty() ? "km/h" : snapshot.currentUnits.windSpeed);
  m_wind.value->setText(
      std::format(
          "{} {} {} from {}", windArrow(current.windDirectionDeg), static_cast<int>(std::lround(windSpeed)), windUnit,
          windDirectionLabel(current.windDirectionDeg)
      )
  );

  m_humidity.value->setText(std::format("{}%", current.relativeHumidityPercent));
  // UV only matters in daylight.
  if (m_uv.row->visible() != current.isDay) {
    m_uv.row->setVisible(current.isDay);
  }
  term_frame::setDottedMeterRow(m_uv, std::clamp(current.uvIndex / kUvScale, 0.0, 1.0), std::format("{:.1f}", current.uvIndex));

  if (snapshot.forecastDays.empty()) {
    m_sun.value->setText("--");
  } else {
    const std::string format = timeFormatOf(m_config);
    const auto& today = snapshot.forecastDays.front();
    m_sun.value->setText(
        "rise " + formatIsoTime(today.sunriseIso, format.c_str()) + "  set "
        + formatIsoTime(today.sunsetIso, format.c_str())
    );
  }
}

void WeatherTab::syncHours(const WeatherSnapshot& snapshot) {
  m_hourCount = std::min(kHourCount, snapshot.forecastHours.size());
  double minTemp = 0.0;
  double maxTemp = 0.0;
  for (std::size_t i = 0; i < m_hourCount; ++i) {
    const double temp = snapshot.forecastHours[i].temperatureC;
    minTemp = i == 0 ? temp : std::min(minTemp, temp);
    maxTemp = i == 0 ? temp : std::max(maxTemp, temp);
  }
  const double span = maxTemp - minTemp;
  bool dry = m_hourCount > 0;
  for (std::size_t i = 0; i < m_hourCount; ++i) {
    dry = dry && snapshot.forecastHours[i].precipitationProbabilityPercent <= 0;
  }
  if (m_dryLabel->visible() != dry) {
    m_dryLabel->setVisible(dry);
  }

  for (std::size_t i = 0; i < kHourCount; ++i) {
    HourColumn& column = m_hours[i];
    const bool used = i < m_hourCount;
    if (column.column->visible() != used) {
      column.column->setVisible(used);
    }
    if (!used) {
      continue;
    }
    const auto& hour = snapshot.forecastHours[i];
    const bool now = i == 0;
    column.hour->setText(hourLabel(hour.timeIso));
    column.hour->setColor(now ? accent() : term_frame::dimColor());
    column.glyph->setGlyph(WeatherService::glyphForCode(hour.weatherCode, hour.isDay));
    column.glyph->setColor(now ? accent() : term_frame::dimColor());
    column.temp->setText(temperatureText(hour.temperatureC));
    if (column.percent->visible() == dry) {
      column.percent->setVisible(!dry);
    }
    if (column.rainTrack->visible() == dry) {
      column.rainTrack->setVisible(!dry);
    }
    column.percent->setText(std::format("{}%", hour.precipitationProbabilityPercent));
    column.bar->setFill(now ? accent() : term_frame::dimColor());
    column.barFraction = span > 0.01 ? static_cast<float>(0.3 + 0.7 * (hour.temperatureC - minTemp) / span) : 0.6F;
    column.rainFraction = static_cast<float>(std::clamp(hour.precipitationProbabilityPercent, 0, 100)) / 100.0F;
  }
}

void WeatherTab::syncDays(const WeatherSnapshot& snapshot) {
  m_dayCount = std::min(kDayCount, snapshot.forecastDays.size());
  double weekMin = 0.0;
  double weekMax = 0.0;
  for (std::size_t i = 0; i < m_dayCount; ++i) {
    const auto& day = snapshot.forecastDays[i];
    weekMin = i == 0 ? day.temperatureMinC : std::min(weekMin, day.temperatureMinC);
    weekMax = i == 0 ? day.temperatureMaxC : std::max(weekMax, day.temperatureMaxC);
  }
  const double span = std::max(0.01, weekMax - weekMin);
  const std::string today = todayIso(snapshot.utcOffsetSeconds);

  m_todayRow = kDayCount;
  for (std::size_t i = 0; i < kDayCount; ++i) {
    DayRow& row = m_days[i];
    const bool used = i < m_dayCount;
    if (row.row->visible() != used) {
      row.row->setVisible(used);
    }
    if (!used) {
      continue;
    }
    const auto& day = snapshot.forecastDays[i];
    const bool isToday = day.dateIso == today;
    if (isToday) {
      m_todayRow = i;
    }
    const ColorSpec label = isToday ? accent() : text();
    const ColorSpec muted = isToday ? accent() : term_frame::dimColor();
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
    row.bar->setFill(isToday ? accent() : term_frame::dimColor());
    row.rangeStart = static_cast<float>((day.temperatureMinC - weekMin) / span);
    row.rangeEnd = static_cast<float>((day.temperatureMaxC - weekMin) / span);
  }
}

std::string WeatherTab::quickPanelHints() const {
  if (m_weather == nullptr || !m_weather->enabled() || !m_weather->locationConfigured() || !m_weather->snapshot().valid) {
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

  const auto age = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::system_clock::now() - snapshot.fetchedAt);
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
      const float barWidth = std::max(3.0F, std::floor(trackWidth * 0.4F));
      const float barHeight = std::clamp(std::round(trackHeight * hour.barFraction), 2.0F, trackHeight);
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
    const float barWidth = std::min(trackWidth, std::max(3.0F, std::round((day.rangeEnd - day.rangeStart) * trackWidth)));
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
  if (isoDate.size() != 10) {
    return isoDate;
  }

  std::tm tm{};
  tm.tm_year = std::stoi(isoDate.substr(0, 4)) - 1900;
  tm.tm_mon = std::stoi(isoDate.substr(5, 2)) - 1;
  tm.tm_mday = std::stoi(isoDate.substr(8, 2));
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
  m_shaderTime += deltaMs * 0.001F;
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
  return term_frame::lowerAscii(snapshot.locationName.empty() ? i18n::tr("location.locations.current") : snapshot.locationName);
}
