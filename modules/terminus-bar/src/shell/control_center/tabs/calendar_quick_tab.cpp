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
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  namespace chr = std::chrono;

  // Logical px (unscaled). The panel body is laid out from these so quickPanelHeight() never needs nodes.
  constexpr float kCellWidth = 24.0F;
  constexpr float kWeekColumnWidth = 20.0F;
  constexpr float kRowHeight = term_frame::kRowHeight;
  constexpr int kAgendaRows = 12; // events first, then up to three open tasks (a long title wraps to two lines)
  constexpr int kMaxTasks = 3;
  constexpr float kTitleRowHeight = 17.0F;
  constexpr float kHeroHeight = 46.0F;
  constexpr float kHeroClockSize = 32.0F;
  constexpr float kBoxHeight =
      2.0F * (Style::spaceSm + Style::borderWidth) + kTitleRowHeight + Style::spaceXs + kAgendaRows * kRowHeight;
  constexpr float kPanelHeight = kHeroHeight + Style::spaceSm + kBoxHeight;
  constexpr float kFallbackBodyWidth = 442.0F;
  constexpr int kMaxSelectedRows = 3;
  constexpr int kNextDays = 7;
  constexpr float kPassedAlpha = 0.55F;

  using term_frame::dimColor;

  std::tm localTm(std::chrono::system_clock::time_point time) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
    localtime_r(&raw, &local);
    return local;
  }

  chr::year_month_day today() {
    const std::tm local = localTm(chr::system_clock::now());
    return chr::year{local.tm_year + 1900} / chr::month{static_cast<unsigned>(local.tm_mon + 1)}
        / chr::day{static_cast<unsigned>(local.tm_mday)};
  }

  int keyOf(chr::year_month_day date) {
    return static_cast<int>(date.year()) * 10000 + static_cast<int>(static_cast<unsigned>(date.month())) * 100
        + static_cast<int>(static_cast<unsigned>(date.day()));
  }

  int keyOf(chr::system_clock::time_point time) {
    const std::tm local = localTm(time);
    return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
  }

  std::tm tmOf(chr::year_month_day date) {
    std::tm value{};
    value.tm_year = static_cast<int>(date.year()) - 1900;
    value.tm_mon = static_cast<int>(static_cast<unsigned>(date.month())) - 1;
    value.tm_mday = static_cast<int>(static_cast<unsigned>(date.day()));
    value.tm_wday = static_cast<int>(chr::weekday{chr::sys_days{date}}.c_encoding());
    return value;
  }

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

  struct AgendaItem {
    const CalendarEvent* event = nullptr;
    int startKey = 0;
  };

  chr::year_month_day addMonths(chr::year_month_day date, int months) {
    const chr::year_month target = chr::year_month{date.year(), date.month()} + chr::months{months};
    const chr::day last = (target / chr::last).day();
    return target / (date.day() > last ? last : date.day());
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
  m_lastMinuteKey = minuteKey();
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
    if (minuteKey() != m_lastMinuteKey) {
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

float CalendarQuickTab::quickPanelHeight(float width, float fallback) const {
  (void)width;
  (void)fallback;
  return kPanelHeight;
}

std::string CalendarQuickTab::quickPanelHints() const { return "←→ day  PgUp/Dn month  t today"; }

std::string CalendarQuickTab::quickPanelStatus() const {
  const chr::year_month_day now = today();
  const chr::days yday = chr::sys_days{now} - chr::sys_days{now.year() / chr::January / 1} + chr::days{1};
  return std::format("week {:%V} · day {}", chr::sys_days{now}, yday.count());
}

bool CalendarQuickTab::handleQuickPanelKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  (void)modifiers;
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
    days = -7;
    break;
  case XKB_KEY_Down:
    days = 7;
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

void CalendarQuickTab::shiftDays(int days) {
  select(chr::year_month_day{chr::sys_days{m_selected} + chr::days{days}});
}

void CalendarQuickTab::shiftMonths(int months) { select(addMonths(m_selected, months)); }

void CalendarQuickTab::markDirty() {
  m_dirty = true;
  PanelManager::instance().refresh();
}

std::string CalendarQuickTab::minuteKey() const { return formatLocalTime("%Y%m%d%H%M"); }

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
  if (const std::string key = minuteKey(); key != m_lastMinuteKey) {
    m_dirty = true;
  }
  if (!m_dirty) {
    return;
  }
  rebuild();
  m_root->setSize(m_bodyWidth, m_bodyHeight);
  m_root->layout(renderer);
}

void CalendarQuickTab::rebuild() {
  if (m_root == nullptr) {
    return;
  }
  m_dirty = false;
  m_lastMinuteKey = minuteKey();
  while (!m_root->children().empty()) {
    m_root->removeChild(m_root->children().front().get());
  }

  const float s = contentScale();
  const float font = Style::fontSizeCaption * s;
  const std::string family(term_frame::kFont);
  const ColorSpec textColor = colorSpecFromRole(ColorRole::OnSurface);
  const ColorSpec primary = colorSpecFromRole(ColorRole::Primary);
  const float inset = Style::spaceSm * s + Style::borderWidth;
  const float bodyWidth = (m_bodyWidth > 0.0F ? m_bodyWidth : kFallbackBodyWidth * s);
  const float rowH = kRowHeight * s;
  const float boxH = kBoxHeight * s;
  const float gap = Style::spaceSm * s;

  const bool showWeeks = m_config != nullptr && m_config->config().controlCenter.calendarTab.showWeekNumbers;
  const int firstDay = m_config != nullptr
      ? calendar_view::resolveFirstDayOfWeek(m_config->config().controlCenter.calendarTab.firstDayOfWeek)
      : localeFirstDayOfWeek();

  const auto nowTime = chr::system_clock::now();
  const chr::year_month_day todayDate = today();
  const int todayKey = keyOf(todayDate);
  const int selectedKey = keyOf(m_selected);
  const CalendarSnapshot* snapshot = m_calendar != nullptr ? &m_calendar->snapshot() : nullptr;

  // ---- hero -------------------------------------------------------------------------------------
  {
    const std::tm nowTm = localTm(nowTime);
    const chr::days left =
        chr::sys_days{todayDate.year() / chr::December / 31} - chr::sys_days{todayDate};
    auto hero = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * s});
    hero->addChild(
        ui::label({
            .text = formatStrftime("%H:%M", nowTm),
            .fontSize = kHeroClockSize * s,
            .fontWeight = FontWeight::Medium,
            .fontFamily = family,
            .color = primary,
            .maxLines = 1,
        })
    );
    auto info = ui::column({.align = FlexAlign::Start, .justify = FlexJustify::Center, .gap = 0.0F, .flexGrow = 1.0F});
    info->addChild(
        ui::label({
            .text = formatStrftime("%A %-d %B %Y", nowTm),
            .fontSize = font,
            .fontFamily = family,
            .color = dimColor(),
            .maxWidth = std::max(0.0F, bodyWidth - term_frame::monoWidth(6, kHeroClockSize * s) - Style::spaceMd * s),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
        })
    );
    info->addChild(
        ui::label({
            .text = std::format("{} days left in {}", left.count(), static_cast<int>(todayDate.year())),
            .fontSize = font,
            .fontFamily = family,
            .color = dimColor(),
            .maxLines = 1,
        })
    );
    hero->addChild(std::move(info));
    term_frame::pinSize(*hero, bodyWidth, kHeroHeight * s);
    m_root->addChild(std::move(hero));
  }

  // ---- geometry shared by both boxes ------------------------------------------------------------
  const float cellW = kCellWidth * s;
  const float weekW = showWeeks ? kWeekColumnWidth * s : 0.0F;
  const float gridInnerW = 7.0F * cellW + weekW;
  const float leftW = gridInnerW + 2.0F * inset;
  const float rightW = std::max(0.0F, bodyWidth - leftW - gap);

  auto columns = ui::row({.align = FlexAlign::Start, .gap = gap});

  // ---- ¹ month grid -----------------------------------------------------------------------------
  {
    auto box = term_frame::box("¹", "calendar", s);
    auto content = ui::column({.align = FlexAlign::Start, .gap = 0.0F});

    // Navigation: ‹  November 2026  ›
    auto nav = ui::row({.align = FlexAlign::Center});
    const auto arrow = [&](const char* glyph, int delta) {
      auto area = ui::inputArea(
          {.onClick = [this, delta](const InputArea::PointerData&) { shiftMonths(delta); }},
          ui::label({
              .text = glyph,
              .fontSize = font,
              .fontFamily = family,
              .color = primary,
              .maxLines = 1,
          })
      );
      area->setSize(cellW, rowH);
      return area;
    };
    nav->addChild(arrow("‹", -1));
    auto titleArea = ui::inputArea(
        {.onClick = [this](const InputArea::PointerData&) { select(today()); }},
        ui::label({
            .text = monthTitle(m_selected),
            .fontSize = font,
            .fontFamily = family,
            .color = textColor,
            .width = gridInnerW - 2.0F * cellW,
            .maxLines = 1,
            .textAlign = TextAlign::Center,
        })
    );
    titleArea->setSize(gridInnerW - 2.0F * cellW, rowH);
    nav->addChild(std::move(titleArea));
    nav->addChild(arrow("›", 1));
    term_frame::pinSize(*nav, gridInnerW, rowH);
    content->addChild(std::move(nav));

    // Weekday initials.
    auto weekdays = ui::row({.align = FlexAlign::Center});
    if (showWeeks) {
      auto cell = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center});
      cell->addChild(ui::label({.text = "wk", .fontSize = font, .fontFamily = family, .color = dimColor(), .maxLines = 1}));
      term_frame::pinSize(*cell, weekW, rowH);
      weekdays->addChild(std::move(cell));
    }
    for (int i = 0; i < 7; ++i) {
      std::tm value{};
      value.tm_wday = (firstDay + i) % 7;
      value.tm_mday = 1;
      auto cell = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center});
      cell->addChild(
          ui::label({
              .text = firstCodepoints(formatStrftime("%a", value), 2),
              .fontSize = font,
              .fontFamily = family,
              .color = textColor,
              .maxLines = 1,
          })
      );
      term_frame::pinSize(*cell, cellW, rowH);
      weekdays->addChild(std::move(cell));
    }
    term_frame::pinSize(*weekdays, gridInnerW, rowH);
    content->addChild(std::move(weekdays));

    // Day cells: six rows starting on the configured week start.
    const chr::sys_days firstOfMonth{m_selected.year() / m_selected.month() / 1};
    const int offset = (static_cast<int>(chr::weekday{firstOfMonth}.c_encoding()) - firstDay + 7) % 7;
    const chr::sys_days gridStart = firstOfMonth - chr::days{offset};
    const int thursdayColumn = (4 - firstDay + 7) % 7;

    std::vector<std::pair<int, int>> ranges;
    if (snapshot != nullptr) {
      ranges.reserve(snapshot->events.size());
      for (const CalendarEvent& event : snapshot->events) {
        ranges.push_back(calendar_view::eventDayKeys(event));
      }
    }
    const auto hasEvents = [&ranges](int key) {
      return std::any_of(ranges.begin(), ranges.end(), [key](const auto& range) {
        return key >= range.first && key <= range.second;
      });
    };

    for (int week = 0; week < 6; ++week) {
      auto row = ui::row({.align = FlexAlign::Center});
      if (showWeeks) {
        auto cell = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center});
        cell->addChild(
            ui::label({
                .text = std::format("{:%V}", gridStart + chr::days{week * 7 + thursdayColumn}),
                .fontSize = font,
                .fontFamily = family,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.7F),
                .maxLines = 1,
            })
        );
        term_frame::pinSize(*cell, weekW, rowH);
        row->addChild(std::move(cell));
      }
      for (int col = 0; col < 7; ++col) {
        const chr::year_month_day date{gridStart + chr::days{week * 7 + col}};
        const int key = keyOf(date);
        const bool inMonth = date.month() == m_selected.month() && date.year() == m_selected.year();
        const bool isToday = key == todayKey;
        const bool isSelected = key == selectedKey;

        ColorSpec labelColor = inMonth ? textColor : colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.55F);
        if (isToday) {
          labelColor = colorSpecFromRole(ColorRole::Surface);
        } else if (isSelected) {
          labelColor = primary;
        }
        auto cell = ui::column({
            .align = FlexAlign::Center,
            .justify = FlexJustify::Center,
            .fill = isToday ? std::optional<ColorSpec>(primary) : std::nullopt,
            .radius = 0.0F,
            .border = isSelected ? std::optional<ColorSpec>(isToday ? colorSpecFromRole(ColorRole::OnSurface) : primary)
                                 : std::nullopt,
            .borderWidth = Style::borderWidth,
        });
        cell->addChild(
            ui::label({
                .text = std::to_string(static_cast<unsigned>(date.day())),
                .fontSize = font,
                .fontWeight = isToday || isSelected ? FontWeight::Bold : FontWeight::Normal,
                .fontFamily = family,
                .color = labelColor,
                .maxLines = 1,
            })
        );
        if (hasEvents(key)) {
          const float dotW = 4.0F * s;
          const float dotH = 2.0F * s;
          auto dot = ui::box({
              .fill = isToday ? colorSpecFromRole(ColorRole::Surface) : primary,
              .radius = 0.0F,
              .width = dotW,
              .height = dotH,
              .participatesInLayout = false,
          });
          dot->setPosition(std::round((cellW - dotW) * 0.5F), rowH - dotH - 3.0F * s);
          cell->addChild(std::move(dot));
        }
        term_frame::pinSize(*cell, cellW, rowH);

        auto area = ui::inputArea({.onClick = [this, date](const InputArea::PointerData&) { select(date); }}, std::move(cell));
        area->setSize(cellW, rowH);
        row->addChild(std::move(area));
      }
      term_frame::pinSize(*row, gridInnerW, rowH);
      content->addChild(std::move(row));
    }
    term_frame::pinSize(*content, gridInnerW, rowH * 8.0F);
    box->addChild(std::move(content));
    term_frame::pinSize(*box, leftW, boxH);
    columns->addChild(std::move(box));
  }

  // ---- ² agenda ---------------------------------------------------------------------------------
  {
    auto box = term_frame::box("²", "agenda", s);
    auto list = ui::column({.align = FlexAlign::Start, .gap = 0.0F});

    const float timeW = term_frame::monoWidth(11, font);
    const float listW = std::max(0.0F, rightW - 2.0F * inset);
    const float titleW = std::max(0.0F, listW - timeW - Style::spaceXs * s);
    int rows = 0;

    const auto addLine = [&](std::string text, const ColorSpec& color) {
      auto line = ui::label({
          .text = std::move(text),
          .fontSize = font,
          .fontFamily = family,
          .color = color,
          .maxWidth = listW,
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      });
      auto row = ui::row({.align = FlexAlign::Center});
      row->addChild(std::move(line));
      term_frame::pinSize(*row, listW, rowH);
      list->addChild(std::move(row));
      ++rows;
    };
    // A wrapping row (tasks) takes two lines when the title does not fit on one.
    const auto wrapsToTwoLines = [&](const std::string& title) {
      const std::size_t perLine = std::max<std::size_t>(1, static_cast<std::size_t>(titleW / (font * 0.6F)));
      return title.size() > perLine;
    };
    const auto addEvent = [&](const std::string& when, const std::string& title, float alpha, bool wrap = false) {
      const bool twoLines = wrap && wrapsToTwoLines(title);
      auto row = ui::row({.align = twoLines ? FlexAlign::Start : FlexAlign::Center, .gap = Style::spaceXs * s});
      row->addChild(
          ui::label({
              .text = when,
              .fontSize = font,
              .fontFamily = family,
              .color = dimColor(),
              .width = timeW,
              .maxLines = 1,
          })
      );
      row->addChild(
          ui::label({
              .text = title.empty() ? "(untitled)" : title,
              .fontSize = font,
              .fontFamily = family,
              .color = colorSpecFromRole(ColorRole::OnSurface, alpha),
              .maxWidth = titleW,
              .maxLines = twoLines ? 2 : 1,
              .ellipsize = TextEllipsize::End,
          })
      );
      term_frame::pinSize(*row, listW, twoLines ? 2.0F * rowH : rowH);
      list->addChild(std::move(row));
      rows += twoLines ? 2 : 1;
    };

    const auto timeOf = [](const CalendarEvent& event) {
      return formatStrftime("%H:%M", localTm(event.start));
    };

    std::vector<AgendaItem> selectedItems;
    std::vector<AgendaItem> nextItems;
    if (snapshot != nullptr) {
      const chr::sys_days windowEnd = chr::sys_days{todayDate} + chr::days{kNextDays};
      const int endKey = keyOf(chr::year_month_day{windowEnd});
      for (const CalendarEvent& event : snapshot->events) {
        const auto [startKey, endDayKey] = calendar_view::eventDayKeys(event);
        const bool onSelected = selectedKey >= startKey && selectedKey <= endDayKey;
        if (onSelected) {
          selectedItems.push_back({&event, startKey});
        } else if (startKey >= todayKey && startKey <= endKey && !calendar_view::eventPassed(event, nowTime)) {
          nextItems.push_back({&event, startKey});
        }
      }
    }
    const auto byStart = [](const AgendaItem& a, const AgendaItem& b) {
      if (a.event->allDay != b.event->allDay) {
        return a.event->allDay;
      }
      return a.event->start < b.event->start;
    };
    std::sort(selectedItems.begin(), selectedItems.end(), byStart);
    std::sort(nextItems.begin(), nextItems.end(), [](const AgendaItem& a, const AgendaItem& b) {
      return a.event->start < b.event->start;
    });

    addLine(formatStrftime("%a %-d %b", tmOf(m_selected)), dimColor());
    std::string syncError;
    if (m_calendar != nullptr) {
      switch (m_calendar->syncError()) {
      case CalendarService::SyncError::AuthFailed:
        syncError = "calendar: auth failed";
        break;
      case CalendarService::SyncError::Unreachable:
        syncError = "calendar: server unreachable";
        break;
      case CalendarService::SyncError::NoCalendars:
        syncError = "connected · no event calendar";
        break;
      case CalendarService::SyncError::Failed:
        syncError = "calendar: sync failed";
        break;
      case CalendarService::SyncError::None:
        break;
      }
    }
    const ColorSpec errorColor = colorSpecFromRole(ColorRole::Error);
    const bool configured = m_calendar != nullptr && m_calendar->enabled() && m_config != nullptr
        && !m_config->config().calendar.accounts.empty();
    // Nothing in the selected day or the next seven: one dim line instead of three.
    const bool noneAtAll = configured && syncError.empty() && selectedItems.empty() && nextItems.empty()
        && snapshot != nullptr && snapshot->valid;
    if (noneAtAll) {
      addLine("no events in 7 days", dimColor());
    } else if (selectedItems.empty() && !syncError.empty()) {
      addLine(syncError, errorColor);
    } else if (selectedItems.empty()) {
      addLine(configured ? "connected · no events" : "no calendar connected", dimColor());
    } else {
      const bool isToday = selectedKey == todayKey;
      const int total = static_cast<int>(selectedItems.size());
      // The last visible slot becomes "+N more" when the day overflows.
      const int shown = total > kMaxSelectedRows ? kMaxSelectedRows - 1 : total;
      for (int i = 0; i < shown; ++i) {
        const CalendarEvent& event = *selectedItems[static_cast<std::size_t>(i)].event;
        const float alpha = isToday && calendar_view::eventPassed(event, nowTime) ? kPassedAlpha : 1.0F;
        addEvent(event.allDay ? "all day" : timeOf(event), event.title, alpha);
      }
      if (shown < total) {
        addLine(std::format("+{} more", total - shown), dimColor());
      }
    }

    if (configured && !noneAtAll && rows < kAgendaRows - 1) {
      addLine("next 7 days", dimColor());
      int shown = 0;
      for (const AgendaItem& item : nextItems) {
        if (rows >= kAgendaRows) {
          break;
        }
        const CalendarEvent& event = *item.event;
        std::string when = formatStrftime("%a", localTm(event.start));
        when += event.allDay ? " all-day" : " " + timeOf(event);
        addEvent(when, event.title, 1.0F);
        ++shown;
      }
      if (!syncError.empty() && !selectedItems.empty() && rows < kAgendaRows) {
        addLine(syncError, errorColor);
      } else if (shown == 0 && rows < kAgendaRows) {
        addLine(snapshot != nullptr && snapshot->valid ? "none" : "syncing…", dimColor());
      }
    }

    // Open tasks from the Tasks collection under the events: dated ones first (soonest first), then undated.
    // Overdue ones read "late <date>".
    if (m_tasks != nullptr && rows < kAgendaRows - 1) {
      std::vector<const TaskItem*> open;
      for (const TaskItem& task : m_tasks->snapshot().items) {
        if (!task.completed) {
          open.push_back(&task);
        }
      }
      std::stable_sort(open.begin(), open.end(), [](const TaskItem* a, const TaskItem* b) {
        if (a->dueDate.empty() != b->dueDate.empty()) {
          return !a->dueDate.empty();
        }
        return a->dueDate < b->dueDate;
      });
      // The header only appears with at least its first task under it.
      if (!open.empty() && rows + 1 + (wrapsToTwoLines(open.front()->title) ? 2 : 1) <= kAgendaRows) {
        const std::string todayIso = std::format(
            "{:04d}-{:02d}-{:02d}", static_cast<int>(todayDate.year()), static_cast<unsigned>(todayDate.month()),
            static_cast<unsigned>(todayDate.day())
        );
        static constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        addLine(std::format("tasks · {} open", open.size()), dimColor());
        int shownTasks = 0;
        for (const TaskItem* task : open) {
          if (shownTasks >= kMaxTasks || rows + (wrapsToTwoLines(task->title) ? 2 : 1) > kAgendaRows) {
            break;
          }
          ++shownTasks;
          std::string when;
          if (task->dueDate.size() == 10) {
            const int month = std::atoi(task->dueDate.substr(5, 2).c_str());
            const int day = std::atoi(task->dueDate.substr(8, 2).c_str());
            when = std::format("{} {}", day, month >= 1 && month <= 12 ? kMonths[month - 1] : "?");
            if (task->dueDate < todayIso) {
              when = "late " + when;
            }
          }
          addEvent(when, task->title, 1.0F, true);
        }
      }
    }

    term_frame::pinSize(*list, listW, rowH * static_cast<float>(kAgendaRows));
    box->addChild(std::move(list));
    term_frame::pinSize(*box, rightW, boxH);
    columns->addChild(std::move(box));
  }

  term_frame::pinSize(*columns, bodyWidth, boxH);
  m_root->addChild(std::move(columns));
}
