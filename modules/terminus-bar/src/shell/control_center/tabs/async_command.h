#pragma once

// Shared by the network, bluetooth and sound quick panels: run an external command on a worker and deliver its
// result on the main loop, but only while the tab that asked is still alive.

#include "core/deferred_call.h"
#include "core/process/process.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asynccmd {

  // Runs `args` (with `LC_ALL=C`) on a worker and calls `done` on the main loop. Nothing is called once `alive` has
  // expired (the tab was destroyed). A command that cannot start still reports, as a result with exit code -1.
  inline void run(
      std::vector<std::string> args, std::chrono::milliseconds timeout, std::weak_ptr<bool> alive,
      std::function<void(process::RunResult)> done
  ) {
    const auto deliver = [alive, done](process::RunResult result) {
      DeferredCall::callLater([alive, done, result = std::move(result)]() mutable {
        const auto token = alive.lock();
        if (!token || !*token) {
          return;
        }
        done(std::move(result));
      });
    };
    const bool started = process::runAsync(
        args, process::RunCallbacks{.onExit = [deliver](process::RunResult result) { deliver(std::move(result)); }},
        process::RunOptions{
            .timeout = timeout, .env = {process::EnvOverride{"LC_ALL", std::string("C")}}}
    );
    if (!started) {
      process::RunResult failed;
      failed.exitCode = -1;
      failed.err = "could not start " + (args.empty() ? std::string("command") : args.front());
      deliver(std::move(failed));
    }
  }

  // First non-blank line of `text`, trimmed of spaces, tabs and carriage returns; empty when there is none.
  [[nodiscard]] inline std::string firstLine(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size()) {
      std::size_t end = text.find('\n', begin);
      if (end == std::string_view::npos) {
        end = text.size();
      }
      std::string_view line = text.substr(begin, end - begin);
      while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r')) {
        line.remove_prefix(1);
      }
      while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.remove_suffix(1);
      }
      if (!line.empty()) {
        return std::string(line);
      }
      begin = end + 1;
    }
    return {};
  }

} // namespace asynccmd
