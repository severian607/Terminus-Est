#!/usr/bin/env python3
"""Typed Terminus-Est settings and offline native-configuration rendering."""
from __future__ import annotations

import argparse
import contextlib
import copy
import fcntl
import importlib
import json
import math
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import stat
import subprocess
import sys
import tomllib

import tokens


# Schema
PROJECT = Path(__file__).resolve().parents[1]
MAX_BYTES = 1024 * 1024
# Skin adapters (skin_*) turn the shared tokens in lib/tokens.py into native application files.
ADAPTER_NAMES = ("appearance", "session", "skin_strata", "skin_zed", "skin_launcher", "skin_notify", "skin_gtk")
# How a changed key reaches Hyprland (config/hyprland/look_and_feel.lua reads generated/appearance.lua):
# colour keys are re-applied live by o.apply_colors(); the keys in HYPRLAND_RELOAD_KEYS are read once at
# load, so they need `hyprctl reload`; every other key is consumed elsewhere (Ghostty, Terminus-Bar,
# the skins) and leaves Hyprland alone.
HYPRLAND_COLOR_KEYS = frozenset("appearance." + name for name in (
    "accent", "active_border", "inactive_border", "background", "muted", "alert"))
HYPRLAND_RELOAD_KEYS = frozenset("appearance." + name for name in (
    "active_opacity", "inactive_opacity", "default_active_opacity", "default_inactive_opacity",
    "global_active_opacity", "global_inactive_opacity", "blur", "blur_strength", "brightness",
    "contrast", "dim_inactive", "dim_strength", "ui_font"))
SCHEMA = []


def control(key, label, kind, default, minimum=None, maximum=None, step=None, choices=None):
    entry = {"key": key, "label": label, "kind": kind, "default": default}
    if minimum is not None:
        entry.update(min=minimum, max=maximum, step=step)
    if choices is not None:
        entry["choices"] = choices
    SCHEMA.append(entry)


