#pragma once

namespace terminusbar::ipc {

  // Entry point for `terminusbar msg <command> [args...]`. Returns a process exit
  // code. Forwards the command to the running instance over the IPC socket.
  int runCli(int argc, char* argv[]);

} // namespace terminusbar::ipc
