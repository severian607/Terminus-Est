"""Zed theme skin: ~/.config/zed/themes/terminus-est.json from lib/skins/zed.json."""

from __future__ import annotations

import json
from pathlib import Path

import tokens

THEME = "zed/themes/terminus-est.json"
# ~/.config/zed links into the config root, so this file is the one Zed loads.


def _slots(settings: dict) -> dict:
    t = tokens.tokens(settings)
    accent, background, foreground = t["accent"], t["background"], t["foreground"]
    glass = t["glass_opacity"]
    return {
        "glass": tokens.hex8(background, glass),
        # The inactive title bar sits slightly below the glass level.
        "glass_inactive": tokens.hex8(background, glass * 0.87),
        "popover": tokens.hex8(background, t["popover_opacity"]),
        "foreground": foreground,
        "muted": t["muted"],
        # Placeholder and disabled text: one step below muted.
        "dim": tokens.mix(t["muted"], background, 0.28),
        "outline": t["outline"],
        "accent": accent,
        "accent_66": accent + "66",
        "accent_59": accent + "59",
        "link_hover": tokens.mix(accent, foreground, 0.3),
        # Neutral list selection; the accent is reserved for marks.
        "selected": tokens.hex8(foreground, 0.08),
    }


def render(settings: dict, root: Path) -> dict[Path, str]:
    text = tokens.fill(tokens.template("zed.json"), _slots(settings))
    try:
        json.loads(text)
    except ValueError as error:
        raise ValueError(f"generated Zed theme is not valid JSON: {error}") from error
    return {Path(root) / THEME: text}


def apply(settings: dict, root: Path, changed: set[Path]) -> list[str]:
    if Path(root) / THEME not in changed:
        return []
    if not tokens.is_live(root):
        return ["Zed theme not published (isolated root)"]
    return ["Zed reloads the theme file by itself; nothing to signal"]