control("appearance.background_opacity", "Application background opacity", "number", 0.42, 0, 1, 0.01)
control("appearance.active_opacity", "Preserved applications: active opacity", "number", 1.0, 0.1, 1, 0.01)
control("appearance.inactive_opacity", "Preserved applications: inactive opacity", "number", 0.97, 0.1, 1, 0.01)
control("appearance.default_active_opacity", "Ordinary applications: active opacity", "number", 0.97, 0.1, 1, 0.01)
control("appearance.default_inactive_opacity", "Ordinary applications: inactive opacity", "number", 0.90, 0.1, 1, 0.01)
control("appearance.global_active_opacity", "Compositor global active opacity", "number", 0.97, 0.1, 1, 0.01)
control("appearance.global_inactive_opacity", "Compositor global inactive opacity", "number", 0.95, 0.1, 1, 0.01)
control("appearance.blur", "Compositor background blur", "boolean", True)
control("appearance.blur_strength", "Blur strength", "integer", 6, 1, 20, 1)
control("appearance.brightness", "Blur background brightness (not monitor backlight)", "number", 0.42, 0, 2, 0.01)
control("appearance.contrast", "Blur background contrast", "number", 0.87, 0, 2, 0.01)
control("appearance.dim_inactive", "Dim inactive windows", "boolean", False)
control("appearance.dim_strength", "Inactive window dim strength", "number", 0.5, 0, 1, 0.01)
control("appearance.bar_opacity", "Terminus-Bar background opacity", "number", 1.0, 0, 1, 0.01)
control("appearance.background", "Desktop and application background tint", "color", "#0b0b0b")
control("appearance.accent", "Accent (follows the wallpaper: terminus-skwd-palette sets it on every wallpaper change)", "color", "#86d993")
control("appearance.active_border", "Active window border color (follows the accent)", "color", "#86d993")
control("appearance.inactive_border", "Inactive window border and hairline color", "color", "#262626")
control("appearance.foreground", "Text color", "color", "#ececec")
control("appearance.muted", "Secondary text color", "color", "#8c8c8c")
control("appearance.alert", "Alert and critical color", "color", "#e8a07a")
control("appearance.glass_opacity", "Application glass opacity (Strata, Zed)", "number", 0.46, 0, 1, 0.01)
control("appearance.chrome_opacity", "Extra opacity of sidebars and headers above the glass", "number", 0.28, 0, 1, 0.01)
control("appearance.popover_opacity", "Menu and popover opacity", "number", 0.90, 0, 1, 0.01)
control("appearance.ui_font", "Interface font family", "select", "MonaspiceNe Nerd Font", choices=["MonaspiceNe Nerd Font", "Monaspace Neon", "Monaspace Argon", "JetBrainsMono Nerd Font", "JetBrainsMonoNL Nerd Font"])
control("screensaver.enabled", "Enable non-locking Terminus-Screensaver", "boolean", True)
control("screensaver.idle_seconds", "Non-locking screensaver idle delay (seconds)", "integer", 150, 30, 7200, 1)
control("screensaver.style", "Terminus-Screensaver style (does not authenticate)", "select", "per-monitor", choices=["per-monitor", "wordmark", "arcade"])
control("screensaver.fps", "Terminus-Screensaver frame rate (capped at each monitor's refresh rate)", "integer", 30, 15, 144, 1)
control("login.animation", "Login animation (explicit authenticated apply)", "select", "dur_file", choices=["none", "doom", "matrix", "colormix", "gameoflife", "dur_file", "terminus"])
control("login.password_style", "Login password field style (explicit authenticated apply)", "select", "constellation", choices=["asterisk", "constellation"])
control("login.animation_frame_delay", "Login animation frame delay (milliseconds)", "integer", 20, 10, 1000, 1)
control("login.animation_timeout_sec", "Login animation timeout (seconds)", "integer", 60, 0, 3600, 1)
control("login.background", "Login background", "color", "#0b0b0b")
control("login.foreground", "Login foreground", "color", "#ececec")
control("login.accent", "Login accent", "color", "#86d993")
control("login.show_borders", "Show login borders", "boolean", True)
control("login.show_clock", "Show login clock (24-hour hours and minutes)", "boolean", False)
CONTROLS = {entry["key"]: entry for entry in SCHEMA}


class SettingsError(Exception):
    """An actionable validation, persistence, or application failure."""


def defaults():
    result = {}
    for entry in SCHEMA:
        section, key = entry["key"].split(".")
        result.setdefault(section, {})[key] = entry["default"]
    return result


def validate_value(key, value):
    entry = CONTROLS.get(key)
    if entry is None:
        raise SettingsError(f"Unknown settings key: {key}")
    kind = entry["kind"]
    if kind == "boolean":
        valid = type(value) is bool
    elif kind == "integer":
        valid = type(value) is int
    elif kind == "number":
        valid = type(value) is int or (type(value) is float and math.isfinite(value))
    elif kind == "color":
        valid = tokens.is_color(value)
    else:
        valid = isinstance(value, str) and value in entry["choices"]
    if not valid:
        raise SettingsError(f"{key}: expected {kind}" + (f" from {entry['choices']}" if kind == "select" else ""))
    if "min" in entry and not entry["min"] <= value <= entry["max"]:
        raise SettingsError(f"{key}: expected {entry['min']}..{entry['max']}")
    return float(value) if kind == "number" else value


def validate_settings(data):
    if not isinstance(data, dict):
        raise SettingsError("Settings must be a TOML object")
    result = defaults()
    for section, values in data.items():
        if section not in result or not isinstance(values, dict):
            raise SettingsError(f"Unknown or invalid settings section: {section}")
        for key, value in values.items():
            result[section][key] = validate_value(f"{section}.{key}", value)
    return result


# Source preservation and bounded input

def load(root, root_fd):
    text = read_root_text(root, root / "settings.toml", root_fd)
    try:
        return validate_settings(tomllib.loads(text or "")), text or ""
    except tomllib.TOMLDecodeError as exc:
        raise SettingsError(f"settings.toml: {exc}") from exc


def literal(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False)


