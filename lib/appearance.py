"""Ghostty background tint and cursor colour; other palettes are owned by their skins."""

from __future__ import annotations

import math
import os
import signal
import sys
from collections.abc import Iterator
from pathlib import Path

import tokens

GHOSTTY_EXECUTABLES = ("ghostty", "ghostty (deleted)")
GHOSTTY_CONFIG_NAMES = ("config", "config.ghostty")
# Command-line options that make an instance's configuration its own, not the desktop's.
GHOSTTY_OVERRIDE_OPTIONS = ("--config-file", "--config-default-files", "--background", "--background-opacity")
# Options after these belong to the program Ghostty runs, not to Ghostty.
GHOSTTY_COMMAND_START = ("-e", "--")
CONFIG_LOCATION_VARIABLES = (b"HOME", b"XDG_CONFIG_HOME", b"GHOSTTY_CONFIG_DIR")


# Ghostty override rendering


def _number(value: float) -> str:
    return f"{value:.6f}".rstrip("0").rstrip(".") or "0"


def render(settings: dict, root: Path) -> dict[Path, str]:
    """Plan deterministic files without inspecting or changing the live desktop."""
    appearance = settings["appearance"]
    opacity = appearance["background_opacity"]
    if type(opacity) not in (int, float) or not math.isfinite(opacity) or not 0 <= opacity <= 1:
        raise ValueError("appearance.background_opacity must be a finite number from 0 to 1")
    for key in ("background", "accent"):
        if not tokens.is_color(appearance[key]):
            raise ValueError(f"appearance.{key} must be a six-digit RGB color")
    return {
        root / "generated" / "appearance-ghostty.conf": (
            "# Terminus-Est generated background and cursor override (terminus-settings)\n"
            "# Font, palette, shader and padding remain in Ghostty config.\n"
            f"background = {appearance['background'].lower()}\nbackground-opacity = {_number(opacity)}\n"
            f"cursor-color = {appearance['accent'].lower()}\n"
        ),
    }


# Safe native application reload


def _open_own_ghostty(process: Path) -> int | None:
    """A pidfd for this user's Ghostty process, else None.

    Opening before any further inspection means a reused numeric PID cannot be signalled.
    """
    try:
        if process.stat().st_uid != os.getuid():
            return None
        if process.joinpath("comm").read_text().strip() != "ghostty":
            return None
        return os.pidfd_open(int(process.name))
    except (FileNotFoundError, ProcessLookupError):
        return None
    except PermissionError as error:
        raise RuntimeError(f"Cannot inspect Ghostty reload eligibility: {error}") from error


def _has_override_options(process: Path) -> bool:
    options = []
    for raw in process.joinpath("cmdline").read_bytes().split(b"\0")[1:]:
        arg = os.fsdecode(raw)
        if arg in GHOSTTY_COMMAND_START or arg.startswith("--command="):
            break
        options.append(arg)
    return any(arg.split("=", 1)[0] in GHOSTTY_OVERRIDE_OPTIONS for arg in options)


def _config_directory(process: Path) -> tuple[Path, Path]:
    """The (home, Ghostty config directory) the process resolves its configuration from.

    Only the public config-location variables are read; nothing from the
    environment or command line is retained or included in notices.
    """
    locations = {}
    for entry in process.joinpath("environ").read_bytes().split(b"\0"):
        key, separator, value = entry.partition(b"=")
        if separator and key in CONFIG_LOCATION_VARIABLES:
            locations[os.fsdecode(key)] = os.fsdecode(value)
    home = Path(locations.get("HOME", str(Path.home())))
    config_home = Path(locations.get("XDG_CONFIG_HOME") or home / ".config")
    return home, Path(locations.get("GHOSTTY_CONFIG_DIR") or config_home / "ghostty")


def _included_configs(config: Path, home: Path) -> Iterator[Path]:
    """Every `config-file = ...` target named in one Ghostty config, as a path."""
    for line in config.read_text().splitlines():
        key, separator, value = line.partition("=")
        if not separator or key.strip() != "config-file":
            continue
        value = value.strip().removeprefix("?").strip().strip('"')
        if value.startswith("~/"):
            yield home / value[2:]
        else:
            included = Path(value)
            yield included if included.is_absolute() else config.parent / included


