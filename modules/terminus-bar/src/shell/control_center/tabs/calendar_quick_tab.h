#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/tab.h"

#include <chrono>
#include <cstdint>
#include <string>

class CalendarService;
class TaskService;
class ConfigService;
class Flex;
class Renderer;
struct CalendarFrame;

// Compact btop-style calendar for the bar clock QuickPanel: hero clock, month grid and an agenda.
class CalendarQuickTab : public Tab {
public:
  CalendarQuickTab(ConfigService* config, CalendarService* calendar, TaskService* tasks = nullptr);
  ~CalendarQuickTab() override;

  std::unique_ptr<Flex> create() override;
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
  [[nodiscard]] CalendarFrame makeFrame() const;
  void rebuild();
  [[nodiscard]] std::unique_ptr<Flex> buildMonthBox(const CalendarFrame& frame);
  [[nodiscard]] std::unique_ptr<Flex> buildMonthNav(const CalendarFrame& frame);
  [[nodiscard]] std::unique_ptr<Flex> buildWeekRow(const CalendarFrame& frame, std::chrono::sys_days weekStart);
  [[nodiscard]] std::unique_ptr<Flex> buildAgendaBox(const CalendarFrame& frame) const;
  void select(std::chrono::year_month_day date);
  void shiftDays(int days);
  void shiftMonths(int months);
  void markDirty();

  ConfigService* m_config = nullptr;
  TaskService* m_tasks = nullptr;
  CalendarService* m_calendar = nullptr;
  std::uint64_t m_changeCallbackId = 0;
  bool m_changeCallbackRegistered = false;
  Flex* m_root = nullptr;
  Timer m_clockTimer;
  std::chrono::year_month_day m_selected;
  bool m_selectedInitialised = false;
  bool m_dirty = true;
  bool m_active = false;
  float m_bodyWidth = 0.0F;
  float m_bodyHeight = 0.0F;
  std::int64_t m_lastMinute = 0; // minutes since the epoch at the last rebuild
};
