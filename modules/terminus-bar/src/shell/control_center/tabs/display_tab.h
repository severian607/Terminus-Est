#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/tab.h"
#include "shell/control_center/term_frame.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class BrightnessService;
class Button;
class ConfigService;
class Flex;
class GammaService;
class Label;
class Renderer;
class WeatherService;

// Dedicated display panel body: ¹brightness (per output + all + DDC rescan) and ²night light
// (schedule, force-on, warmth). Hosted by a QuickPanel opened from the bar's brightness widget.
class DisplayTab : public Tab {
public:
  DisplayTab(
      BrightnessService* brightness, GammaService* nightLight, ConfigService* config,
      WeatherService* weather = nullptr
  );

  std::unique_ptr<Flex> create() override;
  void setActive(bool active) override;
  void onClose() override;
  [[nodiscard]] float quickPanelHeight(float width, float fallback) const override;
  [[nodiscard]] std::string quickPanelHints() const override { return "r rescan  n schedule  o force"; }
  [[nodiscard]] bool handleQuickPanelKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

private:
  struct BrightnessRow {
    std::string displayId; // empty = the `all` row
    term_frame::MeterRow meter;
    float lastValue = -1.0F;
    bool lastControllable = true;
  };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;

  void rebuildBrightnessRows(float scale);
  void syncBrightness();
  void syncNightLight();
  [[nodiscard]] bool brightnessDragging() const;
  [[nodiscard]] float minimumBrightness() const;

  void rescanDisplays();
  void toggleSchedule();
  void toggleForce();
  // Sunset/sunrise as "%H:%M", same sources as the Home night-light toggle; empty strings when unknown.
  void sunTimeStrings(std::string& sunset, std::string& sunrise) const;

  void queueBrightness(const std::string& displayId, float value);
  void flushPendingBrightness();

  void queueWarmth(std::int32_t kelvin);
  void flushPendingWarmth();
  [[nodiscard]] std::int32_t warmthCeiling() const;

  BrightnessService* m_brightness = nullptr;
  GammaService* m_nightLight = nullptr;
  ConfigService* m_config = nullptr;
  WeatherService* m_weather = nullptr;

  Flex* m_rootLayout = nullptr;
  Flex* m_brightnessBox = nullptr;
  Label* m_brightnessChip = nullptr;
  Flex* m_brightnessPanel = nullptr;
  Label* m_brightnessUnavailable = nullptr;
  term_frame::Keyword m_rescanKey;

  Flex* m_nightBox = nullptr;
  Label* m_nightChip = nullptr;
  Flex* m_nightControls = nullptr;
  Label* m_nightUnavailable = nullptr;
  term_frame::ToggleRow m_scheduleRow;
  term_frame::ToggleRow m_forceRow;
  term_frame::MeterRow m_warmthMeter;
  Label* m_nightStatus = nullptr;

  bool m_rowsFresh = false; // rows were just (re)built and have not been synced yet
  std::vector<std::unique_ptr<BrightnessRow>> m_rows;
  std::string m_rowsKey;
  bool m_rowsBuilt = false;
  int m_reportedRows = -1; // display-row count the panel height was last sized for
  bool m_resizePending = false;

  // Brightness write debounce (same policy as MonitorTab).
  std::string m_pendingDisplayId;
  bool m_pendingValid = false;
  float m_pendingBrightness = -1.0F;
  float m_lastSentBrightness = -1.0F;
  std::string m_lastSentId;
  std::chrono::steady_clock::time_point m_lastCommitAt;
  std::chrono::steady_clock::time_point m_ignoreStateUntil;
  Timer m_debounceTimer;

  // Warmth write debounce: persisting rewrites settings.toml and reloads, so coalesce a drag.
  std::int32_t m_pendingKelvin = -1;
  Timer m_warmthTimer;

  bool m_syncing = false;
  bool m_active = false;
};
