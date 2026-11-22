"""Strata file manager skin: ~/.config/strata/style.css from design tokens.

Strata reads exactly one user stylesheet and hot-reloads it, so the generated
text is published as a regular file at that path. Sidebar icons are matched by
child position; their rules are rebuilt from the live sidebar at deploy time.
"""

from __future__ import annotations

from pathlib import Path
import colorsys
import re
import shlex
import shutil
import sys
import tomllib
from urllib.parse import unquote, urlsplit

import tokens

MARKER = "/* @sidebar-icons@ */"
XDG_PLACES = {  # place id -> (user-dirs key, default directory name)
    "desktop": ("DESKTOP", "Desktop"), "documents": ("DOCUMENTS", "Documents"),
    "downloads": ("DOWNLOAD", "Downloads"), "music": ("MUSIC", "Music"),
    "pictures": ("PICTURES", "Pictures"), "videos": ("VIDEOS", "Videos"),
}
PIN_FALLBACK = "folder"
RESET = ("-gtk-icon-size: 0px; -gtk-icon-transform: scale(0); color: transparent; filter: none; "
         "min-width: 24px; min-height: 20px;\n  background-repeat: no-repeat; background-position: center; "
         "background-size: 20px 20px;")


def _generated(root: Path) -> Path:
    return root / "generated" / "strata.css"


def _icons_dir(root: Path) -> Path:
    return root / "generated" / "strata-icons"


def _icon_sources() -> Path:
    return Path(__file__).resolve().parent.parent / "config" / "strata" / "icons"


def _generated_theme(root: Path) -> Path:
    return root / "generated" / "strata-theme.toml"


def _stylesheet() -> Path:
    return Path.home() / ".config" / "strata" / "style.css"


def _theme() -> Path:
    # Selected in Strata as "Terminus-Est" (theme = "terminus-est" in its settings.toml).
    return Path.home() / ".config" / "strata" / "themes" / "terminus-est.toml"


# Sidebar icon recolouring

_HEX = re.compile(r"#[0-9a-fA-F]{6}")
GREY_SATURATION = 0.18  # HSV saturation below this counts as neutral


def _is_grey(color: str) -> bool:
    r, g, b = (c / 255 for c in tokens.rgb(color.lower()))
    return colorsys.rgb_to_hsv(r, g, b)[1] < GREY_SATURATION


def recolour(svg: str, t: dict) -> str:
    """Map an icon's source colours onto the palette; opacity attributes are untouched.

    Near-grey colours become muted. The most frequent chromatic colour (ties: first seen)
    becomes the accent; every other chromatic colour becomes the highlight tone.
    """
    found = [c.lower() for c in _HEX.findall(svg)]
    chromatic = [c for c in dict.fromkeys(found) if not _is_grey(c)]
    main = max(chromatic, key=lambda c: found.count(c), default=None)
    highlight = tokens.mix(t["accent"], t["foreground"], 0.5)

    def swap(match: re.Match) -> str:
        color = match[0].lower()
        if _is_grey(color):
            return t["muted"]
        return t["accent"] if color == main else highlight
    return _HEX.sub(swap, svg)


def _icons(t: dict, root: Path) -> dict[Path, str]:
    folder = _icons_dir(root) / t["accent"].lstrip("#")
    try:
        sources = sorted(_icon_sources().glob("*.svg"))
    except OSError:
        return {}
    return {folder / source.name: recolour(source.read_text(encoding="utf-8"), t) for source in sources}


def _prune_icons(root: Path, accent: str) -> bool:
    """Remove icon directories of earlier accents; True when something was removed."""
    removed = False
    try:
        entries = list(_icons_dir(root).iterdir())
    except OSError:
        return False
    for entry in entries:
        if entry.name != accent.lstrip("#") and entry.is_dir() and not entry.is_symlink():
            shutil.rmtree(entry, ignore_errors=True)
            removed = True
    return removed


