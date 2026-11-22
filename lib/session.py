"""Native idle/saver generation and staged, explicitly applied login appearance."""

import json
from pathlib import Path
import subprocess

import tokens


# Rendering

def render(settings: dict, root: Path) -> dict[Path, str]:
    saver = settings["screensaver"]
    if type(saver["enabled"]) is not bool:
        raise ValueError("screensaver.enabled must be boolean")
    for key, low, high in (("idle_seconds", 30, 7200), ("fps", 15, 144)):
        if type(saver[key]) is not int or not low <= saver[key] <= high:
            raise ValueError(f"screensaver.{key} must be an integer in {low}..{high}")
    if saver["style"] not in ("per-monitor", "wordmark", "arcade"):
        raise ValueError("Invalid screensaver style")
    login = settings["login"]
    if set(login) != {"animation", "password_style", "animation_frame_delay", "animation_timeout_sec",
                      "background", "foreground", "accent", "show_borders", "show_clock"}:
        raise ValueError("Unexpected login appearance fields")
    if login["animation"] not in ("none", "doom", "matrix", "colormix", "gameoflife", "dur_file", "terminus"):
        raise ValueError("Invalid login animation")
    if login["password_style"] not in ("asterisk", "constellation"):
        raise ValueError("Invalid login password style")
    for key, low, high in (("animation_frame_delay", 10, 1000), ("animation_timeout_sec", 0, 3600)):
        if type(login[key]) is not int or not low <= login[key] <= high:
            raise ValueError(f"Invalid login.{key}")
    for key in ("background", "foreground", "accent"):
        if not tokens.is_color(login[key]):
            raise ValueError(f"Invalid login.{key} color")
    for key in ("show_borders", "show_clock"):
        if type(login[key]) is not bool:
            raise ValueError(f"login.{key} must be boolean")
    idle = "# Terminus-Est idle: non-authenticating screensaver, not a lock.\ngeneral {\n    after_sleep_cmd = sleep 1 && terminus system wake\n}\n"
    # Availability is checked by terminus-screensaver itself; keep the existing idle
    # listener alive while disabled so availability never changes pause state.
    idle += ("\nlistener {\n"
             f"    timeout = {saver['idle_seconds']}\n"
             "    on-timeout = terminus-screensaver\n"
             "    on-resume = terminus-screensaver --resume\n}\n")
    environment = ("# Generated validated literals; consumed as data, never sourced.\n"
                   f"TERMINUS_SCREENSAVER_ENABLED={'true' if saver['enabled'] else 'false'}\n"
                   f"TERMINUS_SCREENSAVER_IDLE_SECONDS={saver['idle_seconds']}\n"
                   f"TERMINUS_SCREENSAVER_STYLE={saver['style']}\n"
                   f"TERMINUS_SCREENSAVER_FPS={saver['fps']}\n")
    return {
        root / "generated/hypridle.conf": idle,
        root / "generated/screensaver.env": environment,
        root / "login/appearance.json": json.dumps(login, indent=2, sort_keys=True) + "\n",
    }


# Runtime application

def apply(settings: dict, root: Path, changed: set[Path]) -> list[str]:
    notices = []
    if root / "login/appearance.json" in changed:
        notices.append("Login appearance staged only. Use Apply Login to authenticate; effective at the next Ly start, without restarting this session.")
    if root / "generated/screensaver.env" in changed:
        notices.append("Terminus-Screensaver rendering changes take effect on its next launch; it does not authenticate or lock.")
    if root / "generated/hypridle.conf" not in changed:
        return notices
    if not tokens.is_live(root):
        notices.append("Idle configuration rendered for an isolated root; running desktop unchanged.")
        return notices
    # hypridle.service is the single owner of the idle daemon; inactive means paused.
    try:
        if not tokens.unit_active("hypridle.service"):
            notices.append("Idle is paused or not running; saved configuration will be used when idle is resumed.")
            return notices
        completed = subprocess.run(["systemctl", "--user", "restart", "hypridle.service"],
                                   capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeError(f"Settings saved, but hypridle.service could not be restarted: {error}")
    if completed.returncode:
        raise RuntimeError("Settings saved, but hypridle.service could not be restarted: " + completed.stderr.strip()[:1000])
    notices.append("hypridle.service restarted with the new idle configuration.")
    return notices
