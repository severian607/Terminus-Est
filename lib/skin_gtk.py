"""GTK3/GTK4/libadwaita named colours: generated/gtk-colors.css from design tokens.

~/.config/gtk-3.0/gtk.css and ~/.config/gtk-4.0/gtk.css import the generated
file; GTK applications read it when they start.
"""

from __future__ import annotations

from pathlib import Path

import tokens


def _generated(root: Path) -> Path:
    return root / "generated" / "gtk-colors.css"


def render(settings: dict, root: Path) -> dict[Path, str]:
    t = tokens.tokens(settings)
    values = {name: t[name] for name in ("background", "foreground", "surface", "muted", "accent", "alert")}
    return {_generated(root): tokens.fill(tokens.template("gtk-colors.css"), values)}


def apply(settings: dict, root: Path, changed: set[Path]) -> list[str]:
    if _generated(root) not in changed:
        return []
    if not tokens.is_live(root):
        return ["GTK colours staged in an offline root; running applications were not changed."]
    return ["GTK colours saved; GTK applications read them at their next start."]
