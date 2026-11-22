// Control-center Tasks tab: browse, edit and trash CalDAV tasks held by TaskService.
// synchronize() runs when the tab or the service snapshot changed; it diffs the row widgets against the snapshot.

#include "shell/control_center/tabs/tasks_tab.h"

#include "render/scene/input_dispatcher.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/scroll_into_view.h"
#include "ui/split_pane_focus.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <optional>
#include <utility>

namespace {

  constexpr std::size_t NotesPreviewMaxCodePoints = 180;
  constexpr int PreviewMaxLines = 2;
  constexpr int TitleMaxLines = 2;
  constexpr int UpcomingDays = 7; // matches the "Upcoming (next 7 days)" filter label
  constexpr float NotesInputHeightPx = 88.0F;
  constexpr float ViewPickerMinWidthPx = 132.0F;
  constexpr float SortPickerMinWidthPx = 146.0F;
  constexpr float NarrowContentWidthPx = 430.0F;

  // Option order is the enum order of TasksTab::View / DateFilter; selection indices are cast directly.
  const std::vector<std::string> ViewOptions{"Active", "Completed", "Trash", "History"};
  const std::vector<std::string> DateOptions{"All dates", "Today", "Overdue", "Upcoming (next 7 days)", "No date"};
  const std::vector<std::string> SortOptions{"Sort: Due", "Sort: Title"};

  ColorSpec taskAccent(float opacity = 1.0F) {
    return fixedColorSpec(rgba(248.0F / 255.0F, 205.0F / 255.0F, 1.0F / 255.0F, opacity));
  }

  void applyTaskCardStyle(Flex& card, float scale, float opacity) {
    control_center::applySectionCardStyle(card, scale, opacity);
    card.setFill(fixedColorSpec(rgba(17.0F / 255.0F, 19.0F / 255.0F, 23.0F / 255.0F, 1.0F)));
    card.setBorder(taskAccent(), Style::borderWidth * scale);
  }

  void styleTaskButtons(Node& node) {
    if (auto* button = dynamic_cast<Button*>(&node);
        button != nullptr && button->variant() != ButtonVariant::Destructive) {
      auto colors = Button::defaultPalette(button->variant());
      const auto ink = fixedColorSpec(rgba(0.04F, 0.04F, 0.04F, 1.0F));
      const bool filled = button->variant() == ButtonVariant::Primary || button->variant() == ButtonVariant::Secondary;
      colors.borderWidth = Style::borderWidth;
      colors.normal.border = taskAccent(0.5F);
      if (filled) {
        colors.normal.bg = taskAccent();
        colors.normal.label = ink;
      }
      colors.hover = {taskAccent(0.18F), taskAccent(), taskAccent()};
      colors.pressed = {taskAccent(), taskAccent(), ink};
      colors.selected = Button::ButtonStateColors{taskAccent(), taskAccent(), ink};
      colors.disabled = {taskAccent(0.08F), taskAccent(0.2F), taskAccent(0.4F)};
      button->setCustomPalette(std::move(colors));
    }
    for (const auto& child : node.children())
      styleTaskButtons(*child);
  }

  void show(Node* node, bool visible) {
    node->setVisible(visible);
    node->setParticipatesInLayout(visible);
  }

  std::unique_ptr<Label> taskLabel(std::string text, float scale, Label** out = nullptr) {
    return ui::label({
        .out = out,
        .text = std::move(text),
        .fontSize = Style::fontSizeMini * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
    });
  }

  ui::ButtonProps compactButton(
      std::string text, ButtonVariant variant, float scale, std::function<void()> onClick, Button** out = nullptr
  ) {
    return {
        .out = out,
        .text = std::move(text),
        .fontSize = Style::fontSizeMini * scale,
        .controlHeight = Style::controlHeightSm * scale,
        .variant = variant,
        .onClick = std::move(onClick),
    };
  }

  // A column that starts hidden; the confirmation and password panels are shown on demand.
  std::unique_ptr<Flex> hiddenColumn(Flex** out, float scale) {
    return ui::column({
        .out = out,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .visible = false,
        .participatesInLayout = false,
    });
  }

  // YYYY-MM-DD of the local day `days` from `time`.
  std::string localDate(std::time_t time, int days = 0) {
    std::tm local{};
    localtime_r(&time, &local);
    local.tm_mday += days;
    local.tm_isdst = -1;
    std::mktime(&local);
    char date[11]{};
    std::strftime(date, sizeof(date), "%Y-%m-%d", &local);
    return date;
  }

  std::string localTimestamp(std::int64_t timestamp) {
    if (timestamp == 0)
      return "before local history (time unknown)";
    const auto time = static_cast<std::time_t>(timestamp);
    std::tm local{};
    localtime_r(&time, &local);
    char formatted[32]{};
    std::strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M", &local);
    return formatted;
  }

  // `query` is already lower-case.
  bool matchesSearch(const std::string& value, const std::string& query) {
    return query.empty() || StringUtils::toLower(value).find(query) != std::string::npos;
  }

  std::string notesPreview(const std::string& value) {
    auto preview = StringUtils::truncateUtf8CodePoints(value, NotesPreviewMaxCodePoints);
    if (preview.size() < value.size())
      preview += "…";
    std::replace(preview.begin(), preview.end(), '\n', ' ');
    std::replace(preview.begin(), preview.end(), '\r', ' ');
    return preview;
  }

  bool isOverdue(const TaskItem& item, const std::string& today) {
    return !item.completed && !item.dueDate.empty() && item.dueDate < today;
  }

  std::string taskDetail(const TaskItem& item, const std::string& today) {
    std::string detail = item.dueDate.empty() ? "No due date"
        : isOverdue(item, today)              ? "Overdue · " + item.dueDate
        : item.dueDate == today               ? "Due today · " + item.dueDate
                                              : "Due " + item.dueDate;
    if (item.pending)
      detail += " · Pending";
    if (item.conflict)
      detail += " · Conflict";
    return detail;
  }

