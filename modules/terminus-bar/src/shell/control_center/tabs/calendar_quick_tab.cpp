// Bar-clock quick panel: hero clock, month grid (¹) and agenda (²) with a fixed row budget for events and tasks.

#include "shell/control_center/tabs/calendar_quick_tab.h"

#include "calendar/calendar_service.h"
#include "calendar/calendar_types.h"
#include "config/config_service.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/control_center/term_frame.h"
#include "shell/panel/panel_manager.h"
#include "tasks/task_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/calendar_view.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  namespace chr = std::chrono;

  // Logical px (unscaled). The panel body is laid out from these so quickPanelHeight() never needs nodes.
  constexpr float kCellWidth = 24.0F;
  constexpr float kWeekColumnWidth = 20.0F;
  constexpr float kRowHeight = term_frame::kRowHeight;
  constexpr float kTitleRowHeight = 17.0F;
  constexpr float kHeroHeight = 46.0F;
  constexpr float kHeroClockSize = 32.0F;
  constexpr float kFallbackBodyWidth = 442.0F;
  constexpr float kEventDotWidth = 4.0F;
  constexpr float kEventDotHeight = 2.0F;
  constexpr float kEventDotBottomMargin = 3.0F;
  constexpr float kPassedAlpha = 0.55F;
  constexpr float kMonoAdvanceEm = 0.6F; // JetBrains Mono glyph advance, for the wrap estimate

  constexpr int kDaysPerWeek = 7;
  constexpr int kGridWeeks = 6;
  constexpr int kGridRows = 2 + kGridWeeks; // navigation + weekday initials + weeks
  constexpr int kThursday = 4;              // tm_wday; ISO week numbers are named by the week's Thursday

  // Agenda row budget: events first, then up to kMaxTasks open tasks (a long title wraps to two lines).
  constexpr int kAgendaRows = 12;
  constexpr int kMaxSelectedRows = 3; // the last slot turns into "+N more" on overflow
  constexpr int kMaxTasks = 3;
  constexpr int kNextDays = 7;
  constexpr std::size_t kTimeColumnChars = 11;

  constexpr float kBoxHeight =
      2.0F * (Style::spaceSm + Style::borderWidth) + kTitleRowHeight + Style::spaceXs + kAgendaRows * kRowHeight;
  constexpr float kPanelHeight = kHeroHeight + Style::spaceSm + kBoxHeight;

  constexpr std::array<const char*, 12> kMonthAbbreviations = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

  using term_frame::dimColor;

  std::tm localTm(chr::system_clock::time_point time) {
    const std::time_t raw = chr::system_clock::to_time_t(time);
    std::tm local{};
    localtime_r(&raw, &local);
    return local;
  }

  chr::year_month_day today() {
    const std::tm local = localTm(chr::system_clock::now());
    return chr::year{local.tm_year + 1900}
    / chr::month{static_cast<unsigned>(local.tm_mon + 1)}
    / chr::day{static_cast<unsigned>(local.tm_mday)};
  }

  // yyyymmdd, comparable as an integer.
  int keyOf(chr::year_month_day date) {
    return static_cast<int>(date.year()) * 10000
        + static_cast<int>(static_cast<unsigned>(date.month())) * 100
        + static_cast<int>(static_cast<unsigned>(date.day()));
  }

  std::tm tmOf(chr::year_month_day date) {
    std::tm value{};
    value.tm_year = static_cast<int>(date.year()) - 1900;
    value.tm_mon = static_cast<int>(static_cast<unsigned>(date.month())) - 1;
    value.tm_mday = static_cast<int>(static_cast<unsigned>(date.day()));
    value.tm_wday = static_cast<int>(chr::weekday{chr::sys_days{date}}.c_encoding());
    return value;
  }

  // Minutes since the epoch: changes exactly when the local clock face does, without formatting a string.
  std::int64_t currentMinute() { return chr::floor<chr::minutes>(chr::system_clock::now()).time_since_epoch().count(); }

  // First `count` UTF-8 codepoints of `text`.
  std::string firstCodepoints(const std::string& text, int count) {
    std::size_t end = 0;
    int seen = 0;
    while (end < text.size() && seen < count) {
      ++end;
      while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
        ++end;
      }
      ++seen;
    }
    return text.substr(0, end);
  }

  std::string monthTitle(chr::year_month_day date) {
    std::tm value = tmOf(date);
    value.tm_mday = 1;
#ifdef __GLIBC__
    return formatStrftime("%OB %Y", value);
#else
    return formatStrftime("%B %Y", value);
#endif
  }

  chr::year_month_day addMonths(chr::year_month_day date, int months) {
    const chr::year_month target = chr::year_month{date.year(), date.month()} + chr::months{months};
    const chr::day last = (target / chr::last).day();
    return target / (date.day() > last ? last : date.day());
  }

  std::string clockTime(const CalendarEvent& event) { return formatStrftime("%H:%M", localTm(event.start)); }

} // namespace

