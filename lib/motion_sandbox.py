"""Helpers shared by the preview recorders (terminus-motion-previews, terminus-screenfx-previews).

Both drive tools/terminus-motion-sandbox: a nested Hyprland on a hidden headless output, so nothing is loaded
into the real session.
"""

import json
import re
import subprocess
import time
from pathlib import Path

project = Path(__file__).resolve().parents[1]
TOOLS = project / "tools"


class Sandbox:
    """A running sandbox named `name` (a-z0-9, 1-8 chars): the real palette and a wallpaper, stopped on exit.

    `extra` is Lua appended to the sandbox config, loaded after the frame look.
    """

    def __init__(self, name, extra=""):
        self.name = name
        self.extra = extra

    def __enter__(self):
        try:
            self.start()
        except BaseException:  # __exit__ never runs when __enter__ raises
            self.stop()
            raise
        return self

    def __exit__(self, *_exc):
        self.stop()

    def tool(self, verb, *args, check=True, **kwargs):
        return subprocess.run([TOOLS / "terminus-motion-sandbox", verb, self.name, *map(str, args)], check=check, **kwargs)

    def spawn(self, *command):
        """A client inside the sandbox, in the background."""
        return subprocess.Popen([TOOLS / "terminus-motion-sandbox", "run", self.name, *map(str, command)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def start(self):
        self.tool("start")
        Path(f"/tmp/tms-{self.name}/extra.lua").write_text(self.look() + self.extra)
        self.tool("ctl", "reload", stdout=subprocess.DEVNULL)
        image = wallpaper()
        if image:
            self.spawn("swaybg", "-i", image, "-m", "fill")
            time.sleep(0.8)

    def stop(self):
        self.tool("stop", check=False)

    def alive(self):
        return not self.tool("ctl", "version", check=False, capture_output=True).returncode

    def restart(self):
        """A crashed plugin takes the sandbox compositor with it: start a clean one."""
        self.stop()
        self.start()

    def dispatch(self, expression):
        self.tool("ctl", "dispatch", expression, stdout=subprocess.DEVNULL)

    def eval(self, lua):
        self.tool("ctl", "eval", lua, stdout=subprocess.DEVNULL)

    def clients(self):
        return json.loads(self.tool("ctl", "clients", "-j", capture_output=True, text=True).stdout)

    def wait_for_clients(self, count, timeout=6.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if len(self.clients()) == count:
                return
            time.sleep(0.1)
        raise RuntimeError(f"expected {count} sandbox windows, found {len(self.clients())}")

    def look(self):
        """Sandbox-only Lua: the real palette and a scaled-down version of the real window frame."""
        colours = dict(re.findall(r'(\w+) = "#([0-9a-fA-F]{6})"', (project / "config/generated/appearance.lua").read_text()))
        active, inactive = colours.get("active_border", "ffcccd"), colours.get("inactive_border", "262626")
        return f"""hl.config({{
  general = {{ gaps_in = 4, gaps_out = 10, border_size = 1,
              col = {{ active_border = "rgb({active})", inactive_border = "rgb({inactive})" }} }},
  decoration = {{ rounding = 10, shadow = {{ enabled = true, range = 12, render_power = 2, offset = "0 4",
                                           color = "rgba(00000080)" }} }},
}})
"""

    def recording(self, raw, seconds, verb="record"):
        return Recording(self, raw, seconds, verb)


def wallpaper():
    try:
        outputs = json.loads(subprocess.run(["skwd-helm", "current", "--json"], capture_output=True, text=True,
                                            timeout=5).stdout)["outputs"]
        path = next(o.get("current") or o.get("path") for o in outputs if o.get("connected"))
        return path if path and Path(path).is_file() else None
    except (OSError, ValueError, KeyError, StopIteration, subprocess.SubprocessError):
        return None


class Recording:
    """Films the sandbox for `seconds`; the demo runs inside the `with` block.

    verb `record` films from inside the sandbox; `film` films its window from the real session, which is the
    only way to include the sandbox's screen shader (the compositor hands its own clients an unshaded copy).
    """

    def __init__(self, sandbox, raw, seconds, verb):
        self.sandbox, self.raw, self.seconds, self.verb = sandbox, raw, seconds, verb

    def __enter__(self):
        self.proc = subprocess.Popen([TOOLS / "terminus-motion-sandbox", self.verb, self.sandbox.name, self.raw,
                                      str(self.seconds)])
        time.sleep(0.35)  # wf-recorder needs a moment before its first frame
        return self

    def __exit__(self, *_exc):
        self.proc.wait()
