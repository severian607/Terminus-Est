"""Shared stylesheet and theme loading for the Super+D/Super+A/Super+I overlay pickers."""

import json
import subprocess
import tomllib
from pathlib import Path

from settings import SettingsError, defaults as settings_defaults, validate_settings
from tokens import mix, tokens

SETTINGS_FILE = Path.home() / ".config/terminus-est/settings.toml"

# Square, flat, monospace, one 1px outline; brightness carries state.
CSS_TEMPLATE = """
window { background: transparent; }
.frame-wrap { padding-top: 9px; }
.shader-panel {
    background: @bg@; color: @fg@; border: 1px solid @line@; border-radius: 0;
    padding: 24px 20px 12px 20px;
    font-family: '@font@', 'JetBrains Mono', 'Noto Sans Mono', monospace;
    font-size: 13px;
}
.frame-title { background: @bg@; color: @fg@; padding: 0 6px; margin: 0 14px; font-size: 13px; }
.frame-state { background: @bg@; color: #ffffff; padding: 0 6px; margin: 0 14px; font-size: 12px; }
.search { background: transparent; color: @fg@; border: 1px solid @line@; border-radius: 0;
          padding: 4px 8px; min-height: 24px; box-shadow: none; }
.search:focus-within { border-color: @accent@; }
.search text { color: @fg@; }
.strip-label { color: @dim@; font-size: 12px; }
.chip { color: @accent@; font-size: 12px; margin-right: 8px; }
.chip.shadowed { color: @dim@; }
.none { color: @dim@; font-size: 12px; }
.clear { background: transparent; color: @muted@; border: 1px solid @line@; border-radius: 0;
         box-shadow: none; padding: 1px 10px; min-height: 0; font-size: 12px; }
.clear:hover { background: @hover@; color: @fg@; }
.clear:disabled { color: @dim@; }
.strip, .body-rule { border-bottom: 1px solid @line@; padding-bottom: 8px; }
.cat-pane { border-right: 1px solid @line@; padding-right: 12px; }
.cats { background: transparent; }
.cats row { border-radius: 0; padding: 5px 8px; border-left: 2px solid transparent; color: @muted@; }
.cats row:hover { background: @hover@; }
.cats row:selected { background: @hover@; color: @fg@; border-left-color: @accent@; }
.count { color: @dim@; font-size: 12px; }
.cats row.nested { padding-top: 0; padding-bottom: 0; }
.tree-trunk, .tree-tick { background: @accent@; }
.fx-list { background: transparent; }
.fx-list listitem { padding: 7px 10px; border-left: 2px solid transparent;
                    border-bottom: 1px solid @line@; border-radius: 0; }
.fx-list listitem:hover { background: @hover@; }
.fx-list listitem:selected { background: @hover@; border-left-color: @accent@; }
.emoji { font-size: 20px; min-width: 34px; }
.fx-name { color: @fg@; font-size: 14px; }
.fx-name.on-name { color: @accent@; }
.fx-desc { color: @muted@; font-size: 12px; }
.meta { color: @dim@; font-size: 11px; }
.on { color: @accent@; font-size: 12px; font-weight: 700; }
.shadow { color: @dim@; font-size: 12px; }
.empty { color: @dim@; font-size: 14px; }
.footer { color: @dim@; font-size: 11px; margin-top: 10px; }
.status { color: @alert@; font-size: 11px; margin-top: 10px; }
"""


def hypr_json(command):
    """Parsed `hyprctl COMMAND -j`."""
    return json.loads(subprocess.check_output(
        ["hyprctl", command, "-j"], text=True, timeout=2))


def read_theme(extra_css=""):
    """Return (css bytes, stamp) from the central settings; defaults fill missing keys.

    `extra_css` holds a picker's own rules and may use the same @token@ names.
    """
    try:
        mtime = SETTINGS_FILE.stat().st_mtime
        with SETTINGS_FILE.open("rb") as handle:
            saved = tomllib.load(handle)
    except (OSError, tomllib.TOMLDecodeError):
        mtime, saved = None, {}
    try:
        settings = validate_settings(saved)
    except SettingsError:
        mtime, settings = None, settings_defaults()
    palette = tokens(settings)
    bg, fg, muted = palette["background"], palette["foreground"], palette["muted"]
    values = {
        "bg": bg, "fg": fg, "accent": palette["accent"], "alert": palette["alert"],
        "line": mix(bg, fg, 0.15), "hover": mix(bg, fg, 0.07),
        "muted": muted, "dim": mix(bg, muted, 0.7), "font": palette["ui_font"],
    }
    css = CSS_TEMPLATE + extra_css
    for key, value in values.items():
        css = css.replace(f"@{key}@", value)
    return css.encode(), (str(SETTINGS_FILE), mtime)