def render(settings: dict, root: Path) -> dict[Path, str]:
    t = tokens.tokens(settings)
    glass, chrome = t["glass_opacity"], t["chrome_opacity"]
    if tokens.light_mode():
        # Modus Operandi Tinted; the cream window is nearly opaque so the wallpaper does not wash out the text.
        t = {**t, **tokens.LIGHT, "surface": "#efe9dd", "accent_dim": tokens.mix(tokens.LIGHT["accent"], "#fbf7f0", 0.36)}
        glass, chrome = 0.94, 0.55
    background, foreground, accent = t["background"], t["foreground"], t["accent"]
    values = {
        "accent": accent,
        "accent_dim": t["accent_dim"],
        "foreground": foreground,
        "muted": t["muted"],
        "glass": tokens.rgba(background, glass),
        "chrome": tokens.rgba(background, chrome),
        "surface": t["surface"],  # solid ground of popovers and dialogs (they are not blurred, so no alpha)
        "sheet": tokens.rgba(background, 0.96),
        "sheet_nav": tokens.rgba(background, 0.40),
        "hairline": tokens.rgba(foreground, 0.12),
        "hover": tokens.rgba(foreground, 0.06),
        "select": tokens.rgba(foreground, 0.08),
        "accent_25": tokens.rgba(accent, 0.25),
        "accent_15": tokens.rgba(accent, 0.15),
    }
    text = tokens.fill(tokens.template("strata.css"), values)
    # Strata's own theme tokens: they colour what the stylesheet cannot reach
    # (interface and folder icons are drawn from the theme accent, not from CSS).
    theme = "".join(f'{key} = "{value}"\n' for key, value in (
        ("name", "Terminus-Est"), ("background", background), ("surface", t["surface"]),
        ("text", foreground), ("accent", accent), ("danger", t["alert"]),
        ("muted", t["outline"]), ("highlight", t["outline"]), ("border", t["outline"]),
        ("dim_text", t["muted"])))
    return {
        _generated(root): text,
        _generated_theme(root): "# Generated by terminus-settings (lib/skin_strata.py); edits here are overwritten.\n" + theme,
        **_icons(t, root),
    }


# Sidebar icons

def _user_dirs(home: Path) -> dict[str, Path]:
    found: dict[str, Path] = {}
    try:
        lines = (home / ".config" / "user-dirs.dirs").read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError):
        lines = []
    for line in lines:
        name, sep, value = line.strip().partition("=")
        if not sep or not name.startswith("XDG_") or not name.endswith("_DIR"):
            continue
        try:
            parts = shlex.split(value)
        except ValueError:
            continue
        if parts:
            found[name[4:-4]] = Path(parts[0].replace("$HOME", str(home)))
    return found


def _read_toml(path: Path) -> dict:
    try:
        with path.open("rb") as handle:
            return tomllib.load(handle)
    except (OSError, UnicodeDecodeError, ValueError, tomllib.TOMLDecodeError):
        return {}