  std::string trashDetail(const TrashedTask& archived) {
    const auto& item = archived.task;
    std::string detail = "Deleted "
        + localTimestamp(archived.deletedAt)
        + " · "
        + archived.source
        + (item.completed ? " · Completed" : " · Active")
        + (item.dueDate.empty() ? " · No due date" : " · Due " + item.dueDate);
    if (archived.conflict)
      detail += "\nDeletion conflict: resolve the original task before restoring or removing this copy.";
    else if (archived.pending && !archived.restorable)
      detail += "\nDeletion pending or uncertain: sync to resolve before restoring or removing this copy.";
    else if (archived.pending)
      detail += "\nUnsent deletion: Undo delete keeps the original task.";
    else if (!archived.restorable)
      detail += "\nRestore unavailable: unlock the local store and connect to a writable collection.";
    return detail;
  }

  // Trash rows matching the search, newest deletion first.
  std::vector<std::size_t> visibleTrash(const TaskSnapshot& snapshot, const std::string& query) {
    std::vector<std::size_t> visible;
    visible.reserve(snapshot.trash.size());
    for (std::size_t index = 0; index < snapshot.trash.size(); ++index) {
      const auto& item = snapshot.trash[index];
      if (matchesSearch(item.task.title, query) || matchesSearch(item.task.description, query))
        visible.push_back(index);
    }
    std::ranges::sort(visible, [&](std::size_t left, std::size_t right) {
      const auto& a = snapshot.trash[left];
      const auto& b = snapshot.trash[right];
      return a.deletedAt != b.deletedAt ? a.deletedAt > b.deletedAt : a.id < b.id;
    });
    return visible;
  }

  // History rows matching the search, newest first.
  std::vector<std::size_t> visibleHistory(const TaskSnapshot& snapshot, const std::string& query) {
    std::vector<std::size_t> visible;
    visible.reserve(snapshot.history.size());
    for (std::size_t index = 0; index < snapshot.history.size(); ++index) {
      const auto& entry = snapshot.history[index];
      if (matchesSearch(entry.title, query)
          || matchesSearch(entry.details, query)
          || matchesSearch(entry.action, query)
          || matchesSearch(entry.source, query))
        visible.push_back(index);
    }
    std::ranges::sort(visible, [&](std::size_t left, std::size_t right) {
      const auto& a = snapshot.history[left];
      const auto& b = snapshot.history[right];
      return a.occurredAt != b.occurredAt ? a.occurredAt > b.occurredAt : a.id < b.id;
    });
    return visible;
  }

} // namespace

TasksTab::TasksTab(TaskService* tasks) : m_tasks(tasks) {}

// ---- Widget tree --------------------------------------------------------------------------------------------------

std::unique_ptr<Flex> TasksTab::create() {
  const float scale = contentScale();
  auto root = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [this, scale](Flex& card) {
        applyTaskCardStyle(card, scale, panelCardOpacity());
        card.setClipChildren(true);
      },
  });
  root->addChild(buildHeader(scale));
  root->addChild(taskLabel("", scale, &m_status));
  root->addChild(buildBrowseToolbar(scale));
  root->addChild(buildFilterToolbar(scale));
  root->addChild(taskLabel("", scale, &m_counts));
  root->addChild(buildBody(scale));
  styleTaskButtons(*root);
  m_viewPicker->setOptionIndicators(std::vector<ColorSpec>(ViewOptions.size(), taskAccent()));
  m_viewPicker->setMinWidth(ViewPickerMinWidthPx * scale);
  m_dateFilter->setOptionIndicators(std::vector<ColorSpec>(DateOptions.size(), taskAccent()));
  m_sort->setOptionIndicators(std::vector<ColorSpec>(SortOptions.size(), taskAccent()));
  m_sort->setMinWidth(SortPickerMinWidthPx * scale);
  m_dirty = true;
  scheduleDayRefresh();
  return root;
}

std::unique_ptr<Flex> TasksTab::buildHeader(float scale) {
  auto header = control_center::makeCardHeaderRow("Tasks", scale, taskAccent());
  header->addChild(
      ui::button(compactButton("New", ButtonVariant::Primary, scale, [this]() { openEditor(""); }, &m_new))
  );
  header->addChild(
      ui::button({
          .out = &m_refresh,
          .glyph = "refresh",
          .glyphSize = Style::baseGlyphSize * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = "Sync tasks now",
          .onClick = [this]() {
            if (m_tasks != nullptr) {
              m_notice.clear();
              m_tasks->requestRefresh();
              requestUpdate();
            }
          },
      })
  );
  return header;
}

std::unique_ptr<Flex> TasksTab::buildBrowseToolbar(float scale) {
  return ui::row(
      {.out = &m_browseToolbar, .align = FlexAlign::Stretch, .gap = Style::spaceXs * scale},
      ui::select({
          .out = &m_viewPicker,
          .options = ViewOptions,
          .selectedIndex = static_cast<std::size_t>(m_view),
          .fontSize = Style::fontSizeMini * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .surfaceOpacity = panelCardOpacity(),
          .onSelectionChanged =
              [this](std::size_t index, std::string_view) {
                m_view = static_cast<View>(index);
                m_localAction = LocalConfirmation::None;
                m_resetScroll = true;
                requestUpdate();
              },
      }),
      ui::input({
          .out = &m_search,
          .value = m_query,
          .placeholder = "Search title, notes, history…",
          .fontSize = Style::fontSizeMini * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .clearButtonEnabled = true,
          .minLayoutWidth = 0.0F,
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0F,
          .onChange = [this](const std::string& value) {
            m_query = value;
            m_resetScroll = true;
            requestUpdate();
          },
      })
  );
}