def _includes_generated(process: Path, generated: Path) -> bool:
    home, ghostty_dir = _config_directory(process)
    for name in GHOSTTY_CONFIG_NAMES:
        config = ghostty_dir / name
        if config.is_file() and any(included.resolve() == generated for included in _included_configs(config, home)):
            return True
    return False


def _handles_sigusr2(process: Path) -> bool:
    """True when SIGUSR2 is caught: its default action would terminate Ghostty."""
    status = dict(line.split(":", 1) for line in process.joinpath("status").read_text().splitlines())
    return bool(int(status["SigCgt"].strip(), 16) & (1 << (signal.SIGUSR2 - 1)))


def _reload_if_managed(process: Path, descriptor: int, generated: Path) -> str | None:
    """Signal one Ghostty process.

    "reloaded", "skipped" (separate configuration), "starting" (no SIGUSR2 handler yet), or None (not a Ghostty binary).
    """
    if Path(os.readlink(process / "exe")).name not in GHOSTTY_EXECUTABLES:
        return None
    if _has_override_options(process) or not _includes_generated(process, generated):
        return "skipped"
    if not _handles_sigusr2(process):
        # Ghostty installs the handler after it has read its configuration, so an instance without one is still
        # starting (at login it races the first palette sync) and picks the new file up by itself.
        return "starting"
    signal.pidfd_send_signal(descriptor, signal.SIGUSR2)
    return "reloaded"


def reload_ghostty(root: Path) -> list[str]:
    """Signal only this user's managed default-config Ghostty instances.

    SIGUSR2 is Ghostty GTK's native reload action. A pidfd pins process identity;
    a caught-signal check avoids delivering its otherwise terminating default.
    Explicit configs (including the screensaver) and command-line background
    overrides are intentionally not part of the desktop's native config owner.
    """
    generated = (root / "generated" / "appearance-ghostty.conf").resolve()
    counts = {"reloaded": 0, "skipped": 0, "starting": 0}
    for process in Path("/proc").iterdir():
        if not process.name.isdecimal():
            continue
        descriptor = _open_own_ghostty(process)
        if descriptor is None:
            continue
        try:
            outcome = _reload_if_managed(process, descriptor, generated)
        except (FileNotFoundError, ProcessLookupError):
            continue  # exiting instances need no configuration refresh
        except OSError as error:
            raise RuntimeError(f"Could not reload Ghostty PID {process.name}: {error}") from error
        finally:
            os.close(descriptor)
        if outcome:
            counts[outcome] += 1
    notices = []
    if counts["reloaded"]:
        notices.append(
            f"Requested native background reload for {counts['reloaded']} managed Ghostty process(es). "
            "Ghostty reports any config parse errors in its own configuration-error dialog."
        )
    if counts["skipped"]:
        notices.append(
            f"Left {counts['skipped']} Ghostty process(es) with separate or unmanaged configuration unchanged."
        )
    if counts["starting"]:
        notices.append(
            f"Did not signal {counts['starting']} Ghostty process(es) that are still starting; "
            "they read the saved background tint as they load."
        )
    if not any(counts.values()):
        notices.append("Ghostty is not running; its next launch will read the saved background tint.")
    return notices


def apply(settings: dict, root: Path, changed: set[Path]) -> list[str]:
    """Apply only changed native outputs; offline roots never touch live apps."""
    if root / "generated" / "appearance-ghostty.conf" not in changed:
        return []
    if not tokens.is_live(root):
        return ["Native application appearance staged in an offline root; no running applications were changed."]
    return reload_ghostty(root)


if __name__ == "__main__":
    if sys.argv[1:] != ["reload-ghostty"]:
        print("usage: appearance.py reload-ghostty", file=sys.stderr)
        sys.exit(2)
    try:
        for notice in reload_ghostty(tokens.LIVE_ROOT):
            print(notice)
    except RuntimeError as error:
        print(error, file=sys.stderr)
        sys.exit(1)
