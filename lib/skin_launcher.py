"""Terminus-Launcher skin: the launcher's theme.json from design tokens.

The launcher watches theme.json, but the file reaches it through a link that an
atomic replace can bypass, so a running launcher is told to re-read it.
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path

import tokens

RELOAD_TIMEOUT_SECONDS = 5


def _path(root: Path) -> Path:
    return root / "terminus-launcher" / "theme.json"


def render(settings: dict, root: Path) -> dict[Path, str]:
    """Plan deterministic files without inspecting or changing the live desktop."""
    values = tokens.tokens(settings)
    theme = {
        "background": values["background"],
        "foreground": values["foreground"],
        "muted": values["muted"],
        "outline": values["outline"],
        "accent": values["accent"],
        "accentDim": values["accent_dim"],
        "alert": values["alert"],
        "uiFont": values["ui_font"],
    }
    return {_path(root): json.dumps(theme, indent=2) + "\n"}


def apply(settings: dict, root: Path, changed: set[Path]) -> list[str]:
    """Ask a running launcher to re-read its theme; offline roots touch nothing."""
    if _path(root) not in changed:
        return []
    if not tokens.is_live(root):
        return ["Terminus-Launcher theme staged in an offline root; no running launcher was changed."]
    if not tokens.unit_active("terminus-launcher.service"):
        return ["Terminus-Launcher is not running; it reads the new theme when it starts."]
    command = [str(Path(__file__).resolve().parent.parent / "bin" / "terminus-launcher"), "theme-reload"]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=RELOAD_TIMEOUT_SECONDS)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeError(f"{' '.join(command)} failed: {error}") from error
    output = f"{result.stdout}\n{result.stderr}".strip()
    if result.returncode or "not found" in output.lower():
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError(f"{' '.join(command)} failed: {detail or result.returncode}")
    return ["Terminus-Launcher theme reloaded."]