std::unique_ptr<Flex> TasksTab::buildFilterToolbar(float scale) {
  return ui::row(
      {.out = &m_filterToolbar, .align = FlexAlign::Stretch, .gap = Style::spaceXs * scale},
      ui::select({
          .out = &m_dateFilter,
          .options = DateOptions,
          .selectedIndex = static_cast<std::size_t>(m_date),
          .fontSize = Style::fontSizeMini * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0F,
          .onSelectionChanged =
              [this](std::size_t index, std::string_view) {
                m_date = static_cast<DateFilter>(index);
                m_resetScroll = true;
                requestUpdate();
              },
      }),
      ui::select({
          .out = &m_sort,
          .options = SortOptions,
          .selectedIndex = m_sortByTitle ? 1U : 0U,
          .fontSize = Style::fontSizeMini * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .surfaceOpacity = panelCardOpacity(),
          .onSelectionChanged =
              [this](std::size_t index, std::string_view) {
                m_sortByTitle = index == 1;
                m_resetScroll = true;
                requestUpdate();
              },
      }),
      ui::button(compactButton(
          "Clear local history…", ButtonVariant::Outline, scale,
          [this]() {
            if (m_tasks == nullptr)
              return;
            const auto& history = m_tasks->snapshot().history;
            m_confirmHistoryCount = history.size();
            m_confirmHistoryNewest = history.empty() ? "" : history.front().id;
            m_localAction = LocalConfirmation::ClearHistory;
            m_focusLocalPending = true;
            requestUpdate();
          },
          &m_clearHistory
      ))
  );
}

// Scrolling body: password prompt, editor, local-record confirmation, scope note, empty note, then the row list.
std::unique_ptr<ScrollView> TasksTab::buildBody(float scale) {
  auto scroll = ui::scrollView({
      .out = &m_scroll,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = 1.0F,
      .configure = [scale](ScrollView& view) {
        view.clearFill();
        view.clearBorder();
        view.content()->setAlign(FlexAlign::Stretch);
        view.content()->setGap(Style::spaceMd * scale);
      },
  });
  auto* content = scroll->content();
  content->addChild(buildPasswordCard(scale));
  content->addChild(buildEditor(scale));
  content->addChild(buildLocalConfirmation(scale));
  content->addChild(taskLabel("", scale, &m_localScope));
  content->addChild(taskLabel("", scale, &m_empty));
  content->addChild(
      ui::column({
          .out = &m_list,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceSm * scale,
      })
  );
  return scroll;
}

std::unique_ptr<Flex> TasksTab::buildPasswordCard(float scale) {
  auto card = hiddenColumn(&m_passwordCard, scale);
  card->addChild(taskLabel("Collection password", scale));
  card->addChild(
      ui::input({
          .out = &m_password,
          .placeholder = "Password",
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .passwordMode = true,
          .surfaceOpacity = panelCardOpacity(),
          .onSubmit = [this](const std::string&) { submitPassword(); },
      })
  );
  card->addChild(ui::button(compactButton("Save password", ButtonVariant::Secondary, scale, [this]() {
    submitPassword();
  })));
  return card;
}

std::unique_ptr<Flex> TasksTab::buildEditor(float scale) {
  auto editor = ui::column({
      .out = &m_editor,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .visible = m_editing,
      .participatesInLayout = m_editing,
  });
  editor->addChild(taskLabel("New task", scale, &m_editorTitle));
  editor->addChild(taskLabel("Title", scale));
  editor->addChild(
      ui::input({
          .out = &m_title,
          .value = m_draft.title,
          .placeholder = "Task title",
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .surfaceOpacity = panelCardOpacity(),
          .onChange =
              [this](const std::string& value) {
                m_draft.title = value;
                m_draftChanged = true;
              },
          .onSubmit = [this](const std::string&) { saveEditor(); },
      })
  );
  editor->addChild(taskLabel("Notes", scale));
  editor->addChild(
      ui::input({
          .out = &m_notes,
          .value = m_draft.description,
          .placeholder = "Notes (optional)",
          .fontSize = Style::fontSizeBody * scale,
          .surfaceOpacity = panelCardOpacity(),
          .height = NotesInputHeightPx * scale,
          .onChange =
              [this](const std::string& value) {
                m_draft.description = value;
                m_draftChanged = true;
              },
          .onSubmit = [this](const std::string&) { saveEditor(); },
          .configure = [](Input& input) { input.setMultiline(true); },
      })
  );
  editor->addChild(taskLabel("Due date · YYYY-MM-DD, or empty", scale));
  editor->addChild(
      ui::input({
          .out = &m_due,
          .value = m_draft.dueDate,
          .placeholder = "YYYY-MM-DD",
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .surfaceOpacity = panelCardOpacity(),
          .onChange =
              [this](const std::string& value) {
                m_draft.dueDate = value;
                m_draftChanged = true;
              },
          .onSubmit = [this](const std::string&) { saveEditor(); },
      })
  );
  editor->addChild(
      ui::row(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale},
          ui::button({
              .out = &m_save,
              .text = "Add task",
              .fontSize = Style::fontSizeMini * scale,
              .controlHeight = Style::controlHeightSm * scale,
              .variant = ButtonVariant::Primary,
              .flexGrow = 1.0F,
              .onClick = [this]() { saveEditor(); },
          }),
          ui::button(compactButton("Cancel", ButtonVariant::Ghost, scale, [this]() { cancelEditor(); }))
      )
  );
  editor->addChild(
      ui::button(compactButton(
          "Delete task…", ButtonVariant::Outline, scale,
          [this]() {
            if (const auto* item = selectedItem(); item != nullptr) {
              m_confirmingDelete = true;
              m_confirmingConflict = item->conflict;
              requestUpdate();
            }
          },
          &m_delete
      ))
  );
  editor->addChild(
      ui::button({
          .out = &m_copy,
          .text = "Copy to new task",
          .fontSize = Style::fontSizeMini * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = "Copy title, notes and due date into an unsaved active task; source stays unchanged",
          .onClick = [this]() { copyToNewTask(); },
      })
  );
  editor->addChild(buildDeleteConfirmation(scale));
  return editor;
}

