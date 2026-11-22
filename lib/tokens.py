"""Design tokens shared by every generated skin.

settings.toml is the only palette authority: the neutrals are fixed there and
the accent follows the wallpaper (tools/terminus-skwd-palette sets it). Skin
adapters (lib/skin_*.py) turn these tokens into native application files, so
no skin hard-codes a colour, an opacity or the interface font.
"""

from __future__ import annotations

import os
from pathlib import Path
import re
import secrets
import subprocess

SKINS = Path(__file__).resolve().parent / "skins"
LIVE_ROOT = (Path.home() / ".config/terminus-est").resolve()
_HEX = re.compile(r"#[0-9a-fA-F]{6}")
_SLOT = re.compile(r"\{\{([a-z0-9_]+)\}\}")


# Colour arithmetic

def is_color(value) -> bool:
    """True for a six-digit `#rrggbb` string."""
    return isinstance(value, str) and _HEX.fullmatch(value) is not None


def rgb(color: str) -> tuple[int, int, int]:
    if not is_color(color):
        raise ValueError(f"expected a six-digit RGB colour, got {color!r}")
    return tuple(int(color[index:index + 2], 16) for index in (1, 3, 5))


def mix(color: str, toward: str, amount: float) -> str:
    """Move `color` towards `toward`; 0 keeps it, 1 reaches the target."""
    return "#" + "".join(
        f"{round(a + (b - a) * amount):02x}" for a, b in zip(rgb(color), rgb(toward)))


def _alpha(alpha: float) -> float:
    if not 0 <= alpha <= 1:
        raise ValueError(f"alpha must be within 0..1, got {alpha!r}")
    return alpha


def rgba(color: str, alpha: float) -> str:
    """CSS `rgba(r, g, b, a)`."""
    channels = ", ".join(map(str, rgb(color)))
    return f"rgba({channels}, {f'{_alpha(alpha):.3f}'.rstrip('0').rstrip('.') or '0'})"


def hex8(color: str, alpha: float) -> str:
    """`#rrggbbaa`, for formats without an rgba() function."""
    rgb(color)
    return f"{color.lower()}{round(_alpha(alpha) * 255):02x}"


# Light/dark mode

MODE_FILE = Path.home() / ".local/state/terminus-est/theme-mode"
# Modus Operandi Tinted, the same light palette as the bar, Zed and Obsidian.
LIGHT = {"background": "#fbf7f0", "foreground": "#000000", "muted": "#595959",
         "outline": "#9f9690", "accent": "#0031a9", "alert": "#a60000"}


def light_mode() -> bool:
    """True while terminus-theme-toggle has put the desktop in light mode."""
    try:
        return MODE_FILE.read_text(encoding="utf-8").strip() == "light"
    except OSError:
        return False


# Tokens

def tokens(settings: dict) -> dict:
    """The palette every skin draws from. Colours are lowercase `#rrggbb`."""
    appearance = settings["appearance"]
    background = appearance["background"].lower()
    foreground = appearance["foreground"].lower()
    accent = appearance["accent"].lower()
    return {
        "background": background,
        "foreground": foreground,
        "muted": appearance["muted"].lower(),
        "outline": appearance["inactive_border"].lower(),
        # One step above the background, for opaque raised surfaces.
        "surface": mix(background, foreground, 0.027),
        "accent": accent,
        # Quieter accent for section labels and secondary marks.
        "accent_dim": mix(accent, background, 0.36),
        "on_accent": background,
        "alert": appearance["alert"].lower(),
        "glass_opacity": float(appearance["glass_opacity"]),
        "chrome_opacity": float(appearance["chrome_opacity"]),
        "popover_opacity": float(appearance["popover_opacity"]),
        "ui_font": appearance["ui_font"],
    }


# Templates

def template(name: str) -> str:
    """A skin template shipped next to the adapters (lib/skins/<name>)."""
    return (SKINS / name).read_text(encoding="utf-8")


def fill(text: str, values: dict) -> str:
    """Replace every `{{name}}` slot; an unknown slot is an error, never silently kept."""
    def slot(match: re.Match) -> str:
        if match[1] not in values:
            raise KeyError(f"unknown skin slot {{{{{match[1]}}}}}")
        return str(values[match[1]])
    return _SLOT.sub(slot, text)


# Publishing

def is_live(root: Path) -> bool:
    """False for an isolated --root: those never touch running applications."""
    return Path(root).resolve() == LIVE_ROOT


def unit_active(unit: str) -> bool:
    """True when the systemd user unit is active."""
    return subprocess.run(["systemctl", "--user", "is-active", "--quiet", unit], timeout=15).returncode == 0


def deploy(path: Path, text: str, mode: int = 0o644) -> bool:
    """Atomically publish `text` at an application's own path; True when it changed.

    The file is replaced in its own directory, so applications that watch that
    directory notice. A symlink at `path` is replaced by a regular file.
    """
    path = Path(path)
    try:
        if not path.is_symlink() and path.read_text(encoding="utf-8") == text:
            return False
    except (OSError, UnicodeDecodeError):
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{secrets.token_hex(8)}.tmp")
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
    return True
