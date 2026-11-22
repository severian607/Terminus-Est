#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class ConfigService;
class HttpClient;
namespace security {
  class SecretStore;
  class StorageKeyProvider;
} // namespace security

struct TaskItem {
  std::string id, title, description, dueDate;
  bool completed = false, pending = false, conflict = false;
};
struct TaskDraft {
  std::string title, description, dueDate;
};
struct TrashedTask {
  std::string id;
  TaskItem task;
  std::int64_t deletedAt = 0;
  std::string source;
  bool pending = false, conflict = false, restorable = false;
};
struct TaskHistoryEntry {
  std::string id, taskId, title, action, source, details;
  std::int64_t occurredAt = 0;
};
struct TaskSnapshot {
  std::vector<TaskItem> items;
  std::vector<TrashedTask> trash;
  std::vector<TaskHistoryEntry> history;
  std::string status;
  bool syncing = false, needsPassword = false, writable = false;
  std::uint64_t revision = 0;
};

class TaskService {
public:
  TaskService(ConfigService&, HttpClient&, security::SecretStore&, security::StorageKeyProvider&);
  ~TaskService();
  TaskService(const TaskService&) = delete;
  TaskService& operator=(const TaskService&) = delete;
  void initialize();
  void tick();
  [[nodiscard]] int pollTimeoutMs() const;
  [[nodiscard]] const TaskSnapshot& snapshot() const;
  void setChangeCallback(std::function<void()> callback);
  void requestRefresh();
  bool addTask(const TaskDraft&);
  bool updateTask(const std::string& id, const TaskDraft&);
  bool setCompleted(const std::string& id, bool);
  // On conflicts, discards only the local draft; never deletes the remote task.
  bool removeTask(const std::string& id);
  bool restoreTask(const std::string& trashId);
  bool purgeTask(const std::string& trashId);
  bool clearHistory();
  void savePassword(std::string);
  void syncPersistence();
  [[nodiscard]] bool hasEncryptedPersistence() const;
  bool clearEncryptedPersistenceForRecovery();

private:
  class Impl;
  std::unique_ptr<Impl> m;
};