std::unique_ptr<Flex> TasksTab::buildDeleteConfirmation(float scale) {
  auto confirmation = hiddenColumn(&m_confirmation, scale);
  confirmation->addChild(taskLabel("", scale, &m_confirmationTitle));
  confirmation->addChild(
      ui::button(compactButton(
          "Confirm delete", ButtonVariant::Destructive, scale,
          [this]() {
            const auto* item = selectedItem();
            if (m_tasks == nullptr || !m_confirmingDelete || item == nullptr) {
              return;
            }
            // A conflict discard must never turn into a remote delete after a refresh.
            if (item->conflict != m_confirmingConflict) {
              m_confirmingDelete = false;
              m_notice = "Task changed; review it before deleting.";
            } else if (m_tasks->removeTask(m_selectedId)) {
              cancelEditor();
            }
            requestUpdate();
          },
          &m_confirmDelete
      ))
  );
  confirmation->addChild(ui::button(compactButton("Keep task", ButtonVariant::Ghost, scale, [this]() {
    m_confirmingDelete = false;
    requestUpdate();
  })));
  return confirmation;
}

std::unique_ptr<Flex> TasksTab::buildLocalConfirmation(float scale) {
  auto confirmation = hiddenColumn(&m_localConfirmation, scale);
  confirmation->addChild(taskLabel("", scale, &m_localConfirmationTitle));
  confirmation->addChild(
      ui::button(compactButton(
          "Remove local copy", ButtonVariant::Destructive, scale, [this]() { confirmLocalAction(); }, &m_confirmLocal
      ))
  );
  confirmation->addChild(ui::button(compactButton("Cancel", ButtonVariant::Ghost, scale, [this]() {
    m_localAction = LocalConfirmation::None;
    requestUpdate();
  })));
  return confirmation;
}

// ---- Actions ------------------------------------------------------------------------------------------------------

void TasksTab::requestUpdate() {
  m_dirty = true;
  PanelManager::instance().refresh();
}

const TaskItem* TasksTab::selectedItem() const {
  if (m_tasks == nullptr || m_selectedId.empty()) {
    return nullptr;
  }
  const auto& items = m_tasks->snapshot().items;
  const auto found = std::ranges::find(items, m_selectedId, &TaskItem::id);
  return found == items.end() ? nullptr : &*found;
}

void TasksTab::openEditor(const std::string& id) {
  if (m_editing && m_draftChanged) {
    m_notice = "Save or cancel the current draft before opening another task.";
    requestUpdate();
    return;
  }
  m_selectedId = id;
  if (const auto* item = selectedItem(); item != nullptr) {
    m_draft = {item->title, item->description, item->dueDate};
  } else if (!id.empty()) {
    m_notice = "Task no longer available. Refresh the collection.";
    requestUpdate();
    return;
  } else {
    m_draft = {};
  }
  m_title->setValue(m_draft.title);
  m_notes->setValue(m_draft.description);
  m_due->setValue(m_draft.dueDate);
  m_notice.clear();
  m_editing = true;
  m_draftChanged = false;
  m_copiedDraft = false;
  m_confirmingDelete = false;
  m_focusEditorPending = true;
  requestUpdate();
}

void TasksTab::saveEditor() {
  if (m_tasks == nullptr || !m_editing || m_confirmingDelete) {
    return;
  }
  m_notice.clear();
  // The service validates title/date and preserves timed DUE values when unchanged.
  const bool saved = m_selectedId.empty() ? m_tasks->addTask(m_draft) : m_tasks->updateTask(m_selectedId, m_draft);
  if (saved) {
    cancelEditor();
  }
  requestUpdate();
}

void TasksTab::copyToNewTask() {
  if (!m_editing || m_selectedId.empty() || m_confirmingDelete) {
    return;
  }
  // Copy the editable draft, including unsaved edits, without touching the source.
  m_selectedId.clear();
  m_copiedDraft = true;
  m_draftChanged = true;
  m_notice = "Unsaved copy: title, notes and due date only. Saving adds a new active task; the source is unchanged.";
  m_focusEditorPending = true;
  requestUpdate();
}

void TasksTab::confirmLocalAction() {
  if (m_tasks == nullptr)
    return;
  m_notice.clear();
  if (m_localAction == LocalConfirmation::Purge) {
    const auto& trash = m_tasks->snapshot().trash;
    const auto found = std::ranges::find(trash, m_confirmTrashId, &TrashedTask::id);
    if (found == trash.end() || found->pending || found->conflict) {
      m_notice = "Saved copy changed or deletion is unresolved. Review Trash before removing it.";
      m_localAction = LocalConfirmation::None;
    } else if (m_tasks->purgeTask(m_confirmTrashId)) {
      m_localAction = LocalConfirmation::None;
    }
  } else if (m_localAction == LocalConfirmation::ClearHistory) {
    const auto& history = m_tasks->snapshot().history;
    if (history.size() != m_confirmHistoryCount || (!history.empty() && history.front().id != m_confirmHistoryNewest)) {
      m_notice = "History changed. Review it and confirm clearing again.";
      m_localAction = LocalConfirmation::None;
    } else if (m_tasks->clearHistory()) {
      m_localAction = LocalConfirmation::None;
    }
  }
  requestUpdate();
}

// Day filters (Today, Overdue, Upcoming) go stale at midnight.
void TasksTab::scheduleDayRefresh() {
  const auto now = std::time(nullptr);
  std::tm midnight{};
  localtime_r(&now, &midnight);
  midnight.tm_mday += 1;
  midnight.tm_hour = midnight.tm_min = midnight.tm_sec = 0;
  midnight.tm_isdst = -1;
  const auto next = std::mktime(&midnight);
  m_dayTimer.start(std::chrono::seconds(std::max<std::time_t>(1, next - now)), [this]() {
    requestUpdate();
    scheduleDayRefresh();
  });
}

