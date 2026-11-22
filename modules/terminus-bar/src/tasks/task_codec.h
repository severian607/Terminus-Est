#pragma once
#include "tasks/task_service.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace tasks {
  struct Resource {
    std::string etag, data;
  };
  using Resources = std::map<std::string, Resource>;
  // A quoted, single-line ETag; weak validators (W/"...") are refused.
  bool strongEtag(const std::string& etag);
  std::optional<std::string> collectionUrl(std::string_view url);
  std::optional<std::string> resourceUrl(const std::string& collection, std::string_view href);
  bool parseReport(const std::string& body, const std::string& collection, Resources& out);
  std::optional<TaskItem> parseTask(const std::string& data);
  bool validDraft(const TaskDraft& draft);
  std::string newTask(const TaskDraft& draft, const std::string& uid);
  // Preserve the complete calendar, replacing the identity of all recurrence components.
  std::string restoreTask(const std::string& source, const std::string& uid);
  // Compare drafts with the displayed local version so conflict resolution edits only fields requested by the user.
  std::string editTask(
      const std::string& source, const TaskItem& displayed, const TaskDraft* draft,
      std::optional<bool> completed = std::nullopt
  );
  bool equivalent(const std::string& a, const std::string& b);
  std::string randomId();
  std::string accountId(const std::string& url, const std::string& username);
} // namespace tasks
