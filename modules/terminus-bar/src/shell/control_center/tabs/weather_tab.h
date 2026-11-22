#pragma once

#include "render/core/render_styles.h"
#include "shell/control_center/tab.h"
#include "shell/control_center/term_frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

class Box;
class EffectNode;
class Flex;
class Glyph;
class Label;
class Slider;
class WeatherService;
class ConfigService;
struct WeatherSnapshot;

// Compact terminal-style weather report: ¹now, ²hours (next 12) and ³days (7) boxes; the update info goes in the host
// footer hints.
class WeatherTab : public Tab {
public:
  WeatherTab(WeatherService* weather, ConfigService* config);

  std::unique_ptr<Flex> create() override;
  void onClose() override;
  void onFrameTick(float deltaMs) override;
  [[nodiscard]] std::string quickPanelStatus() const override;
  [[nodiscard]] std::string quickPanelHints() const override;
  [[nodiscard]] float quickPanelHeight(float width, float fallback) const override;

private:
  static constexpr std::size_t kHourCount = 12;
  static constexpr std::size_t kDayCount = 7;

  struct HourColumn {
    Flex* column = nullptr;
    Label* hour = nullptr;
    Glyph* glyph = nullptr;
    Flex* barTrack = nullptr;
    Box* bar = nullptr;
    Label* temp = nullptr;
    Flex* rainTrack = nullptr;
    Box* rain = nullptr;
    Label* percent = nullptr;
    float barFraction = 0.0F;
    float rainFraction = 0.0F;
  };

  struct DayRow {
    Flex* row = nullptr;
    Label* day = nullptr;
    Glyph* glyph = nullptr;
    Label* condition = nullptr;
    Label* low = nullptr;
    Flex* track = nullptr;
    Box* bar = nullptr;
    Label* high = nullptr;
    float rangeStart = 0.0F;
    float rangeEnd = 0.0F;
  };

  // What the report's row layout depends on; a change means the body must be measured again.
  struct ShapeKey {
    bool reportVisible = false;
    bool dryLabelVisible = false;
    bool stateBodyVisible = false;
    std::size_t hourCount = 0;
    std::size_t dayCount = 0;

    [[nodiscard]] bool operator==(const ShapeKey&) const = default;
  };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void addEffectLayer();
  [[nodiscard]] std::unique_ptr<Flex> buildStateBox(float scale);
  [[nodiscard]] std::unique_ptr<Flex> buildNowBox(float scale);
  [[nodiscard]] std::unique_ptr<Flex> buildHoursBox(float scale);
  [[nodiscard]] std::unique_ptr<Flex> buildDaysBox(float scale);
  void layoutNow(float inner, float scale);
  void layoutHours(float inner, float scale);
  void layoutDays(float inner, float scale);
  void sync();
  void showState(const std::string& title, const std::string& body);
  void syncNow(const WeatherSnapshot& snapshot);
  void syncHours(const WeatherSnapshot& snapshot);
  void syncDays(const WeatherSnapshot& snapshot);
  void relayoutReport(Renderer& renderer);
  void positionBars();
  void hideEffect();
  void updateEffectFrame();
  void syncEffect(const WeatherSnapshot& snapshot);
  [[nodiscard]] static EffectType effectForWeatherCode(std::int32_t code, bool isDay);
  [[nodiscard]] std::string temperatureText(double celsius) const;
  [[nodiscard]] std::string feelsText(const WeatherSnapshot& snapshot) const;
  [[nodiscard]] std::string windText(const WeatherSnapshot& snapshot) const;
  [[nodiscard]] static std::string hourLabel(const std::string& isoTime, bool twelveHour);
  [[nodiscard]] static std::string todayIso(std::int32_t utcOffsetSeconds);
  [[nodiscard]] static std::string weekdayLabel(const std::string& isoDate);

  WeatherService* m_weather = nullptr;
  ConfigService* m_config = nullptr;

  Flex* m_root = nullptr;
  Flex* m_data = nullptr;
  Flex* m_stateBox = nullptr;
  Label* m_stateTitle = nullptr;
  Label* m_stateBody = nullptr;

  Flex* m_nowBox = nullptr;
  EffectNode* m_effectNode = nullptr;
  Box* m_veil = nullptr;
  EffectType m_activeEffect = EffectType::None;
  float m_shaderTime = 0.0F;
  Label* m_dryLabel = nullptr;
  Flex* m_nowRow = nullptr;
  Label* m_temp = nullptr;
  Glyph* m_glyph = nullptr;
  Label* m_condition = nullptr;
  Label* m_feels = nullptr;
  term_frame::KeyValueRow m_wind;
  term_frame::KeyValueRow m_humidity;
  term_frame::DottedMeterRow m_uv;
  term_frame::KeyValueRow m_sun;

  Flex* m_hoursBox = nullptr;
  Flex* m_hoursStrip = nullptr;
  std::array<HourColumn, kHourCount> m_hours{};

  Flex* m_daysBox = nullptr;
  std::array<DayRow, kDayCount> m_days{};

  float m_contentHeight = 0.0F; // logical px; kept across reopen
  float m_laidOutWidth = 0.0F;
  ShapeKey m_shape;
  bool m_resizePending = false;
  std::size_t m_hourCount = 0;
  std::size_t m_dayCount = 0;
  std::size_t m_todayRow = kDayCount;
};