void TasksTab::cancelEditor() {
  m_editing = false;
  m_confirmingDelete = false;
  m_draftChanged = false;
  m_copiedDraft = false;
  m_focusEditorPending = false;
  m_selectedId.clear();
  m_draft = {};
  m_notice.clear();
  requestUpdate();
}

void TasksTab::submitPassword() {
  if (m_tasks == nullptr || m_password == nullptr || !m_tasks->snapshot().needsPassword) {
    return;
  }
  if (m_password->value().empty()) {
    m_notice = "Enter the collection password.";
  } else {
    m_notice.clear();
    m_tasks->savePassword(m_password->value());
    m_password->setValue("");
  }
  requestUpdate();
}

// ---- Row selection ------------------------------------------------------------------------------------------------

bool TasksTab::showsTasks() const { return m_view == View::Active || m_view == View::Completed; }

bool TasksTab::passesDateFilter(const std::string& due) const {
  switch (m_date) {
  case DateFilter::Today:
    return due == m_today;
  case DateFilter::Overdue:
    return !due.empty() && due < m_today;
  case DateFilter::Upcoming:
    return !due.empty() && due > m_today && due <= m_upcomingEnd;
  case DateFilter::None:
    return due.empty();
  case DateFilter::All:
    break;
  }
  return true;
}

// Tasks of the current view matching search and (Active only) date filter, in the chosen sort order. `total` counts
// the tasks in the view before search and date filters.
std::vector<std::size_t>
TasksTab::visibleTasks(const TaskSnapshot& snapshot, const std::string& query, std::size_t& total) const {
  std::vector<std::size_t> visible;
  visible.reserve(snapshot.items.size());
  total = 0;
  for (std::size_t index = 0; index < snapshot.items.size(); ++index) {
    const auto& item = snapshot.items[index];
    if (item.completed != (m_view == View::Completed))
      continue;
    ++total;
    if (!matchesSearch(item.title, query) && !matchesSearch(item.description, query))
      continue;
    if (m_view == View::Active && !passesDateFilter(item.dueDate))
      continue;
    visible.push_back(index);
  }
  std::ranges::sort(visible, [&](std::size_t left, std::size_t right) {
    const auto& a = snapshot.items[left];
    const auto& b = snapshot.items[right];
    if (m_sortByTitle) {
      if (a.title != b.title)
        return a.title < b.title;
    } else {
      if (a.dueDate.empty() != b.dueDate.empty())
        return !a.dueDate.empty();
      if (a.dueDate != b.dueDate)
        return a.dueDate < b.dueDate;
    }
    return a.id < b.id;
  });
  return visible;
}

// The indices of the rows the current view, search and filters show, in display order; `total` is how many the view
// holds before the search and date filters.
std::vector<std::size_t> TasksTab::visibleRows(const TaskSnapshot& snapshot, std::size_t& total) const {
  const auto query = StringUtils::toLower(m_query);
  if (showsTasks())
    return visibleTasks(snapshot, query, total);
  if (m_view == View::Trash) {
    total = snapshot.trash.size();
    return visibleTrash(snapshot, query);
  }
  total = snapshot.history.size();
  return visibleHistory(snapshot, query);
}

const std::string& TasksTab::rowId(const TaskSnapshot& snapshot, std::size_t index) const {
  if (showsTasks())
    return snapshot.items[index].id;
  return m_view == View::Trash ? snapshot.trash[index].id : snapshot.history[index].id;
}

// ---- Row synchronisation (runs on every snapshot change) ----------------------------------------------------------

// Widgets are rebuilt only when the visible row identities change; otherwise rows are updated in place so focus,
// hover and scroll position survive status-only snapshot changes.
void TasksTab::synchronizeRows(const TaskSnapshot& snapshot) {
  std::size_t total = 0;
  const std::vector<std::size_t> visible = visibleRows(snapshot, total);
  const bool sameRows = visible.size() == m_rows.size()
      && std::equal(visible.begin(), visible.end(), m_rows.begin(), [&](std::size_t index, const Row& row) {
                          return row.view == m_view && row.id == rowId(snapshot, index);
                        });
  std::optional<InputDispatcher::TabFocusSnapshot> focus;
  auto* dispatcher = inputDispatcher();
  if (!sameRows) {
    if (dispatcher != nullptr && isNodeInSubtree(dispatcher->focusedArea(), m_list)) {
      focus = dispatcher->captureTabFocus();
      dispatcher->setFocus(nullptr);
    }
    rebuildRows(snapshot, visible);
  }
  for (std::size_t index = 0; index < visible.size(); ++index) {
    auto& row = m_rows[index];
    if (showsTasks())
      updateTaskRow(row, snapshot.items[visible[index]], snapshot.writable);
    else if (m_view == View::Trash)
      updateTrashRow(row, snapshot.trash[visible[index]], snapshot.writable);
    else
      updateHistoryRow(row, snapshot.history[visible[index]]);
  }
  if (focus.has_value()) {
    dispatcher->restoreTabFocus(std::move(*focus));
    if (!isNodeInSubtree(dispatcher->focusedArea(), m_root))
      dispatcher->setFocus(m_search->inputArea());
  }
  if (m_resetScroll) {
    m_scroll->setScrollOffset(0.0F);
    m_resetScroll = false;
  }
  updateCounts(snapshot, visible.size(), total);
  show(m_empty, visible.empty());
  m_empty->setText(
      total != 0                      ? "No matches. Clear search or change the date filter."
          : m_view == View::Trash     ? "No saved deletions yet."
          : m_view == View::History   ? "No local activity yet."
          : m_view == View::Completed ? "No completed tasks."
          : snapshot.writable         ? "No active tasks. Add one with New."
                                      : "No cached active tasks. Connect and refresh."
  );
}