def sidebar_rules(accent: str, home: Path | None = None) -> str:
    """Reset rule plus one icon rule per mapped sidebar row, from the live sidebar layout."""
    home = Path(home) if home else Path.home()
    config = home / ".config"
    strata = _read_toml(config / "strata" / "settings.toml")
    icons = _read_toml(config / "terminus-est" / "strata" / "sidebar-icons.toml")
    dirs = _user_dirs(home)
    xdg = {}
    for place, (key, default) in XDG_PLACES.items():
        xdg[place] = dirs.get(key, home / default)

    visible = []
    for place in strata.get("sidebar_order", []):
        if not strata.get(f"sidebar_show_{place}", True):
            continue
        if place in xdg and (not xdg[place].is_dir() or (place == "desktop" and xdg[place] == home)):
            continue
        visible.append(place)

    hidden = {home, *xdg.values()}
    pins: list[Path] = []
    try:
        bookmarks = (config / "gtk-3.0" / "bookmarks").read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError):
        bookmarks = []
    for line in bookmarks:
        uri = line.strip().split(" ", 1)[0]
        if not uri.startswith("file://"):
            continue
        path = Path(unquote(urlsplit(uri).path))
        if path not in hidden and path not in pins:
            pins.append(path)

    rows: list[tuple[str, str]] = []  # (selector, icon stem)
    places = icons.get("places", {})
    for k, place in enumerate(visible, 1):
        if place in places:
            rows.append((f"window .sidebar > :nth-child({2 * k}) image", places[place]))
    # Pinned rows (.sidebar-pinned-row) alternate with gap widgets inside their own box;
    # an unmapped pin gets the generic folder icon so no stock coloured icon survives.
    pin_icons = icons.get("pins", {})
    for j, path in enumerate(pins, 1):
        rows.append((f"window .sidebar-pinned-row:nth-child({2 * j}) image", pin_icons.get(str(path), PIN_FALLBACK)))

    selectors = [selector for selector, _ in rows]
    device = icons.get("device")
    if device:
        selectors.append("window .sidebar-device image")
    if not selectors:
        return ""
    folder = _icons_dir(home / ".config" / "terminus-est") / accent.lstrip("#").lower()
    out = []
    for start in range(0, len(selectors), 3):
        out.append(", ".join(selectors[start:start + 3]))
    text = ",\n".join(out) + " {\n  " + RESET + "\n}\n"
    for selector, stem in rows:
        text += f'{selector} {{ background-image: url("file://{folder}/{stem}.svg"); }}\n'
    if device:
        text += f'window .sidebar-device image {{ background-image: url("file://{folder}/{device}.svg"); }}\n'
    return text.rstrip("\n")


def deploy(root: Path | None = None) -> bool:
    """Publish the stylesheet with current sidebar rules, and the theme; True when either changed."""
    root = Path(root) if root else tokens.LIVE_ROOT
    try:
        text = _generated(root).read_text(encoding="utf-8")
    except FileNotFoundError:
        print(f"Strata stylesheet not generated yet: {_generated(root)}", file=sys.stderr)
        return False
    accent = _read_toml(_generated_theme(root)).get("accent")
    if not tokens.is_color(accent):
        print(f"Strata theme not generated yet: {_generated_theme(root)}", file=sys.stderr)
        return False
    _prune_icons(root, accent)
    changed = tokens.deploy(_stylesheet(), text.replace(MARKER, sidebar_rules(accent), 1))
    try:
        changed = tokens.deploy(_theme(), _generated_theme(root).read_text(encoding="utf-8")) or changed
    except FileNotFoundError:
        pass
    return changed


def apply(settings: dict, root: Path, changed: set[Path]) -> list[str]:
    icons = _icons_dir(root)
    if not ({_generated(root), _generated_theme(root)} & changed or any(icons in path.parents for path in changed)):
        return []
    if not tokens.is_live(root):
        return ["Strata skin staged in an offline root; the running file manager was not changed."]
    if deploy(root):
        return ["Strata stylesheet and theme updated; it reloads automatically."]
    return []


def refresh(root: Path | None = None) -> bool:
    """Re-render only the Strata skin from settings.toml (current light/dark mode) and publish it."""
    root = Path(root) if root else tokens.LIVE_ROOT
    with (root / "settings.toml").open("rb") as handle:
        settings = tomllib.load(handle)
    for path, text in render(settings, root).items():
        tokens.deploy(path, text)
    return deploy(root)


if __name__ == "__main__":
    if sys.argv[1:] not in (["deploy"], ["refresh"]):
        sys.exit("usage: skin_strata.py deploy|refresh")
    action = refresh if sys.argv[1] == "refresh" else deploy
    print("Strata stylesheet changed" if action() else "Strata stylesheet unchanged")
