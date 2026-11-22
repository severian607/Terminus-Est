"""Notification skins: mako config and the SwayOSD stylesheet from design tokens.

mako reads its config at start and on `makoctl reload`; SwayOSD reads its
stylesheet only when swayosd-server starts, so it is restarted when it changes.
"""

from __future__ import annotations

from pathlib import Path
import subprocess
import tokens


def _paths(root: Path) -> tuple[Path, Path]:
    return root / "generated" / "mako.ini", root / "swayosd" / "style.css"


def render(settings: dict, root: Path) -> dict[Path, str]:
    """Plan deterministic files without inspecting or changing the live desktop."""
    values = tokens.tokens(settings)
    # Notifications use the same glass translucency as application windows; Hyprland blurs them.
    values["glass_alpha"] = tokens.hex8(values["background"], values["glass_opacity"])
    mako, swayosd = _paths(root)
    return {
        mako: tokens.fill(tokens.template("mako.ini"), values),
        swayosd: tokens.fill(tokens.template("swayosd.css"), values),
    }


def _run(command: list[str]) -> None:
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeError(f"{' '.join(command)} failed: {error}") from error
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError(f"{' '.join(command)} failed: {detail or result.returncode}")


def apply(settings: dict, root: Path, changed: set[Path]) -> list[str]:
    """Reload only what changed; offline roots never touch running applications."""
    mako, swayosd = _paths(root)
    affected = {mako, swayosd} & changed
    if not affected:
        return []
    if not tokens.is_live(root):
        return ["Notification skins staged in an offline root; no running applications were changed."]
    notices = []
    if mako in affected:
        if tokens.unit_active("mako.service"):
            _run(["makoctl", "reload"])
            notices.append("mako reloaded.")
    if swayosd in affected:
        _run(["systemctl", "--user", "try-restart", "swayosd-server.service"])
        notices.append("SwayOSD restarted to read its new stylesheet.")
    return notices
