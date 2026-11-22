#pragma once

#include "app/poll_source.h"
#include "tasks/task_service.h"

class TaskPollSource final : public PollSource {
public:
  explicit TaskPollSource(TaskService& tasks) : m_tasks(tasks) {}
  [[nodiscard]] int pollTimeoutMs() const override { return m_tasks.pollTimeoutMs(); }
  void dispatch(const std::vector<pollfd>&, std::size_t) override { m_tasks.tick(); }

protected:
  void doAddPollFds(std::vector<pollfd>&) override {}

private:
  TaskService& m_tasks;
};