// Everything one rebuild() derives once and the section builders share.
struct CalendarFrame {
  float scale = 1.0F;
  float font = 0.0F;
  float rowHeight = 0.0F;
  float boxHeight = 0.0F;
  float gap = 0.0F;
  float bodyWidth = 0.0F;
  float cellWidth = 0.0F;
  float weekWidth = 0.0F; // 0 when week numbers are hidden
  float gridWidth = 0.0F; // inner width of the month grid
  float monthBoxWidth = 0.0F;
  float agendaBoxWidth = 0.0F;
  bool showWeeks = false;
  int firstDay = 0; // tm_wday of the first column
  std::string family;
  ColorSpec text;
  ColorSpec primary;
  chr::system_clock::time_point now;
  chr::year_month_day today;
  int todayKey = 0;
  int selectedKey = 0;
  const CalendarSnapshot* snapshot = nullptr;
  std::vector<std::pair<int, int>> eventDays; // eventDayKeys of snapshot->events, index-aligned

  [[nodiscard]] bool dayHasEvents(int key) const {
    return std::any_of(eventDays.begin(), eventDays.end(), [key](const auto& range) {
      return key >= range.first && key <= range.second;
    });
  }
};

namespace {

  std::unique_ptr<Label> frameLabel(
      const CalendarFrame& frame, std::string text, const ColorSpec& color,
      std::optional<FontWeight> weight = std::nullopt
  ) {
    return ui::label({
        .text = std::move(text),
        .fontSize = frame.font,
        .fontWeight = weight,
        .fontFamily = frame.family,
        .color = color,
        .maxLines = 1,
    });
  }

