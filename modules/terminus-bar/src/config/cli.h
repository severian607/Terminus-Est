#pragma once

namespace terminusbar::config {

  // Entry point for `terminusbar config <command> [options]`. Returns a process
  // exit code. Pure CLI helper; does not start Application or mutate live config.
  int runCli(int argc, char* argv[]);

} // namespace terminusbar::config
