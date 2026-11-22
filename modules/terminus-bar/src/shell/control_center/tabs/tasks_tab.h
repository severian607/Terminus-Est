#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/tab.h"
#include "tasks/task_service.h"

#include <cstdint>
#include <string>
#include <vector>

class Button;
class Input;
class InputArea;
class ScrollView;
class Select;

class TasksTab final : public Tab {
public:
  explicit TasksTab(TaskService* tasks);
  std::unique_ptr<Flex> create() override;
  void setActive(bool active) override;
  void onClose() override;

private:
  enum class View { Active, Completed, Trash, History };
  enum class DateFilter { All, Today, Overdue, Upcoming, None };
  enum class LocalConfirmation { None, Purge, ClearHistory };
  struct Row {
    std::string id;
    View view = View::Active;
    bool undoDelete = false;
    Label* title = nullptr;
    Label* preview = nullptr;
    Button* complete = nullptr;
    Button* open = nullptr;
    Label* detail = nullptr;
  };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void synchronize();
  void synchronizeRows(const TaskSnapshot& snapshot);
  [[nodiscard]] std::vector<std::size_t> visibleRows(const TaskSnapshot& snapshot, std::size_t& total) const;
  void openEditor(const std::string& id);
  void saveEditor();
  void copyToNewTask();
  void confirmLocalAction();
  void scheduleDayRefresh();
  void cancelEditor();
  void submitPassword();
  void requestUpdate();
  void focusEditor();
  [[nodiscard]] const TaskItem* selectedItem() const;

  TaskService* m_tasks = nullptr;
  Flex* m_root = nullptr;
  ScrollView* m_scroll = nullptr;
  Flex* m_list = nullptr;
  Flex* m_editor = nullptr;
  Flex* m_passwordCard = nullptr;
  Flex* m_confirmation = nullptr;
  Flex* m_browseToolbar = nullptr;
  Flex* m_filterToolbar = nullptr;
  Flex* m_localConfirmation = nullptr;
  Label* m_localConfirmationTitle = nullptr;
  Label* m_counts = nullptr;
  Label* m_localScope = nullptr;
  Label* m_status = nullptr;
  Label* m_editorTitle = nullptr;
  Label* m_confirmationTitle = nullptr;
  Label* m_empty = nullptr;
  Button* m_refresh = nullptr;
  Button* m_new = nullptr;
  Select* m_viewPicker = nullptr;
  Select* m_dateFilter = nullptr;
  Select* m_sort = nullptr;
  Button* m_clearHistory = nullptr;
  Button* m_confirmLocal = nullptr;
  Button* m_copy = nullptr;
  Input* m_search = nullptr;
  Button* m_save = nullptr;
  Button* m_delete = nullptr;
  Button* m_confirmDelete = nullptr;
  Input* m_title = nullptr;
  Input* m_notes = nullptr;
  Input* m_due = nullptr;
  Input* m_password = nullptr;
  InputArea* m_lastFocus = nullptr;
  std::vector<Row> m_rows;
  TaskDraft m_draft;
  std::string m_selectedId;
  std::string m_notice;
  std::uint64_t m_revision = 0;
  Timer m_dayTimer;
  std::string m_query;
  std::string m_today;
  std::string m_upcomingEnd;
  std::string m_confirmTrashId;
  std::string m_confirmHistoryNewest;
  std::size_t m_confirmHistoryCount = 0;
  View m_view = View::Active;
  DateFilter m_date = DateFilter::All;
  LocalConfirmation m_localAction = LocalConfirmation::None;
  bool m_sortByTitle = false;
  bool m_copiedDraft = false;
  bool m_dirty = true;
  bool m_active = false;
  bool m_resetScroll = false;
  bool m_editing = false;
  bool m_draftChanged = false;
  bool m_confirmingDelete = false;
  bool m_confirmingConflict = false;
  bool m_focusEditorPending = false;
  bool m_focusLocalPending = false;
};
