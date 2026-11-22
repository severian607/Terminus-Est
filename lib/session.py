"""Native idle/saver generation and staged, explicitly applied login appearance."""

import json
import subprocess
from pathlib import Path

import tokens

# The settings schema declares the same limits; adapters check again as the last gate before files are written.
SCREENSAVER_STYLES = ("per-monitor", "wordmark", "arcade")
SCREENSAVER_INTEGER_RANGES = (("idle_seconds", 30, 7200), ("fps", 15, 144))
LOGIN_ANIMATIONS = ("none", "doom", "matrix", "colormix", "gameoflife", "dur_file", "terminus")
LOGIN_PASSWORD_STYLES = ("asterisk", "constellation")
LOGIN_INTEGER_RANGES = (("animation_frame_delay", 10, 1000), ("animation_timeout_sec", 0, 3600))
LOGIN_COLOR_KEYS = ("background", "foreground", "accent")
LOGIN_BOOLEAN_KEYS = ("show_borders", "show_clock")
LOGIN_FIELDS = {
    "animation",
    "password_style",
    *(key for key, _, _ in LOGIN_INTEGER_RANGES),
    *LOGIN_COLOR_KEYS,
    *LOGIN_BOOLEAN_KEYS,
}
HYPRIDLE_RESTART_TIMEOUT_SECONDS = 30
HYPRIDLE_ERROR_CHARS = 1000

HYPRIDLE_HEAD = (
    "# Terminus-Est idle: non-authenticating screensaver, not a lock.\n"
    "general {\n    after_sleep_cmd = sleep 1 && terminus system wake\n}\n"
)


# Rendering


def _check_screensaver(saver: dict) -> None:
    if type(saver["enabled"]) is not bool:
        raise ValueError("screensaver.enabled must be boolean")
    for key, low, high in SCREENSAVER_INTEGER_RANGES:
        if type(saver[key]) is not int or not low <= saver[key] <= high:
            raise ValueError(f"screensaver.{key} must be an integer in {low}..{high}")
    if saver["style"] not in SCREENSAVER_STYLES:
        raise ValueError("Invalid screensaver style")


def _check_login(login: dict) -> None:
    if set(login) != LOGIN_FIELDS:
        raise ValueError("Unexpected login appearance fields")
    if login["animation"] not in LOGIN_ANIMATIONS:
        raise ValueError("Invalid login animation")
    if login["password_style"] not in LOGIN_PASSWORD_STYLES:
        raise ValueError("Invalid login password style")
    for key, low, high in LOGIN_INTEGER_RANGES:
        if type(login[key]) is not int or not low <= login[key] <= high:
            raise ValueError(f"Invalid login.{key}")
    for key in LOGIN_COLOR_KEYS:
        if not tokens.is_color(login[key]):
            raise ValueError(f"Invalid login.{key} color")
    for key in LOGIN_BOOLEAN_KEYS:
        if type(login[key]) is not bool:
            raise ValueError(f"login.{key} must be boolean")


def host_idle(root: Path) -> str | None:
    """This host's own idle configuration (config/hosts/<hostname>/hypridle.conf), if it has one."""
    try:
        host = Path("/etc/hostname").read_text().strip()
    except OSError:
        return None
    if not host or "/" in host or host.startswith("."):
        return None
    try:
        return (root / "hosts" / host / "hypridle.conf").read_text()
    except OSError:
        return None


def render(settings: dict, root: Path) -> dict[Path, str]:
    saver = settings["screensaver"]
    login = settings["login"]
    _check_screensaver(saver)
    _check_login(login)
    # Availability is checked by terminus-screensaver itself; keep the existing idle
    # listener alive while disabled so availability never changes pause state.
    idle = host_idle(root) or (
        HYPRIDLE_HEAD + "\nlistener {\n"
        f"    timeout = {saver['idle_seconds']}\n"
        "    on-timeout = terminus-screensaver\n"
        "    on-resume = terminus-screensaver --resume\n}\n"
    )
    environment = (
        "# Generated validated literals; consumed as data, never sourced.\n"
        f"TERMINUS_SCREENSAVER_ENABLED={'true' if saver['enabled'] else 'false'}\n"
        f"TERMINUS_SCREENSAVER_IDLE_SECONDS={saver['idle_seconds']}\n"
        f"TERMINUS_SCREENSAVER_STYLE={saver['style']}\n"
        f"TERMINUS_SCREENSAVER_FPS={saver['fps']}\n"
    )
    return {
        root / "generated/hypridle.conf": idle,
        root / "generated/screensaver.env": environment,
        root / "login/appearance.json": json.dumps(login, indent=2, sort_keys=True) + "\n",
    }


# Runtime application


def apply(settings: dict, root: Path, changed: set[Path]) -> list[str]:
    notices = []
    if root / "login/appearance.json" in changed:
        notices.append(
            "Login appearance staged only. Use Apply Login to authenticate; "
            "effective at the next Ly start, without restarting this session."
        )
    if root / "generated/screensaver.env" in changed:
        notices.append(
            "Terminus-Screensaver rendering changes take effect on its next launch; it does not authenticate or lock."
        )
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
        completed = subprocess.run(
            ["systemctl", "--user", "restart", "hypridle.service"],
            capture_output=True,
            text=True,
            timeout=HYPRIDLE_RESTART_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeError(f"Settings saved, but hypridle.service could not be restarted: {error}") from error
    if completed.returncode:
        raise RuntimeError(
            "Settings saved, but hypridle.service could not be restarted: "
            + completed.stderr.strip()[:HYPRIDLE_ERROR_CHARS]
        )
    notices.append("hypridle.service restarted with the new idle configuration.")
    return notices