def default_source():
    lines = ["# Terminus-Est central settings", "# Edit through terminus-settings; login changes require explicit login-apply.", ""]
    for section, values in defaults().items():
        lines += [f"# {section.capitalize()}", f"[{section}]"]
        lines += [f"{key} = {literal(value)}" for key, value in values.items()]
        lines.append("")
    return "\n".join(lines)


def update_source(text, dotted_key, value, expected):
    """Replace one owned scalar, retaining all other bytes including comments.

    Bare dotted keys and ordinary section keys are supported. Unusual valid TOML
    forms are rejected instead of being rewritten destructively.
    """
    if not text:
        text = default_source()
    section, name = dotted_key.split(".")
    lines = text.splitlines(keepends=True)
    current = ""
    insertion = None
    matched = False
    for index, line in enumerate(lines):
        header = re.fullmatch(r"\s*\[([A-Za-z0-9_-]+)\]\s*(?:#.*)?(?:\r?\n)?", line)
        if header:
            if current == section and insertion is None:
                insertion = index
            current = header[1]
        key_pattern = re.escape(name if current == section else dotted_key if current == "" else "\0")
        match = re.fullmatch(r"(\s*" + key_pattern + r"\s*=\s*)(\"(?:[^\"\\]|\\.)*\"|'[^']*'|[^#\r\n]*?)(\s*(?:#.*)?)(\r?\n)?", line)
        if match:
            lines[index] = match[1] + literal(value) + match[3] + (match[4] or "")
            matched = True
            break
    if not matched:
        if current == section and insertion is None:
            insertion = len(lines)
        if insertion is None:
            if lines and not lines[-1].endswith("\n"):
                lines[-1] += "\n"
            lines.extend([f"\n# {section.capitalize()}\n", f"[{section}]\n", f"{name} = {literal(value)}\n"])
        else:
            if insertion and not lines[insertion - 1].endswith("\n"):
                lines[insertion - 1] += "\n"
            lines.insert(insertion, f"{name} = {literal(value)}\n")
    updated = "".join(lines)
    try:
        parsed = validate_settings(tomllib.loads(updated))
    except (tomllib.TOMLDecodeError, SettingsError) as exc:
        raise SettingsError("Cannot safely update this TOML layout; use ordinary [section] and scalar key = value syntax") from exc
    if parsed != expected:
        raise SettingsError("TOML edit would change unrelated settings; source was not modified")
    return updated


# Native adapter planning

def adapters():
    result = []
    for name in ADAPTER_NAMES:
        try:
            module = importlib.import_module(name)
        except ImportError as exc:
            raise SettingsError(f"Required native adapter {name} is unavailable: {exc}") from exc
        if not callable(getattr(module, "render", None)) or not callable(getattr(module, "apply", None)):
            raise SettingsError(f"Required adapter {name} does not implement render/apply")
        result.append(module)
    return result


def safe_output(root, path):
    path = Path(path)
    if not path.is_absolute():
        raise SettingsError(f"Adapter returned a non-absolute path: {path}")
    if ".." in path.parts or not path.is_relative_to(root) or path == root:
        raise SettingsError(f"Refusing generated path outside configuration root: {path}")
    return path


def core_render(settings, root):
    appearance = settings["appearance"]
    lua = "-- # Generated Terminus-Est appearance; edit through terminus-settings.\nreturn {\n"
    lua += "".join(f"  {key} = {literal(value)},\n" for key, value in appearance.items())
    lua += "}\n"
    # The bar's colours come from the wallpaper palette (tools/terminus-skwd-palette);
    # this layer carries only the settings Terminus-Bar cannot derive from it.
    return {
        root / "generated/appearance.lua": lua,
        root / "terminus-bar/99-terminus-appearance.toml": (
            "# Generated Terminus-Est native appearance; edit through terminus-settings.\n"
            "# Terminus-Bar root rail\n[bar.default]\n"
            f"background_opacity = {literal(appearance['bar_opacity'])}\n"
        ),
    }