  // A fixed-width, centred grid cell holding one label.
  std::unique_ptr<Flex> centeredCell(const CalendarFrame& frame, float width, std::unique_ptr<Label> label) {
    auto cell = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center});
    cell->addChild(std::move(label));
    term_frame::pinSize(*cell, width, frame.rowHeight);
    return cell;
  }

  std::unique_ptr<Flex> buildHero(const CalendarFrame& frame) {
    const std::tm nowTm = localTm(frame.now);
    const chr::days left = chr::sys_days{frame.today.year() / chr::December / 31} - chr::sys_days{frame.today};
    const float clockSize = kHeroClockSize * frame.scale;
    const float gap = Style::spaceMd * frame.scale;

    auto hero = ui::row({.align = FlexAlign::Center, .gap = gap});
    hero->addChild(
        ui::label({
            .text = formatStrftime("%H:%M", nowTm),
            .fontSize = clockSize,
            .fontWeight = FontWeight::Medium,
            .fontFamily = frame.family,
            .color = frame.primary,
            .maxLines = 1,
        })
    );
    auto info = ui::column({.align = FlexAlign::Start, .justify = FlexJustify::Center, .gap = 0.0F, .flexGrow = 1.0F});
    info->addChild(
        ui::label({
            .text = formatStrftime("%A %-d %B %Y", nowTm),
            .fontSize = frame.font,
            .fontFamily = frame.family,
            .color = dimColor(),
            .maxWidth = std::max(0.0F, frame.bodyWidth - term_frame::monoWidth(6, clockSize) - gap),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
        })
    );
    info->addChild(frameLabel(
        frame, std::format("{} days left in {}", left.count(), static_cast<int>(frame.today.year())), dimColor()
    ));
    hero->addChild(std::move(info));
    term_frame::pinSize(*hero, frame.bodyWidth, kHeroHeight * frame.scale);
    return hero;
  }

  std::unique_ptr<Flex> buildWeekdayRow(const CalendarFrame& frame) {
    auto row = ui::row({.align = FlexAlign::Center});
    if (frame.showWeeks) {
      row->addChild(centeredCell(frame, frame.weekWidth, frameLabel(frame, "wk", dimColor())));
    }
    for (int i = 0; i < kDaysPerWeek; ++i) {
      std::tm value{};
      value.tm_wday = (frame.firstDay + i) % kDaysPerWeek;
      value.tm_mday = 1;
      row->addChild(centeredCell(
          frame, frame.cellWidth, frameLabel(frame, firstCodepoints(formatStrftime("%a", value), 2), frame.text)
      ));
    }
    term_frame::pinSize(*row, frame.gridWidth, frame.rowHeight);
    return row;
  }

  // One day of the month grid: today is a filled cell, the selection a bordered one, event days carry a dot.
  std::unique_ptr<Flex>
  buildDayCell(const CalendarFrame& frame, chr::year_month_day date, chr::year_month_day selected) {
    const int key = keyOf(date);
    const bool inMonth = date.month() == selected.month() && date.year() == selected.year();
    const bool isToday = key == frame.todayKey;
    const bool isSelected = key == frame.selectedKey;

    ColorSpec labelColor = inMonth ? frame.text : colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.55F);
    if (isToday) {
      labelColor = colorSpecFromRole(ColorRole::Surface);
    } else if (isSelected) {
      labelColor = frame.primary;
    }
    auto cell = ui::column({
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .fill = isToday ? std::optional<ColorSpec>(frame.primary) : std::nullopt,
        .radius = 0.0F,
        .border = isSelected
            ? std::optional<ColorSpec>(isToday ? colorSpecFromRole(ColorRole::OnSurface) : frame.primary)
            : std::nullopt,
        .borderWidth = Style::borderWidth,
    });
    cell->addChild(frameLabel(
        frame, std::to_string(static_cast<unsigned>(date.day())), labelColor,
        isToday || isSelected ? FontWeight::Bold : FontWeight::Normal
    ));
    if (frame.dayHasEvents(key)) {
      const float dotWidth = kEventDotWidth * frame.scale;
      const float dotHeight = kEventDotHeight * frame.scale;
      auto dot = ui::box({
          .fill = isToday ? colorSpecFromRole(ColorRole::Surface) : frame.primary,
          .radius = 0.0F,
          .width = dotWidth,
          .height = dotHeight,
          .participatesInLayout = false,
      });
      dot->setPosition(
          std::round((frame.cellWidth - dotWidth) * 0.5F),
          frame.rowHeight - dotHeight - kEventDotBottomMargin * frame.scale
      );
      cell->addChild(std::move(dot));
    }
    term_frame::pinSize(*cell, frame.cellWidth, frame.rowHeight);
    return cell;
  }

  // ---- agenda ---------------------------------------------------------------------------------------

  // The agenda's column of rows plus its budget. Every row goes through addLine/addEntry, which keep
  // rowsUsed() in step with the pixel height pinned on the row, so callers only ask fits(n) before adding.
  class AgendaList {
  public:
    AgendaList(const CalendarFrame& frame, float listWidth)
        : m_frame(frame), m_list(ui::column({.align = FlexAlign::Start, .gap = 0.0F})), m_listWidth(listWidth),
          m_timeWidth(term_frame::monoWidth(kTimeColumnChars, frame.font)),
          m_titleWidth(std::max(0.0F, listWidth - m_timeWidth - Style::spaceXs * frame.scale)) {}

    [[nodiscard]] bool fits(int rows) const { return m_rowsUsed + rows <= kAgendaRows; }

    void addLine(std::string text, const ColorSpec& color) {
      auto line = ui::label({
          .text = std::move(text),
          .fontSize = m_frame.font,
          .fontFamily = m_frame.family,
          .color = color,
          .maxWidth = m_listWidth,
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      });
      auto row = ui::row({.align = FlexAlign::Center});
      row->addChild(std::move(line));
      term_frame::pinSize(*row, m_listWidth, m_frame.rowHeight);
      m_list->addChild(std::move(row));
      ++m_rowsUsed;
    }

    // Rows addEntry(…, wrapTitle) will take. The estimate counts bytes against the title column's monospace
    // capacity, so the pinned height and the label's maxLines always agree.
    [[nodiscard]] int entryRows(const std::string& title, bool wrapTitle) const {
      const auto charsPerLine =
          std::max<std::size_t>(1, static_cast<std::size_t>(m_titleWidth / (m_frame.font * kMonoAdvanceEm)));
      return wrapTitle && title.size() > charsPerLine ? 2 : 1;
    }

    // `when` in the dim time column, then the title; a wrapped title takes two rows.
    void addEntry(const std::string& when, const std::string& title, float alpha, bool wrapTitle = false) {
      const int rows = entryRows(title, wrapTitle);
      auto row =
          ui::row({.align = rows == 2 ? FlexAlign::Start : FlexAlign::Center, .gap = Style::spaceXs * m_frame.scale});
      row->addChild(
          ui::label({
              .text = when,
              .fontSize = m_frame.font,
              .fontFamily = m_frame.family,
              .color = dimColor(),
              .width = m_timeWidth,
              .maxLines = 1,
          })
      );
      row->addChild(
          ui::label({
              .text = title.empty() ? "(untitled)" : title,
              .fontSize = m_frame.font,
              .fontFamily = m_frame.family,
              .color = colorSpecFromRole(ColorRole::OnSurface, alpha),
              .maxWidth = m_titleWidth,
              .maxLines = rows,
              .ellipsize = TextEllipsize::End,
          })
      );
      term_frame::pinSize(*row, m_listWidth, static_cast<float>(rows) * m_frame.rowHeight);
      m_list->addChild(std::move(row));
      m_rowsUsed += rows;
    }

    // The list pinned to the full budget height, so the box never resizes with its content.
    [[nodiscard]] std::unique_ptr<Flex> finish() {
      term_frame::pinSize(*m_list, m_listWidth, m_frame.rowHeight * static_cast<float>(kAgendaRows));
      return std::move(m_list);
    }

  private:
    const CalendarFrame& m_frame;
    std::unique_ptr<Flex> m_list;
    float m_listWidth;
    float m_timeWidth;
    float m_titleWidth;
    int m_rowsUsed = 0;
  };

  struct AgendaEvents {
    std::vector<const CalendarEvent*> onSelectedDay; // all-day first, then by start
    std::vector<const CalendarEvent*> upcoming;      // other events starting today..+kNextDays, not yet passed
  };

  [[nodiscard]] AgendaEvents collectAgendaEvents(const CalendarFrame& frame) {
    AgendaEvents events;
    if (frame.snapshot == nullptr) {
      return events;
    }
    const int endKey = keyOf(chr::year_month_day{chr::sys_days{frame.today} + chr::days{kNextDays}});
    for (std::size_t i = 0; i < frame.snapshot->events.size(); ++i) {
      const CalendarEvent& event = frame.snapshot->events[i];
      const auto [startKey, endDayKey] = frame.eventDays[i];
      if (frame.selectedKey >= startKey && frame.selectedKey <= endDayKey) {
        events.onSelectedDay.push_back(&event);
      } else if (startKey >= frame.todayKey && startKey <= endKey && !calendar_view::eventPassed(event, frame.now)) {
        events.upcoming.push_back(&event);
      }
    }
    std::sort(
        events.onSelectedDay.begin(), events.onSelectedDay.end(), [](const CalendarEvent* a, const CalendarEvent* b) {
          return a->allDay != b->allDay ? a->allDay : a->start < b->start;
        }
    );
    std::sort(events.upcoming.begin(), events.upcoming.end(), [](const CalendarEvent* a, const CalendarEvent* b) {
      return a->start < b->start;
    });
    return events;
  }

  // Empty when the last sync was fine.
  [[nodiscard]] std::string_view syncErrorText(const CalendarService* calendar) {
    if (calendar == nullptr) {
      return {};
    }
    switch (calendar->syncError()) {
    case CalendarService::SyncError::AuthFailed:
      return "calendar: auth failed";
    case CalendarService::SyncError::Unreachable:
      return "calendar: server unreachable";
    case CalendarService::SyncError::NoCalendars:
      return "connected · no event calendar";
    case CalendarService::SyncError::Failed:
      return "calendar: sync failed";
    case CalendarService::SyncError::None:
      break;
    }
    return {};
  }

  // The selected day's events (or why there are none), at most kMaxSelectedRows rows.
  void addSelectedDay(
      AgendaList& list, const CalendarFrame& frame, const std::vector<const CalendarEvent*>& events,
      std::string_view syncError, bool configured
  ) {
    if (events.empty()) {
      if (!syncError.empty()) {
        list.addLine(std::string(syncError), colorSpecFromRole(ColorRole::Error));
      } else {
        list.addLine(configured ? "connected · no events" : "no calendar connected", dimColor());
      }
      return;
    }
    const bool isToday = frame.selectedKey == frame.todayKey;
    const int total = static_cast<int>(events.size());
    const int shown = total > kMaxSelectedRows ? kMaxSelectedRows - 1 : total;
    for (int i = 0; i < shown; ++i) {
      const CalendarEvent& event = *events[static_cast<std::size_t>(i)];
      const float alpha = isToday && calendar_view::eventPassed(event, frame.now) ? kPassedAlpha : 1.0F;
      list.addEntry(event.allDay ? "all day" : clockTime(event), event.title, alpha);
    }
    if (shown < total) {
      list.addLine(std::format("+{} more", total - shown), dimColor());
    }
  }

  // "next 7 days" with as many upcoming events as the budget allows, or why there are none.
  void addUpcoming(
      AgendaList& list, const CalendarFrame& frame, const std::vector<const CalendarEvent*>& upcoming,
      std::string_view syncError, bool selectedDayHasEvents
  ) {
    constexpr int kHeaderAndOneEntry = 2;
    if (!list.fits(kHeaderAndOneEntry)) {
      return;
    }
    list.addLine("next 7 days", dimColor());
    int shown = 0;
    for (const CalendarEvent* event : upcoming) {
      if (!list.fits(1)) {
        break;
      }
      std::string when = formatStrftime("%a", localTm(event->start));
      when += event->allDay ? " all-day" : " " + clockTime(*event);
      list.addEntry(when, event->title, 1.0F);
      ++shown;
    }
    if (!list.fits(1)) {
      return;
    }
    if (!syncError.empty() && selectedDayHasEvents) {
      list.addLine(std::string(syncError), colorSpecFromRole(ColorRole::Error));
    } else if (shown == 0) {
      list.addLine(frame.snapshot != nullptr && frame.snapshot->valid ? "none" : "syncing…", dimColor());
    }
  }

  // "12 Nov" for a yyyy-mm-dd due date, "late 12 Nov" once it is before `todayIso`; empty when undated or malformed.
  [[nodiscard]] std::string taskDueLabel(const std::string& dueDate, const std::string& todayIso) {
    constexpr std::size_t kIsoDateLength = 10;
    if (dueDate.size() != kIsoDateLength) {
      return {};
    }
    const int month = std::atoi(dueDate.substr(5, 2).c_str());
    const int day = std::atoi(dueDate.substr(8, 2).c_str());
    std::string label = std::format("{} {}", day, month >= 1 && month <= 12 ? kMonthAbbreviations[month - 1] : "?");
    return dueDate < todayIso ? "late " + label : label;
  }

  // Open tasks under the events: dated ones first (soonest first), then undated. The "tasks" header only appears
  // together with the first task under it; later tasks stop at kMaxTasks or when the next one no longer fits.
  void addTasks(AgendaList& list, const TaskService& tasks, chr::year_month_day todayDate) {
    std::vector<const TaskItem*> open;
    for (const TaskItem& task : tasks.snapshot().items) {
      if (!task.completed) {
        open.push_back(&task);
      }
    }
    if (open.empty()) {
      return;
    }
    std::stable_sort(open.begin(), open.end(), [](const TaskItem* a, const TaskItem* b) {
      if (a->dueDate.empty() != b->dueDate.empty()) {
        return !a->dueDate.empty();
      }
      return a->dueDate < b->dueDate;
    });
    constexpr int kHeaderRows = 1;
    if (!list.fits(kHeaderRows + list.entryRows(open.front()->title, true))) {
      return;
    }

    const std::string todayIso = std::format(
        "{:04d}-{:02d}-{:02d}", static_cast<int>(todayDate.year()), static_cast<unsigned>(todayDate.month()),
        static_cast<unsigned>(todayDate.day())
    );
    list.addLine(std::format("tasks · {} open", open.size()), dimColor());
    int shownTasks = 0;
    for (const TaskItem* task : open) {
      if (shownTasks >= kMaxTasks || !list.fits(list.entryRows(task->title, true))) {
        break;
      }
      list.addEntry(taskDueLabel(task->dueDate, todayIso), task->title, 1.0F, true);
      ++shownTasks;
    }
  }

} // namespace

