#!/usr/bin/env python3
# Prints the pointer position in global layout coordinates as "x y" lines, only when it changes.
# Asks the compositor named by the environment (XDG_RUNTIME_DIR, HYPRLAND_INSTANCE_SIGNATURE) over
# the Hyprland command socket: about 80 Hz while the pointer moves, 8 Hz after half a second of rest.
# Exits when the parent process ends or stdout closes.

import json
import os
import socket
import sys
import time

path = "%s/hypr/%s/.socket.sock" % (os.environ["XDG_RUNTIME_DIR"], os.environ["HYPRLAND_INSTANCE_SIGNATURE"])
parent = os.getppid()
last = None
moved_at = 0.0


def query():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(path)
        s.sendall(b"j/cursorpos")
        buf = b""
        while True:
            chunk = s.recv(256)
            if not chunk:
                break
            buf += chunk
        return json.loads(buf)
    finally:
        s.close()


while True:
    if os.getppid() != parent:
        break
    try:
        p = query()
        cur = (p["x"], p["y"])
        if cur != last:
            last = cur
            moved_at = time.monotonic()
            sys.stdout.write("%d %d\n" % cur)
            sys.stdout.flush()
    except BrokenPipeError:
        break
    except (OSError, ValueError, KeyError):
        pass
    time.sleep(0.0125 if time.monotonic() - moved_at < 0.5 else 0.125)