void TasksTab::rebuildRows(const TaskSnapshot& snapshot, const std::vector<std::size_t>& visible) {
  m_lastFocus = nullptr;
  while (!m_list->children().empty())
    m_list->removeChild(m_list->children().front().get());
  m_rows.clear();
  const float scale = contentScale();
  for (const auto index : visible) {
    Row row{.id = rowId(snapshot, index), .view = m_view};
    auto card = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .padding = Style::spaceXs * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceVariant, panelCardOpacity()),
        .radius = Style::radiusSm * scale,
    });
    if (showsTasks())
      card->addChild(buildTaskLine(row, snapshot.items[index].title, scale));
    else
      card->addChild(taskLabel("", scale, &row.title));
    card->addChild(taskLabel("", scale, &row.detail));
    card->addChild(taskLabel("", scale, &row.preview));
    if (showsTasks())
      row.preview->setMaxLines(PreviewMaxLines);
    if (m_view == View::Trash)
      card->addChild(buildTrashActions(row, scale));
    m_list->addChild(std::move(card));
    styleTaskButtons(*m_list->children().back());
    m_rows.push_back(std::move(row));
  }
}

// Completion toggle plus the title button that opens the editor.
std::unique_ptr<Flex> TasksTab::buildTaskLine(Row& row, const std::string& title, float scale) {
  auto line = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
  line->addChild(
      ui::button(compactButton(
          "[ ]", ButtonVariant::Secondary, scale,
          [this, id = row.id]() {
            if (m_tasks == nullptr)
              return;
            const auto& items = m_tasks->snapshot().items;
            const auto found = std::ranges::find(items, id, &TaskItem::id);
            if (found != items.end()) {
              m_notice.clear();
              m_tasks->setCompleted(id, !found->completed);
              requestUpdate();
            }
          },
          &row.complete
      ))
  );
  row.complete->inputArea()->setTabFocusKey("tasks.complete." + row.id);
  line->addChild(
      ui::button({
          .out = &row.open,
          .text = title,
          .fontSize = Style::fontSizeBody * scale,
          .contentAlign = ButtonContentAlign::Start,
          .variant = ButtonVariant::Ghost,
          .padding = Style::spaceXs * scale,
          .flexGrow = 1.0F,
          .onClick = [this, id = row.id]() { openEditor(id); },
          .configure = [](Button& button) { button.label()->setMaxLines(TitleMaxLines); },
      })
  );
  row.open->inputArea()->setTabFocusKey("tasks.open." + row.id);
  return line;
}

// Restore (`row.complete`) and permanent removal of the local copy (`row.open`).
std::unique_ptr<Flex> TasksTab::buildTrashActions(Row& row, float scale) {
  auto actions = ui::row({.align = FlexAlign::Stretch, .wrap = true, .gap = Style::spaceXs * scale});
  actions->addChild(
      ui::button(compactButton(
          "Restore copy", ButtonVariant::Secondary, scale,
          [this, id = row.id]() {
            if (m_tasks == nullptr)
              return;
            m_notice.clear();
            const auto& trash = m_tasks->snapshot().trash;
            const auto archived = std::ranges::find(trash, id, &TrashedTask::id);
            const auto rendered = std::ranges::find(m_rows, id, &Row::id);
            if (archived == trash.end() || rendered == m_rows.end() || archived->pending != rendered->undoDelete) {
              m_notice = "Deletion state changed. Review Trash before restoring.";
            } else {
              m_tasks->restoreTask(id);
            }
            requestUpdate();
          },
          &row.complete
      ))
  );
  row.complete->inputArea()->setTabFocusKey("tasks.restore." + row.id);
  actions->addChild(
      ui::button(compactButton(
          "Remove local copy…", ButtonVariant::Ghost, scale,
          [this, id = row.id]() {
            m_confirmTrashId = id;
            m_localAction = LocalConfirmation::Purge;
            m_focusLocalPending = true;
            requestUpdate();
          },
          &row.open
      ))
  );
  row.open->inputArea()->setTabFocusKey("tasks.purge." + row.id);
  return actions;
}

void TasksTab::updateTaskRow(Row& row, const TaskItem& item, bool writable) {
  row.complete->setText(item.completed ? "[x]" : "[ ]");
  row.complete->setSelected(item.completed);
  row.complete->setEnabled(writable);
  row.complete->setTooltip(item.completed ? "Mark active" : "Mark complete");
  row.open->setText(item.title);
  row.open->setTooltip(item.title);
  row.open->setSelected(m_editing && m_selectedId == item.id);
  row.detail->setText(taskDetail(item, m_today));
  row.detail->setColor(
      item.conflict || isOverdue(item, m_today) ? taskAccent() : colorSpecFromRole(ColorRole::OnSurfaceVariant)
  );
  row.preview->setText(notesPreview(item.description));
  show(row.preview, !item.description.empty());
}

void TasksTab::updateTrashRow(Row& row, const TrashedTask& archived, bool writable) {
  const auto& item = archived.task;
  row.title->setText(item.title);
  row.detail->setText(trashDetail(archived));
  row.preview->setText(item.description);
  show(row.preview, !item.description.empty());
  row.undoDelete = archived.pending;
  row.complete->setText(archived.pending ? "Undo delete" : "Restore copy");
  row.complete->setEnabled(writable && archived.restorable);
  row.complete->setTooltip(
      archived.pending ? "Cancel a safely unsent deletion"
                       : "Create a new synced task with the saved metadata and completion state"
  );
  row.open->setEnabled(writable && !archived.pending && !archived.conflict);
  row.open->setTooltip("Permanently remove only this desktop's saved copy; never delete a server task");
}

void TasksTab::updateHistoryRow(Row& row, const TaskHistoryEntry& entry) {
  row.title->setText(entry.action + " · " + entry.title);
  row.detail->setText(localTimestamp(entry.occurredAt) + " · " + entry.source);
  row.preview->setText(entry.details);
  show(row.preview, !entry.details.empty());
}