CalendarQuickTab::CalendarQuickTab(ConfigService* config, CalendarService* calendar, TaskService* tasks)
    : m_config(config), m_tasks(tasks), m_calendar(calendar), m_selected(today()) {}

CalendarQuickTab::~CalendarQuickTab() {
  if (m_calendar != nullptr && m_changeCallbackRegistered) {
    m_calendar->removeChangeCallback(m_changeCallbackId);
  }
}

std::unique_ptr<Flex> CalendarQuickTab::create() {
  if (m_calendar != nullptr && !m_changeCallbackRegistered) {
    m_changeCallbackRegistered = true;
    m_changeCallbackId = m_calendar->addChangeCallback([this]() { markDirty(); });
  }
  if (!m_selectedInitialised) {
    m_selected = today();
    m_selectedInitialised = true;
  }

  auto root = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * contentScale(),
  });
  m_dirty = true;
  rebuild();
  return root;
}

void CalendarQuickTab::setActive(bool active) {
  m_active = active;
  if (!active) {
    m_clockTimer.stop();
    return;
  }
  // Jump back to today whenever the panel opens so it never shows a stale selection.
  m_selected = today();
  m_dirty = true;
  m_clockTimer.startRepeating(std::chrono::seconds{1}, [this]() {
    if (currentMinute() != m_lastMinute) {
      PanelManager::instance().refresh();
    }
  });
}

