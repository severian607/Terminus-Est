"""Motion engine: window animation presets, window effects, pointer effects and terminal cursor shaders.

Every entry is one file under config/motion/<group>/ with a comment header (label, emoji, category, slot, ...).
Entries that share a slot replace each other. Lua entries run at the end of config/hyprland/animations.lua;
GLSL entries are Ghostty custom shaders. The active set is one small JSON file; the Lua list and the Ghostty
include are generated from it.
"""

import json
import os
import re
import signal
import subprocess
from pathlib import Path

CATEGORIES = [
    "Windows · Pop",
    "Windows · Slide",
    "Windows · Bounce",
    "Windows · Calm",
    "Windows · Fast",
    "Windows · Showy",
    "Window effects",
    "Cursor · Theme",
    "Cursor · Pointer",
    "Cursor · Terminal",
    "Cursor · Overlay",
    "Text · Zed",
]
# Category prefixes the panel shows as one parent row with the "Prefix · Name" categories nested under it.
GROUPS = ["Windows", "Cursor"]
# A slot listed here always holds an entry; clearing it puts the default back.
DEFAULTS = {"windows": "windows/pop", "cursor-theme": "cursor-theme/sword"}
# What the preview recorder does to show an entry, when its header has no `demo:` line.
GROUP_DEMO = {
    "windows": "windows",
    "effects": "windows",
    "pointer": "pointer",
    "terminal-cursor": "typing",
    "overlay": "overlay",
}
ENTRY_SUFFIXES = (".lua", ".glsl", ".jsonc", ".sh", ".cursor")
# Properties of the transient systemd unit that runs an overlay entry (`.sh`).
OVERLAY_UNIT_PROPERTIES = (
    "--property=Restart=on-failure",
    "--property=RestartSec=2",
    "--property=KillSignal=SIGTERM",
    "--property=TimeoutStopSec=3",
)

PROJECT = Path(__file__).resolve().parents[1]
MOTION_DIR = PROJECT / "config/motion"
CURSOR_TOOL = PROJECT / "modules/terminus-cursor/terminus-cursor"
STATE_HOME = Path.home() / ".local/state/terminus-est"
ZED_SETTINGS = Path.home() / ".config/zed/settings.json"
ACTIVE_FILE = STATE_HOME / "motion/active.json"
CURSOR_STATE = STATE_HOME / "cursor/theme"
GHOSTTY_FILE = STATE_HOME / "motion/ghostty.conf"
LUA_FILE = STATE_HOME / "toggles/hypr/motion.lua"
PREVIEW_DIR = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "terminus-motion/previews"

HEADER_RE = re.compile(r"^(?:--|//|#)\s*([a-z]+)\s*:\s*(.*?)\s*$")


class MotionError(Exception):
    pass


class Entry:
    def __init__(self, path):
        self.path = path
        self.group = path.parent.name
        self.id = f"{self.group}/{path.stem}"
        self.kind = path.suffix[1:]
        text = path.read_text()
        self.text = text
        meta = {}
        for line in text.splitlines():
            if line.startswith("#!"):
                continue
            match = HEADER_RE.match(line)
            if not match:
                break
            meta.setdefault(match.group(1), match.group(2))
        self.label = meta.get("label") or path.stem
        self.emoji = meta.get("emoji", "")
        self.category = meta.get("category", "")
        self.slot = meta.get("slot") or self.group
        self.tags = [tag.strip().lower() for tag in meta.get("tags", "").split(",") if tag.strip()]
        self.description = meta.get("description", "")
        self.source = meta.get("source", "original")
        self.license = meta.get("license", "original")
        self.demo = meta.get("demo") or GROUP_DEMO.get(self.group, "windows")
        self.plugin = "hl.plugin.load" in text

    @property
    def preview(self):
        return PREVIEW_DIR / f"{self.id}.mp4"


def load_entries():
    """All entries by id, in category order then by label; the default of a slot leads its category."""
    entries = [Entry(path) for path in MOTION_DIR.glob("*/*") if path.suffix in ENTRY_SUFFIXES]
    order = {name: index for index, name in enumerate(CATEGORIES)}
    entries.sort(
        key=lambda entry: (
            order.get(entry.category, len(order)),
            entry.id not in DEFAULTS.values(),
            entry.label.lower(),
        )
    )
    return {entry.id: entry for entry in entries}


def resolve(name, entries):
    """Accept a full id (windows/pop) or a bare name that is unique across groups."""
    if name in entries:
        return entries[name]
    matches = [entry for entry in entries.values() if entry.path.stem == name]
    if len(matches) == 1:
        return matches[0]
    raise MotionError(
        f"ambiguous entry: {name} ({', '.join(entry.id for entry in matches)})" if matches else f"unknown entry: {name}"
    )


def read_active(entries):
    """slot -> entry id, dropping entries that no longer exist and filling the defaults."""
    try:
        saved = json.loads(ACTIVE_FILE.read_text())
    except (OSError, ValueError):
        saved = {}
    active = {
        slot: name
        for slot, name in saved.items()
        if isinstance(name, str) and name in entries and entries[name].slot == slot
    }
    for slot, name in DEFAULTS.items():
        if slot not in active and name in entries:
            active[slot] = name
    return active


def change(active, command, names, entries):
    """The active set after `set`, `toggle` or `remove` of the named entries."""
    active = dict(active)
    for entry in (resolve(name, entries) for name in names):
        on = active.get(entry.slot) == entry.id
        if command == "set" or (command == "toggle" and not on):
            active[entry.slot] = entry.id
        elif on:
            del active[entry.slot]
    for slot, name in DEFAULTS.items():
        active.setdefault(slot, name)
    return active


def ordered(active, entries):
    """Active entries in list order (window preset first)."""
    return [entry for entry in entries.values() if active.get(entry.slot) == entry.id]