void TasksTab::updateCounts(const TaskSnapshot& snapshot, std::size_t shown, std::size_t total) {
  const auto completed = static_cast<std::size_t>(std::ranges::count_if(snapshot.items, [](const TaskItem& item) {
    return item.completed;
  }));
  const std::string viewName = m_view == View::Active ? "active tasks"
      : m_view == View::Completed                     ? "completed tasks"
      : m_view == View::Trash                         ? "local saved copies"
                                                      : "local history entries";
  m_counts->setText(
      "Active "
      + std::to_string(snapshot.items.size() - completed)
      + " · Completed "
      + std::to_string(completed)
      + " · Trash "
      + std::to_string(snapshot.trash.size())
      + " · History "
      + std::to_string(snapshot.history.size())
      + "\nShowing "
      + std::to_string(shown)
      + " of "
      + std::to_string(total)
      + " "
      + viewName
      + " (totals before filters)"
  );
}

// ---- Synchronisation of the rest of the tab -----------------------------------------------------------------------

void TasksTab::synchronize() {
  if (m_root == nullptr)
    return;
  static const TaskSnapshot unavailable{.status = "Tasks unavailable. Check the Tasks configuration."};
  const auto& snapshot = m_tasks != nullptr ? m_tasks->snapshot() : unavailable;
  if (!m_dirty && m_revision == snapshot.revision)
    return;
  // Never retire a pressed row, or hide an editor, inside an input callback.
  if (inputDispatcher() != nullptr && inputDispatcher()->pointerCaptured())
    return;
  m_dirty = false;
  m_revision = snapshot.revision;
  const auto now = std::time(nullptr);
  m_today = localDate(now);
  m_upcomingEnd = localDate(now, UpcomingDays);
  synchronizeToolbars(snapshot);
  std::string status = snapshot.syncing ? "Syncing · " : "";
  status += snapshot.status;
  if (!m_notice.empty())
    status += "\n" + m_notice;
  m_status->setText(status);

  moveFocusOutOfHiddenPanels(snapshot);
  show(m_passwordCard, snapshot.needsPassword);
  if (!snapshot.needsPassword && !m_password->value().empty())
    m_password->setValue("");
  synchronizeEditor(snapshot);
  synchronizeLocalConfirmation(snapshot);
  synchronizeRows(snapshot);
}

void TasksTab::synchronizeToolbars(const TaskSnapshot& snapshot) {
  m_refresh->setEnabled(m_tasks != nullptr);
  m_new->setEnabled(snapshot.writable);
  show(m_filterToolbar, m_view != View::Trash);
  show(m_dateFilter, m_view == View::Active);
  show(m_sort, showsTasks());
  show(m_clearHistory, m_view == View::History);
  m_clearHistory->setEnabled(snapshot.writable && !snapshot.history.empty());
  show(m_localScope, !showsTasks());
  const std::string viewNote = m_view == View::History
      ? " History keeps the latest "
          + std::to_string(TaskHistoryLimit)
          + " entries; Discovered means first seen here, not the original creation date."
      : " Saved deletions stay until you remove them. Restore copy creates a new task and keeps its saved metadata.";
  m_localScope->setText(
      "Trash and history start with this update and stay encrypted on this desktop for this account. "
      "Other CalDAV clients do not share them."
      + viewNote
  );
}

// Focus must not stay inside a panel this update is about to hide.
void TasksTab::moveFocusOutOfHiddenPanels(const TaskSnapshot& snapshot) {
  auto* dispatcher = inputDispatcher();
  if (dispatcher == nullptr)
    return;
  auto* focused = dispatcher->focusedArea();
  if ((!m_editing && isNodeInSubtree(focused, m_editor))
      || (!snapshot.needsPassword && isNodeInSubtree(focused, m_passwordCard))
      || (!m_confirmingDelete && isNodeInSubtree(focused, m_confirmation))
      || (m_localAction == LocalConfirmation::None && isNodeInSubtree(focused, m_localConfirmation))
      || (m_view != View::Active && isNodeInSubtree(focused, m_dateFilter))
      || (!showsTasks() && isNodeInSubtree(focused, m_sort))
      || (m_view != View::History && isNodeInSubtree(focused, m_clearHistory))) {
    dispatcher->setFocus(m_search->inputArea());
  }
}

void TasksTab::synchronizeEditor(const TaskSnapshot& snapshot) {
  show(m_editor, m_editing);
  const auto* selected = selectedItem();
  const bool conflict = selected != nullptr && selected->conflict;
  m_editorTitle->setText(
      m_selectedId.empty() ? (m_copiedDraft ? "Unsaved copy · new active task" : "New task")
                           : (conflict ? "Conflict · edit to resolve" : "Edit task")
  );
  m_save->setText(m_selectedId.empty() ? "Add task" : "Save changes");
  m_save->setEnabled(snapshot.writable && !m_confirmingDelete && (m_selectedId.empty() || selected != nullptr));
  show(m_copy, !m_selectedId.empty() && !m_confirmingDelete);
  show(m_delete, !m_selectedId.empty() && !m_confirmingDelete);
  m_delete->setEnabled(snapshot.writable && selected != nullptr);
  m_delete->setText(conflict ? "Discard local changes…" : "Delete task…");
  show(m_confirmation, m_confirmingDelete);
  m_confirmDelete->setEnabled(snapshot.writable && selected != nullptr);
  m_confirmDelete->setText(m_confirmingConflict ? "Discard local changes" : "Confirm delete");
  if (m_confirmingDelete) {
    m_confirmationTitle->setText(
        (m_confirmingConflict ? "Discard only local changes to “" : "Delete “")
        + (selected != nullptr ? selected->title : m_draft.title)
        + (m_confirmingConflict ? "”? The server task is kept." : "” from the synced collection?")
    );
  }
}