void CalendarQuickTab::onClose() {
  m_clockTimer.stop();
  m_root = nullptr;
  m_bodyWidth = 0.0F;
  m_bodyHeight = 0.0F;
}

float CalendarQuickTab::quickPanelHeight(float /*width*/, float /*fallback*/) const { return kPanelHeight; }

std::string CalendarQuickTab::quickPanelHints() const { return "←→ day  PgUp/Dn month  t today"; }

std::string CalendarQuickTab::quickPanelStatus() const {
  const chr::year_month_day now = today();
  const chr::days yday = chr::sys_days{now} - chr::sys_days{now.year() / chr::January / 1} + chr::days{1};
  return std::format("week {:%V} · day {}", chr::sys_days{now}, yday.count());
}

bool CalendarQuickTab::handleQuickPanelKey(std::uint32_t sym, std::uint32_t /*modifiers*/, bool pressed, bool preedit) {
  if (preedit) {
    return false;
  }
  int days = 0;
  int months = 0;
  bool toToday = false;
  switch (sym) {
  case XKB_KEY_Left:
    days = -1;
    break;
  case XKB_KEY_Right:
    days = 1;
    break;
  case XKB_KEY_Up:
    days = -kDaysPerWeek;
    break;
  case XKB_KEY_Down:
    days = kDaysPerWeek;
    break;
  case XKB_KEY_Page_Up:
    months = -1;
    break;
  case XKB_KEY_Page_Down:
    months = 1;
    break;
  case XKB_KEY_t:
  case XKB_KEY_T:
    toToday = true;
    break;
  default:
    return false;
  }
  if (!pressed) {
    return true;
  }
  if (toToday) {
    select(today());
  } else if (days != 0) {
    shiftDays(days);
  } else {
    shiftMonths(months);
  }
  return true;
}