def render_plan(settings, root, modules, root_fd):
    outputs = core_render(settings, root)
    for module in modules:
        try:
            rendered = module.render(copy.deepcopy(settings), root)
        except Exception as exc:
            raise SettingsError(f"{module.__name__} render failed: {exc}") from exc
        if not isinstance(rendered, dict):
            raise SettingsError(f"{module.__name__} render did not return a path/text mapping")
        for path, content in rendered.items():
            if path in outputs:
                raise SettingsError(f"Duplicate generated-file ownership: {path}")
            outputs[path] = content
    for path, content in outputs.items():
        check_output(root, path, root_fd)
        if path == root / "settings.toml" or not isinstance(content, str):
            raise SettingsError(f"Invalid adapter output: {path}")
        if len(content.encode()) > MAX_BYTES:
            raise SettingsError(f"Generated output exceeds size limit: {path}")
    return outputs


# Locked atomic persistence
@contextlib.contextmanager
def opened_directory(path, *, dir_fd=None, create=False, missing_ok=False):
    """Pin every component without following links, including the selected root."""
    path = Path(path)
    if ".." in path.parts:
        raise SettingsError(f"Refusing parent traversal: {path}")
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
    fd = os.open("/", flags) if path.is_absolute() else os.dup(dir_fd)
    parts = path.parts[1:] if path.is_absolute() else path.parts
    try:
        for part in parts:
            if create:
                try:
                    os.mkdir(part, mode=0o755, dir_fd=fd)
                except FileExistsError:
                    pass
            try:
                child = os.open(part, flags, dir_fd=fd)
            except FileNotFoundError:
                if not missing_ok:
                    raise
                os.close(fd)
                fd = None
                break
            os.close(fd)
            fd = child
        yield fd
    finally:
        if fd is not None:
            os.close(fd)


def read_file(directory, name):
    """Return a bounded regular-file snapshot and mode from one opened inode."""
    try:
        fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC, dir_fd=directory)
    except FileNotFoundError:
        return None, None
    with os.fdopen(fd, "rb") as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode):
            raise SettingsError(f"Expected a regular configuration file: {name}")
        raw = source.read(MAX_BYTES + 1)
    if len(raw) > MAX_BYTES:
        raise SettingsError(f"File exceeds {MAX_BYTES} bytes: {name}")
    return raw, stat.S_IMODE(info.st_mode)


def read_root_text(root, path, root_fd):
    relative = safe_output(root, path).relative_to(root)
    if root_fd is None:
        return None
    with opened_directory(relative.parent, dir_fd=root_fd, missing_ok=True) as parent:
        raw = None if parent is None else read_file(parent, relative.name)[0]
    try:
        return None if raw is None else raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise SettingsError(f"Expected UTF-8: {path}") from exc


def check_output(root, path, root_fd):
    relative = safe_output(root, path).relative_to(root)
    if root_fd is None:
        return
    with opened_directory(relative.parent, dir_fd=root_fd, missing_ok=True) as parent:
        if parent is None:
            return
        try:
            info = os.stat(relative.name, dir_fd=parent, follow_symlinks=False)
        except FileNotFoundError:
            return
        if not stat.S_ISREG(info.st_mode):
            raise SettingsError(f"Refusing non-regular generated file: {path}")


@contextlib.contextmanager
def locked(root):
    with opened_directory(root, create=True) as root_fd:
        fd = os.open(".settings.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600, dir_fd=root_fd)
        with os.fdopen(fd, "a") as lock:
            if not stat.S_ISREG(os.fstat(lock.fileno()).st_mode):
                raise SettingsError("Expected a regular settings lock file")
            fcntl.flock(lock, fcntl.LOCK_EX)
            yield root_fd


def atomic_write(directory, name, content, mode):
    temporary = f".{name}.{secrets.token_hex(16)}.tmp"
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, mode, dir_fd=directory)
    try:
        with os.fdopen(fd, "wb") as target:
            os.fchmod(target.fileno(), mode)
            target.write(content)
            target.flush()
            os.fsync(target.fileno())
        os.replace(temporary, name, src_dir_fd=directory, dst_dir_fd=directory)
        os.fsync(directory)
    finally:
        try:
            os.unlink(temporary, dir_fd=directory)
        except FileNotFoundError:
            pass