void TasksTab::synchronizeLocalConfirmation(const TaskSnapshot& snapshot) {
  show(m_localConfirmation, m_localAction != LocalConfirmation::None);
  if (m_localAction == LocalConfirmation::Purge) {
    const auto found = std::ranges::find(snapshot.trash, m_confirmTrashId, &TrashedTask::id);
    const bool available = found != snapshot.trash.end();
    m_confirmLocal->setEnabled(snapshot.writable && available && !found->pending && !found->conflict);
    m_confirmLocal->setText("Permanently remove local copy");
    m_localConfirmationTitle->setText(
        available ? "Permanently remove this desktop's saved copy of “"
                + found->task.title
                + "”, deleted "
                + localTimestamp(found->deletedAt)
                + " ("
                + found->source
                + ")? This cannot be undone. "
                  "No server task is deleted; other saved copies and history are kept."
                  : "This saved copy is no longer in Trash."
    );
  } else if (m_localAction == LocalConfirmation::ClearHistory) {
    m_confirmLocal->setEnabled(snapshot.writable && !snapshot.history.empty());
    m_confirmLocal->setText("Clear all local history");
    m_localConfirmationTitle->setText(
        "Permanently clear all "
        + std::to_string(m_confirmHistoryCount)
        + " history entries for this account on this desktop, including entries hidden by search? "
          "This cannot be undone. Tasks, Trash and the CalDAV server are unchanged."
    );
  }
}

void TasksTab::focusEditor() {
  if (m_focusEditorPending
      && m_editor->visible()
      && inputDispatcher() != nullptr
      && !inputDispatcher()->pointerCaptured()) {
    m_focusEditorPending = false;
    inputDispatcher()->setFocus(m_title->inputArea());
  }
  if (m_focusLocalPending && inputDispatcher() != nullptr && !inputDispatcher()->pointerCaptured()) {
    m_focusLocalPending = false;
    if (m_localAction != LocalConfirmation::None && m_localConfirmation->visible()) {
      inputDispatcher()->setFocus(m_confirmLocal->inputArea());
    }
  }
  if (inputDispatcher() == nullptr || m_scroll == nullptr) {
    return;
  }
  auto* focused = inputDispatcher()->focusedArea();
  if (focused != m_lastFocus) {
    m_lastFocus = focused;
    if (focused != nullptr && isNodeInSubtree(focused, m_scroll->content())) {
      scrollNodeIntoScrollView(*m_scroll, nullptr, *focused, Style::spaceSm * contentScale());
    }
  }
}

// ---- Tab hooks ----------------------------------------------------------------------------------------------------

void TasksTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root != nullptr) {
    synchronize();
    const bool narrow = contentWidth < NarrowContentWidthPx * contentScale();
    m_browseToolbar->setDirection(narrow ? FlexDirection::Vertical : FlexDirection::Horizontal);
    m_search->setFlexGrow(narrow ? 0.0F : 1.0F);
    m_filterToolbar->setDirection(narrow ? FlexDirection::Vertical : FlexDirection::Horizontal);
    m_dateFilter->setFlexGrow(narrow ? 0.0F : 1.0F);
    m_root->setSize(contentWidth, bodyHeight);
    m_root->layout(renderer);
    focusEditor();
  }
}

void TasksTab::doUpdate(Renderer& renderer) {
  if (m_root != nullptr) {
    const bool changed = m_dirty || (m_tasks != nullptr && m_tasks->snapshot().revision != m_revision);
    synchronize();
    if (changed) {
      m_root->layout(renderer);
    }
    focusEditor();
  }
}

void TasksTab::setActive(bool active) {
  if (m_active == active)
    return;
  m_active = active;
  if (active) {
    m_dirty = true;
    scheduleDayRefresh();
    if (m_tasks != nullptr)
      m_tasks->requestRefresh();
  } else {
    m_dayTimer.stop();
  }
}

void TasksTab::onPanelCardOpacityChanged(float opacity) {
  if (m_root != nullptr) {
    applyTaskCardStyle(*m_root, contentScale(), opacity);
    for (Input* input : {m_title, m_notes, m_due, m_password, m_search}) {
      input->setSurfaceOpacity(opacity);
    }
    for (Select* select : {m_viewPicker, m_dateFilter, m_sort}) {
      select->setSurfaceOpacity(opacity);
    }
    for (const auto& child : m_list->children()) {
      if (auto* card = dynamic_cast<Flex*>(child.get()); card != nullptr) {
        card->setFill(colorSpecFromRole(ColorRole::SurfaceVariant, opacity));
      }
    }
  }
}

void TasksTab::onClose() {
  m_dayTimer.stop();
  m_root = nullptr;
  m_scroll = nullptr;
  m_list = nullptr;
  m_editor = nullptr;
  m_passwordCard = nullptr;
  m_confirmation = nullptr;
  m_browseToolbar = nullptr;
  m_filterToolbar = nullptr;
  m_localConfirmation = nullptr;
  m_localConfirmationTitle = nullptr;
  m_counts = nullptr;
  m_localScope = nullptr;
  m_status = nullptr;
  m_editorTitle = nullptr;
  m_confirmationTitle = nullptr;
  m_empty = nullptr;
  m_refresh = nullptr;
  m_new = nullptr;
  m_viewPicker = nullptr;
  m_dateFilter = nullptr;
  m_sort = nullptr;
  m_clearHistory = nullptr;
  m_confirmLocal = nullptr;
  m_copy = nullptr;
  m_search = nullptr;
  m_save = nullptr;
  m_delete = nullptr;
  m_confirmDelete = nullptr;
  m_title = nullptr;
  m_notes = nullptr;
  m_due = nullptr;
  m_password = nullptr;
  m_lastFocus = nullptr;
  m_rows.clear();
  m_confirmingDelete = false;
  m_localAction = LocalConfirmation::None;
  m_resetScroll = false;
  m_focusEditorPending = false;
  m_focusLocalPending = false;
  m_dirty = true;
  m_active = false;
  // Keep the unfinished draft across close/reopen; passwords live only in Input.
}