void CalendarQuickTab::select(chr::year_month_day date) {
  if (!date.ok()) {
    return;
  }
  m_selected = date;
  markDirty();
}

void CalendarQuickTab::shiftDays(int days) { select(chr::year_month_day{chr::sys_days{m_selected} + chr::days{days}}); }

void CalendarQuickTab::shiftMonths(int months) { select(addMonths(m_selected, months)); }

void CalendarQuickTab::markDirty() {
  m_dirty = true;
  PanelManager::instance().refresh();
}

void CalendarQuickTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr) {
    return;
  }
  if (std::abs(contentWidth - m_bodyWidth) > 0.5F) {
    m_bodyWidth = contentWidth;
    m_dirty = true;
  }
  m_bodyHeight = bodyHeight;
  if (m_dirty) {
    rebuild();
  }
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
}

void CalendarQuickTab::doUpdate(Renderer& renderer) {
  if (m_root == nullptr) {
    return;
  }
  if (currentMinute() != m_lastMinute) {
    m_dirty = true;
  }
  if (!m_dirty) {
    return;
  }
  rebuild();
  m_root->setSize(m_bodyWidth, m_bodyHeight);
  m_root->layout(renderer);
}

CalendarFrame CalendarQuickTab::makeFrame() const {
  CalendarFrame frame;
  frame.scale = contentScale();
  frame.font = Style::fontSizeCaption * frame.scale;
  frame.rowHeight = kRowHeight * frame.scale;
  frame.boxHeight = kBoxHeight * frame.scale;
  frame.gap = Style::spaceSm * frame.scale;
  frame.bodyWidth = m_bodyWidth > 0.0F ? m_bodyWidth : kFallbackBodyWidth * frame.scale;
  frame.family = term_frame::kFont;
  frame.text = colorSpecFromRole(ColorRole::OnSurface);
  frame.primary = colorSpecFromRole(ColorRole::Primary);

  const auto* calendarConfig = m_config != nullptr ? &m_config->config().controlCenter.calendarTab : nullptr;
  frame.showWeeks = calendarConfig != nullptr && calendarConfig->showWeekNumbers;
  frame.firstDay = calendarConfig != nullptr ? calendar_view::resolveFirstDayOfWeek(calendarConfig->firstDayOfWeek)
                                             : localeFirstDayOfWeek();
  frame.cellWidth = kCellWidth * frame.scale;
  frame.weekWidth = frame.showWeeks ? kWeekColumnWidth * frame.scale : 0.0F;
  frame.gridWidth = static_cast<float>(kDaysPerWeek) * frame.cellWidth + frame.weekWidth;
  frame.monthBoxWidth = frame.gridWidth + 2.0F * term_frame::boxInset(frame.scale);
  frame.agendaBoxWidth = std::max(0.0F, frame.bodyWidth - frame.monthBoxWidth - frame.gap);

  frame.now = chr::system_clock::now();
  frame.today = today();
  frame.todayKey = keyOf(frame.today);
  frame.selectedKey = keyOf(m_selected);
  frame.snapshot = m_calendar != nullptr ? &m_calendar->snapshot() : nullptr;
  if (frame.snapshot != nullptr) {
    frame.eventDays.reserve(frame.snapshot->events.size());
    for (const CalendarEvent& event : frame.snapshot->events) {
      frame.eventDays.push_back(calendar_view::eventDayKeys(event));
    }
  }
  return frame;
}

