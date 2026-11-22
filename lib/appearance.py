"""Ghostty background tint and cursor colour; other palettes are owned by their skins."""

from __future__ import annotations

import math
import os
from pathlib import Path
import signal

import tokens


# Ghostty override rendering

def _number(value: float) -> str:
    return f"{value:.6f}".rstrip("0").rstrip(".") or "0"


def render(settings: dict, root: Path) -> dict[Path, str]:
    """Plan deterministic files without inspecting or changing the live desktop."""
    appearance = settings["appearance"]
    opacity = appearance["background_opacity"]
    background = appearance["background"]
    if (type(opacity) not in (int, float) or not math.isfinite(opacity)
            or not 0 <= opacity <= 1):
        raise ValueError("appearance.background_opacity must be a finite number from 0 to 1")
    if not tokens.is_color(background):
        raise ValueError("appearance.background must be a six-digit RGB color")
    background = background.lower()
    accent = appearance["accent"]
    if not tokens.is_color(accent):
        raise ValueError("appearance.accent must be a six-digit RGB color")
    generated = root / "generated"
    return {
        generated / "appearance-ghostty.conf": (
            "# Terminus-Est generated background and cursor override (terminus-settings)\n"
            "# Font, palette, shader and padding remain in Ghostty config.\n"
            f"background = {background}\nbackground-opacity = {_number(opacity)}\n"
            f"cursor-color = {accent.lower()}\n"
        ),
    }


# Safe native application reload

def reload_ghostty(root: Path) -> list[str]:
    """Signal only this user's managed default-config Ghostty instances.

    SIGUSR2 is Ghostty GTK's native reload action. A pidfd pins process identity;
    a caught-signal check avoids delivering its otherwise terminating default.
    Explicit configs (including the screensaver) and command-line background
    overrides are intentionally not part of the desktop's native config owner.
    """
    generated = (root / "generated" / "appearance-ghostty.conf").resolve()
    reloaded = 0
    skipped = 0
    for process in Path("/proc").iterdir():
        if not process.name.isdecimal():
            continue
        try:
            if process.stat().st_uid != os.getuid():
                continue
            if process.joinpath("comm").read_text().strip() != "ghostty":
                continue
            pid = int(process.name)
            # Opening before inspection means a reused numeric PID cannot be signalled.
            descriptor = os.pidfd_open(pid)
        except (FileNotFoundError, ProcessLookupError):
            continue
        except PermissionError as error:
            raise RuntimeError(f"Cannot inspect Ghostty reload eligibility: {error}") from error
        try:
            executable = Path(os.readlink(process / "exe")).name
            if executable not in ("ghostty", "ghostty (deleted)"):
                continue
            args = process.joinpath("cmdline").read_bytes().split(b"\0")
            options = []
            for raw in args[1:]:
                arg = os.fsdecode(raw)
                if arg in ("-e", "--") or arg.startswith("--command="):
                    break
                options.append(arg)
            if any(arg.split("=", 1)[0] in (
                    "--config-file", "--config-default-files", "--background",
                    "--background-opacity") for arg in options):
                skipped += 1
                continue
            # Only these public config-location variables are used; nothing from
            # the environment or command line is retained or included in notices.
            locations = {}
            for entry in process.joinpath("environ").read_bytes().split(b"\0"):
                key, separator, value = entry.partition(b"=")
                if separator and key in (b"HOME", b"XDG_CONFIG_HOME", b"GHOSTTY_CONFIG_DIR"):
                    locations[os.fsdecode(key)] = os.fsdecode(value)
            home = Path(locations.get("HOME", str(Path.home())))
            config_home = Path(locations.get("XDG_CONFIG_HOME") or home / ".config")
            ghostty_dir = Path(locations.get("GHOSTTY_CONFIG_DIR") or config_home / "ghostty")
            managed = False
            for name in ("config", "config.ghostty"):
                config = ghostty_dir / name
                if not config.is_file():
                    continue
                for line in config.read_text().splitlines():
                    key, separator, value = line.partition("=")
                    if not separator or key.strip() != "config-file":
                        continue
                    value = value.strip().removeprefix("?").strip().strip('"')
                    if value.startswith("~/"):
                        included = home / value[2:]
                    else:
                        included = Path(value)
                        if not included.is_absolute():
                            included = config.parent / included
                    if included.resolve() == generated:
                        managed = True
            if not managed:
                skipped += 1
                continue
            status = dict(line.split(":", 1) for line in process.joinpath("status").read_text().splitlines())
            if not int(status["SigCgt"].strip(), 16) & (1 << (signal.SIGUSR2 - 1)):
                raise RuntimeError(f"Ghostty PID {pid} has no safe SIGUSR2 reload handler; not signalled")
            signal.pidfd_send_signal(descriptor, signal.SIGUSR2)
            reloaded += 1
        except (FileNotFoundError, ProcessLookupError):
            # Exiting instances need no configuration refresh.
            continue
        except OSError as error:
            raise RuntimeError(f"Could not reload Ghostty PID {pid}: {error}") from error
        finally:
            os.close(descriptor)
    notices = []
    if reloaded:
        notices.append(f"Requested native background reload for {reloaded} managed Ghostty process(es). "
                       "Ghostty reports any config parse errors in its own configuration-error dialog.")
    if skipped:
        notices.append(f"Left {skipped} Ghostty process(es) with separate or unmanaged configuration unchanged.")
    if not reloaded and not skipped:
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
    import sys

    if sys.argv[1:] != ["reload-ghostty"]:
        print("usage: appearance.py reload-ghostty", file=sys.stderr)
        sys.exit(2)
    try:
        for notice in reload_ghostty(tokens.LIVE_ROOT):
            print(notice)
    except RuntimeError as error:
        print(error, file=sys.stderr)
        sys.exit(1)