def commit(root, outputs, root_fd):
    changed = set()
    originals = {}
    completed = []
    restoring = False

    def cancel(signum, frame):
        # Do not let a second cancellation interrupt restoration. SIGKILL and
        # power loss cannot be recovered without a durable transaction journal.
        if not restoring:
            if signum == signal.SIGINT:
                raise KeyboardInterrupt
            raise SystemExit(128 + signum)

    previous_handlers = {}
    with contextlib.ExitStack() as directories:
        try:
            for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
                previous_handlers[signum] = signal.getsignal(signum)
                signal.signal(signum, cancel)
            parents = {}
            for path, content in outputs.items():
                relative = safe_output(root, path).relative_to(root)
                if relative.parent not in parents:
                    parents[relative.parent] = directories.enter_context(
                        opened_directory(relative.parent, dir_fd=root_fd, create=True))
                parent = parents[relative.parent]
                raw, mode = read_file(parent, relative.name)
                if raw is not None:
                    try:
                        raw.decode("utf-8")
                    except UnicodeDecodeError as exc:
                        raise SettingsError(f"Expected UTF-8: {path}") from exc
                encoded = content.encode()
                if raw != encoded:
                    originals[path] = (parent, relative.name, raw, mode, encoded)
            for path, (parent, name, raw, mode, encoded) in originals.items():
                # Include the in-flight replacement: it may publish then fail
                # during fsync or be interrupted before returning.
                completed.append(path)
                atomic_write(parent, name, encoded, 0o644 if mode is None else mode)
                changed.add(path)
        except BaseException as exc:
            restoring = True
            failures = []
            for path in reversed(completed):
                parent, name, data, mode, _ = originals[path]
                try:
                    if data is None:
                        try:
                            os.unlink(name, dir_fd=parent)
                        except FileNotFoundError:
                            pass
                        os.fsync(parent)
                    else:
                        atomic_write(parent, name, data, mode)
                except BaseException as rollback:
                    failures.append(f"{path}: {type(rollback).__name__}: {rollback}")
            detail = "; rollback incomplete: " + "; ".join(failures) if failures else "; prior file contents restored"
            if not isinstance(exc, Exception):
                # Preserve the original KeyboardInterrupt/SystemExit identity
                # and expose recovery failures even when SystemExit hides notes.
                exc.add_note("Persistence interrupted" + detail)
                print("terminus-settings: Persistence interrupted" + detail, file=sys.stderr)
                raise
            raise SettingsError(f"Persistence failed: {exc}{detail}") from exc
        finally:
            for signum, handler in previous_handlers.items():
                signal.signal(signum, handler)
    return changed


# Explicit runtime application