void CalendarQuickTab::rebuild() {
  if (m_root == nullptr) {
    return;
  }
  m_dirty = false;
  m_lastMinute = currentMinute();
  while (!m_root->children().empty()) {
    m_root->removeChild(m_root->children().front().get());
  }

  const CalendarFrame frame = makeFrame();
  m_root->addChild(buildHero(frame));
  auto columns = ui::row({.align = FlexAlign::Start, .gap = frame.gap});
  columns->addChild(buildMonthBox(frame));
  columns->addChild(buildAgendaBox(frame));
  term_frame::pinSize(*columns, frame.bodyWidth, frame.boxHeight);
  m_root->addChild(std::move(columns));
}

// ---- ¹ month grid -------------------------------------------------------------------------------------

std::unique_ptr<Flex> CalendarQuickTab::buildMonthNav(const CalendarFrame& frame) {
  auto nav = ui::row({.align = FlexAlign::Center});
  const auto arrow = [&](const char* glyph, int deltaMonths) {
    auto area = ui::inputArea(
        {.onClick = [this, deltaMonths](const InputArea::PointerData&) { shiftMonths(deltaMonths); }},
        frameLabel(frame, glyph, frame.primary)
    );
    area->setSize(frame.cellWidth, frame.rowHeight);
    return area;
  };
  const float titleWidth = frame.gridWidth - 2.0F * frame.cellWidth;
  nav->addChild(arrow("‹", -1));
  auto titleArea = ui::inputArea(
      {.onClick = [this](const InputArea::PointerData&) { select(today()); }},
      ui::label({
          .text = monthTitle(m_selected),
          .fontSize = frame.font,
          .fontFamily = frame.family,
          .color = frame.text,
          .width = titleWidth,
          .maxLines = 1,
          .textAlign = TextAlign::Center,
      })
  );
  titleArea->setSize(titleWidth, frame.rowHeight);
  nav->addChild(std::move(titleArea));
  nav->addChild(arrow("›", 1));
  term_frame::pinSize(*nav, frame.gridWidth, frame.rowHeight);
  return nav;
}

