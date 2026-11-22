#!/usr/bin/env python3
"""Print the pointer position in global layout coordinates as "x y" lines, only when it changes.

Asks the compositor named by the environment (XDG_RUNTIME_DIR, HYPRLAND_INSTANCE_SIGNATURE) over the Hyprland
command socket. Exits when the parent process ends or stdout closes.
"""

import json
import os
import socket
import sys
import time

SOCKET_PATH = f"{os.environ['XDG_RUNTIME_DIR']}/hypr/{os.environ['HYPRLAND_INSTANCE_SIGNATURE']}/.socket.sock"
MOVING_POLL_SECONDS = 0.0125  # 80 Hz while the pointer moves
RESTING_POLL_SECONDS = 0.125  # 8 Hz once it has rested
REST_AFTER_SECONDS = 0.5


def cursor_position() -> tuple[int, int]:
    """One `cursorpos` request; Hyprland closes the socket after each reply."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as hyprland:
        hyprland.connect(SOCKET_PATH)
        hyprland.sendall(b"j/cursorpos")
        reply = b"".join(iter(lambda: hyprland.recv(256), b""))
    position = json.loads(reply)
    return position["x"], position["y"]


def main() -> None:
    parent = os.getppid()
    last = None
    moved_at = 0.0
    while os.getppid() == parent:
        try:
            position = cursor_position()
            if position != last:
                last = position
                moved_at = time.monotonic()
                sys.stdout.write(f"{position[0]:d} {position[1]:d}\n")
                sys.stdout.flush()
        except BrokenPipeError:
            break
        except (OSError, ValueError, KeyError):
            pass  # the compositor is restarting or busy; ask again on the next poll
        resting = time.monotonic() - moved_at >= REST_AFTER_SECONDS
        time.sleep(RESTING_POLL_SECONDS if resting else MOVING_POLL_SECONDS)


if __name__ == "__main__":
    main()
