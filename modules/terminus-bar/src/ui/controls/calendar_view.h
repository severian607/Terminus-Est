#pragma once

#include <chrono>
#include <cstdint>
#include <utility>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class Flex;
class InputArea;
class Label;
class ScrollView;
struct CalendarEvent;
struct CalendarSnapshot;
enum class CalendarFirstDay : std::uint8_t;

namespace calendar_view {

  struct Date {
    int year = 0;
    int month = -1;
    int day = -1;

    [[nodiscard]] bool valid() const noexcept { return year != 0 && month >= 0 && day > 0; }
    bool operator==(const Date&) const = default;
  };

  struct State {
    Date current;
    int displayYear = 0;
    int displayMonth = 0;
    int displayWeekday = 0;
    bool isCurrentMonth = false;
  };

  struct MonthLayout {
    float width = 0.0F;
    float weekdayHeight = 0.0F;
    float dayCellHeight = 0.0F;
    float dayButtonSize = 0.0F;
    float gap = 0.0F;
    float dotDiameter = 0.0F;
    float dotGap = 0.0F;
    float weekColumnWidth = 0.0F;
    float weekLaneInset = 0.0F;
    float weekDividerWidth = 0.0F;
    float weekDaysGap = 0.0F;
  };

  struct MonthBuildOptions {
    Flex& grid;
    Label& monthLabel;
    const CalendarSnapshot* snapshot = nullptr;
    Date selected;
    int monthOffset = 0;
    bool showWeekNumbers = false;
    // tm_wday encoding (Sun=0 .. Sat=6); negative follows the locale.
    int firstDayOfWeek = -1;
    float scale = 1.0F;
    MonthLayout layout;
    std::string fontFamily;
    std::function<void(Date date, int monthShift)> onDateSelected;
    std::function<void(Date date)> onDateRightClicked;
  };
  struct EventLinkOverlay {
    Flex* row = nullptr;
    InputArea* area = nullptr;
  };

  struct EventListState {
    std::vector<EventLinkOverlay> linkOverlays;
  };

  struct EventListBuildOptions {
    ScrollView& scroll;
    bool reserveScrollbarGutter = false;
    Label* title = nullptr;
    const CalendarSnapshot* snapshot = nullptr;
    Date selected;
    float scale = 1.0F;
    std::string_view dateFormat;
    std::string_view timeFormat;
    std::string fontFamily;
    EventListState* state = nullptr;
    std::function<void()> requestRedraw;
  };

  [[nodiscard]] State stateForOffset(int monthOffset);
  [[nodiscard]] int dateKey(Date date) noexcept;

  [[nodiscard]] bool eventPassed(const CalendarEvent& event, std::chrono::system_clock::time_point now);
  // Local start/end day keys (dateKey encoding) an event spans; all-day end is inclusive.
  [[nodiscard]] std::pair<int, int> eventDayKeys(const CalendarEvent& event);
  // tm_wday encoding (Sun=0 .. Sat=6) of the configured week start; Locale defers to localeFirstDayOfWeek().
  [[nodiscard]] int resolveFirstDayOfWeek(CalendarFirstDay setting);

  void rebuildMonth(const MonthBuildOptions& options);
  void rebuildEventList(const EventListBuildOptions& options);
  void layoutEventLinkOverlays(const EventListState& state);

} // namespace calendar_view