// `weekStart`: the first day of the grid row, `week` rows below the grid's first day.
std::unique_ptr<Flex> CalendarQuickTab::buildWeekRow(const CalendarFrame& frame, chr::sys_days weekStart) {
  auto row = ui::row({.align = FlexAlign::Center});
  if (frame.showWeeks) {
    const int thursdayColumn = (kThursday - frame.firstDay + kDaysPerWeek) % kDaysPerWeek;
    row->addChild(centeredCell(
        frame, frame.weekWidth,
        frameLabel(
            frame, std::format("{:%V}", weekStart + chr::days{thursdayColumn}),
            colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.7F)
        )
    ));
  }
  for (int col = 0; col < kDaysPerWeek; ++col) {
    const chr::year_month_day date{weekStart + chr::days{col}};
    auto area = ui::inputArea(
        {.onClick = [this, date](const InputArea::PointerData&) { select(date); }},
        buildDayCell(frame, date, m_selected)
    );
    area->setSize(frame.cellWidth, frame.rowHeight);
    row->addChild(std::move(area));
  }
  term_frame::pinSize(*row, frame.gridWidth, frame.rowHeight);
  return row;
}

std::unique_ptr<Flex> CalendarQuickTab::buildMonthBox(const CalendarFrame& frame) {
  auto box = term_frame::box("¹", "calendar", frame.scale);
  auto content = ui::column({.align = FlexAlign::Start, .gap = 0.0F});
  content->addChild(buildMonthNav(frame));
  content->addChild(buildWeekdayRow(frame));

  const chr::sys_days firstOfMonth{m_selected.year() / m_selected.month() / 1};
  const int offset =
      (static_cast<int>(chr::weekday{firstOfMonth}.c_encoding()) - frame.firstDay + kDaysPerWeek) % kDaysPerWeek;
  const chr::sys_days gridStart = firstOfMonth - chr::days{offset};
  for (int week = 0; week < kGridWeeks; ++week) {
    content->addChild(buildWeekRow(frame, gridStart + chr::days{week * kDaysPerWeek}));
  }
  term_frame::pinSize(*content, frame.gridWidth, frame.rowHeight * static_cast<float>(kGridRows));
  box->addChild(std::move(content));
  term_frame::pinSize(*box, frame.monthBoxWidth, frame.boxHeight);
  return box;
}

// ---- ² agenda -----------------------------------------------------------------------------------------

std::unique_ptr<Flex> CalendarQuickTab::buildAgendaBox(const CalendarFrame& frame) const {
  auto box = term_frame::box("²", "agenda", frame.scale);
  const float listWidth = std::max(0.0F, frame.agendaBoxWidth - 2.0F * term_frame::boxInset(frame.scale));
  AgendaList list(frame, listWidth);

  const AgendaEvents events = collectAgendaEvents(frame);
  const std::string_view syncError = syncErrorText(m_calendar);
  const bool configured = m_calendar != nullptr
      && m_calendar->enabled()
      && m_config != nullptr
      && !m_config->config().calendar.accounts.empty();
  const bool snapshotValid = frame.snapshot != nullptr && frame.snapshot->valid;
  // Nothing on the selected day or in the next seven: one dim line instead of three.
  const bool nothingSoon =
      configured && syncError.empty() && events.onSelectedDay.empty() && events.upcoming.empty() && snapshotValid;

  list.addLine(formatStrftime("%a %-d %b", tmOf(m_selected)), dimColor());
  if (nothingSoon) {
    list.addLine("no events in 7 days", dimColor());
  } else {
    addSelectedDay(list, frame, events.onSelectedDay, syncError, configured);
  }
  if (configured && !nothingSoon) {
    addUpcoming(list, frame, events.upcoming, syncError, !events.onSelectedDay.empty());
  }
  if (m_tasks != nullptr) {
    addTasks(list, *m_tasks, frame.today);
  }

  box->addChild(list.finish());
  term_frame::pinSize(*box, frame.agendaBoxWidth, frame.boxHeight);
  return box;
}