def apply_runtime(settings, root, changed, modules, keys=None):
    """Apply changed outputs. `keys` names the settings a set/reset touched; None means everything."""
    notices, failures = [], []
    for module in modules:
        try:
            messages = module.apply(copy.deepcopy(settings), root, changed)
            if not isinstance(messages, list) or any(not isinstance(message, str) for message in messages):
                raise SettingsError("apply must return a list of diagnostic strings")
            notices.extend(messages)
        except Exception as exc:
            failures.append(f"{module.__name__}: {exc}")
    if root / "generated/appearance.lua" in changed and (keys is None or keys & (HYPRLAND_COLOR_KEYS | HYPRLAND_RELOAD_KEYS)):
        if os.environ.get("HYPRLAND_INSTANCE_SIGNATURE"):
            command = shutil.which("hyprctl")
            if command is None:
                failures.append("Hyprland: hyprctl is unavailable")
            else:
                colors_only = keys is not None and not keys & HYPRLAND_RELOAD_KEYS
                try:
                    proc = None
                    if colors_only:
                        proc = subprocess.run([command, "eval", "o.apply_colors()"], capture_output=True, text=True, timeout=15)
                    if proc is None or proc.returncode:
                        # A configuration loaded before o.apply_colors existed needs the full reload.
                        colors_only = False
                        proc = subprocess.run([command, "reload"], capture_output=True, text=True, timeout=15)
                    if proc.returncode:
                        failures.append("Hyprland reload: " + (proc.stderr or proc.stdout).strip()[:2000])
                    else:
                        notices.append("Hyprland colours updated." if colors_only else "Hyprland appearance reloaded.")
                except (OSError, subprocess.TimeoutExpired) as exc:
                    failures.append(f"Hyprland reload: {exc}")
        else:
            notices.append("Hyprland is not available in this session; appearance is saved for its next load.")
    if root / "generated/appearance.lua" in changed and (keys is None or "appearance.accent" in keys):
        # Rebuild/switch the sword cursor for the new accent; building takes seconds, so detach.
        cursor = Path(__file__).resolve().parents[1] / "modules/terminus-cursor/terminus-cursor"
        try:
            subprocess.Popen([str(cursor), "apply", settings["appearance"]["accent"]], start_new_session=True,
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError as exc:
            failures.append(f"Cursor: {exc}")
    if root / "terminus-bar/99-terminus-appearance.toml" in changed:
        notices.append("Terminus-Bar appearance generated; a running Terminus-Bar observes its configuration directory (no forced reload).")
    for notice in notices:
        print(notice, file=sys.stderr)
    if failures:
        raise SettingsError("Settings are SAVED and generated files are updated, but runtime application FAILED: " + "; ".join(failures) + ". Retry terminus-settings apply after resolving the errors.")


def login_apply(root, isolated):
    if isolated:
        raise SettingsError("login-apply is forbidden with --root; isolated mode never changes login or live services")
    helper = PROJECT / "bin/terminus-login"
    if not helper.is_file():
        raise SettingsError(f"Authenticated login helper is missing: {helper}")
    try:
        start = Path("/proc/self/stat").read_text().rsplit(")", 1)[1].split()[19]
    except (OSError, IndexError) as exc:
        raise SettingsError(f"Cannot identify login request owner; no login write requested: {exc}") from exc
    try:
        proc = subprocess.run(
            [str(helper), "--root", str(root), "--request-pid", str(os.getpid()), "--request-start", start, "apply"],
            capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired as exc:
        raise SettingsError("Authenticated login application timed out; outcome UNKNOWN. The helper may have applied appearance before cancellation; inspect the login configuration before retrying.") from exc
    except OSError as exc:
        raise SettingsError(f"Authenticated login application communication failed; outcome UNKNOWN: {exc}") from exc
    if proc.stderr:
        print(proc.stderr[:16000], file=sys.stderr, end="" if proc.stderr.endswith("\n") else "\n")
    if proc.returncode:
        detail = (proc.stdout or proc.stderr)[:4000].strip() or "outcome UNKNOWN; no helper confirmation received"
        raise SettingsError(f"Login helper failed (exit {proc.returncode}): {detail}")
    return {"status": "applied", "login": "appearance only; existing authentication and session mechanism retained", "message": proc.stdout[:16000].strip()}


# Command line

def cli(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, epilog="set/reset save and apply; render/validate are offline. Login appearance is applied only by authenticated login-apply. Diagnostics are on stderr; successful data is canonical JSON on stdout.")
    parser.add_argument("--root", type=Path, help="isolated configuration root; never applies to live applications/services")
    sub = parser.add_subparsers(dest="command", required=True)
    get = sub.add_parser("get", help="read the canonical object or one dotted key as JSON")
    get.add_argument("key", nargs="?")
    sub.add_parser("schema", help="list typed controls, defaults, bounds and choices as JSON")
    sub.add_parser("validate", help="validate source and every adapter render without writing")
    sub.add_parser("render", help="write validated generated files only; never reload applications")
    set_parser = sub.add_parser("set", help="validate, save one or more JSON values, then render and apply once")
    set_parser.add_argument("pairs", nargs="+", metavar="KEY VALUE", help='dotted key and JSON value, repeatable; quote strings, for example appearance.accent \'"#E15588"\'')
    reset = sub.add_parser("reset", help="reset exactly one dotted key to its declared default")
    reset.add_argument("key")
    apply = sub.add_parser("apply", help="render and apply current settings; does not apply login")
    apply.add_argument("--dry-run", action="store_true", help="validate all renders; no writes or runtime changes")
    sub.add_parser("login-apply", help="explicitly apply staged login appearance using authenticated privilege escalation")
    args = parser.parse_args(argv)
    native_root = tokens.LIVE_ROOT
    isolated = args.root is not None
    root = args.root.expanduser().resolve() if isolated else native_root
    try:
        if isolated and (root.is_relative_to(native_root) or native_root.is_relative_to(root)):
            raise SettingsError("--root must be disjoint from the live Terminus-Est configuration tree")
        if args.command == "schema":
            result = SCHEMA
        elif args.command == "get":
            with opened_directory(root, missing_ok=True) as root_fd:
                result, _ = load(root, root_fd)
            if args.key:
                if args.key not in CONTROLS:
                    raise SettingsError(f"Unknown settings key: {args.key}")
                section, key = args.key.split(".")
                result = result[section][key]
        else:
            modules = adapters()
            readonly = args.command == "validate" or (args.command == "apply" and args.dry_run)
            with opened_directory(root, missing_ok=True) if readonly else locked(root) as root_fd:
                settings, source = load(root, root_fd)
                if args.command in ("set", "reset"):
                    if args.command == "reset":
                        if args.key not in CONTROLS:
                            raise SettingsError(f"Unknown settings key: {args.key}")
                        updates = [(args.key, CONTROLS[args.key]["default"])]
                    else:
                        if len(args.pairs) % 2:
                            raise SettingsError("set expects KEY VALUE pairs")
                        updates = []
                        for name, raw in zip(args.pairs[::2], args.pairs[1::2]):
                            if name not in CONTROLS:
                                raise SettingsError(f"Unknown settings key: {name}")
                            if len(raw) > 4096:
                                raise SettingsError("JSON value exceeds size limit")
                            try:
                                updates.append((name, json.loads(raw, parse_constant=lambda value: (_ for _ in ()).throw(ValueError(f"nonfinite JSON value: {value}")))))
                            except ValueError as exc:
                                raise SettingsError(f"Invalid JSON value for {name}: {exc}") from exc
                    for name, value in updates:
                        value = validate_value(name, value)
                        section, key = name.split(".")
                        settings[section][key] = value
                        source = update_source(source, name, value, settings)
                plan = render_plan(settings, root, modules, root_fd)
                if args.command == "login-apply":
                    plan = {path: content for path, content in plan.items() if path == root / "login/appearance.json"}
                    if not plan:
                        raise SettingsError("Session adapter did not stage the required login appearance file")
                if args.command in ("set", "reset"):
                    # Source is last: failed output writes never publish new settings.
                    plan[root / "settings.toml"] = source
                if readonly:
                    result = {"valid": True, "dry_run": True, "files": [str(path.relative_to(root)) for path in plan]}
                else:
                    if args.command == "login-apply" and isolated:
                        raise SettingsError("login-apply is forbidden with --root")
                    changed = commit(root, plan, root_fd)
                    if args.command == "login-apply":
                        result = login_apply(root, isolated)
                    else:
                        if args.command != "render":
                            if isolated:
                                print("Isolated root: settings saved/rendered only; no live applications or services changed.", file=sys.stderr)
                            else:
                                keys = {name for name, _ in updates} if args.command in ("set", "reset") else None
                                apply_runtime(settings, root, set(plan) if args.command == "apply" else changed, modules, keys)
                        result = settings
        print(json.dumps(result, sort_keys=True, ensure_ascii=False, allow_nan=False))
        return 0
    except (SettingsError, OSError, ValueError) as exc:
        print(f"terminus-settings: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(cli())