def write_if_changed(path, text):
    """Replace `path` atomically unless it already holds `text`; True when it was written."""
    try:
        if path.read_text() == text:
            return False
    except OSError:
        pass  # missing or unreadable: write it
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(text)
    temporary.replace(path)
    return True


def apply_zed(entry, on):
    """Write a .jsonc entry's `on` or `off` settings into Zed's settings.json; Zed reloads the file itself.

    The file has comments, so top-level one-line keys are replaced or inserted as text.
    """
    body = "\n".join(line for line in entry.text.splitlines() if not line.startswith("//"))
    original = text = ZED_SETTINGS.read_text()
    for key, value in json.loads(body)["on" if on else "off"].items():
        line = f"  {json.dumps(key)}: {json.dumps(value)},"
        existing = re.search(rf"^  {re.escape(json.dumps(key))}:.*$", text, re.M)
        if existing:
            text = text[: existing.start()] + line + text[existing.end() :]
        else:
            text = text.replace("{\n", "{\n" + line + "\n", 1)
    if text != original:
        ZED_SETTINGS.write_text(text)  # in place, so the file keeps its 0600 mode


def unit_name(entry):
    return f"terminus-motion-{entry.path.stem}.service"


def sync_overlays(active, entries):
    """Run the active overlay entries as transient user units and stop the others."""
    for entry in entries.values():
        if entry.kind != "sh":
            continue
        unit = unit_name(entry)
        running = subprocess.run(["systemctl", "--user", "is-active", "--quiet", unit]).returncode == 0
        if active.get(entry.slot) == entry.id:
            if not running:
                subprocess.run(
                    [
                        "systemd-run",
                        "--user",
                        "--collect",
                        "--quiet",
                        f"--unit={unit.removesuffix('.service')}",
                        *OVERLAY_UNIT_PROPERTIES,
                        "bash",
                        str(entry.path),
                    ],
                    check=False,
                )
        elif running:
            subprocess.run(["systemctl", "--user", "stop", unit], check=False)


def reload_ghostty():
    """Ask the user's running Ghostty processes to reload their configuration (SIGUSR2)."""
    for proc in Path("/proc").glob("[0-9]*"):
        try:
            if proc.stat().st_uid == os.getuid() and (proc / "comm").read_text().strip() == "ghostty":
                os.kill(int(proc.name), signal.SIGUSR2)
        except (OSError, ValueError):
            continue  # the process exited while being inspected


def apply(active, entries):
    """Persist the active set and bring Hyprland and Ghostty in line with it."""
    chosen = ordered(active, entries)
    write_if_changed(ACTIVE_FILE, json.dumps(active, indent=2, sort_keys=True) + "\n")
    lua = "return {\n" + "".join(f'  "{entry.path}",\n' for entry in chosen if entry.kind == "lua") + "}\n"
    if write_if_changed(LUA_FILE, lua):
        result = subprocess.run(["hyprctl", "reload"], capture_output=True, text=True)
        if result.returncode or result.stdout.strip() != "ok":
            raise MotionError(f"hyprctl reload failed: {(result.stdout + result.stderr).strip()}")
    shader = next((entry for entry in chosen if entry.kind == "glsl"), None)
    conf = "# Generated by terminus-motionfx; the main Ghostty config includes this file.\n"
    if shader:
        conf += f"custom-shader = {shader.path}\ncustom-shader-animation = true\n"
    if write_if_changed(GHOSTTY_FILE, conf):
        reload_ghostty()
    for entry in chosen:
        if entry.kind == "cursor":
            apply_cursor(entry)
    sync_overlays(active, entries)
    for entry in entries.values():
        if entry.kind == "jsonc" and ZED_SETTINGS.exists():
            apply_zed(entry, active.get(entry.slot) == entry.id)


def apply_cursor(entry):
    """Switch the cursor theme (`theme =` and `size =` lines of a .cursor entry) unless it is the last choice."""
    theme = re.search(r"^theme\s*=\s*(\S+)", entry.text, re.M)
    size = re.search(r"^size\s*=\s*(\d+)", entry.text, re.M)
    if not (theme and size):
        raise MotionError(f"{entry.id}: needs `theme = NAME` and `size = N` lines")
    choice = f"{theme.group(1)} {size.group(1)}\n"
    try:
        if CURSOR_STATE.read_text() == choice:
            return
    except OSError:
        pass  # no recorded choice yet: switch
    # Detached: the first sword build takes a few seconds.
    subprocess.Popen(
        [str(CURSOR_TOOL), "use", theme.group(1), size.group(1)],
        start_new_session=True,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def list_entries(entries=None):
    entries = entries if entries is not None else load_entries()
    active = read_active(entries)
    positions = {entry.id: index + 1 for index, entry in enumerate(ordered(active, entries))}
    rows = []
    for entry in entries.values():
        preview = entry.preview
        rows.append(
            {
                "name": entry.id,
                "label": entry.label,
                "emoji": entry.emoji,
                "category": entry.category,
                "slot": entry.slot,
                "tags": entry.tags,
                "description": entry.description,
                "source": entry.source,
                "license": entry.license,
                "demo": entry.demo,
                "kind": entry.kind,
                "path": str(entry.path),
                "flags": [
                    flag for flag, on in (("plugin", entry.plugin), ("default", entry.id in DEFAULTS.values())) if on
                ],
                "active": entry.id in positions,
                "position": positions.get(entry.id),
                "shadowed": False,
                "preview": str(preview) if preview.exists() else "",
            }
        )
    categories = {row["category"] for row in rows}
    return {
        "categories": [category for category in CATEGORIES if category in categories],
        "groups": GROUPS,
        "effects": rows,
    }
