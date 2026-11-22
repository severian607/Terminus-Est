#!/usr/bin/env python3
"""Print every live Hyprland bind that uses SUPER as JSON for the keyboard overlay.

`hyprctl binds -j` is malformed on Hyprland 0.56 (Lua provider), so the readable output is parsed instead.
Output: [{"mods": ["SHIFT", ...], "key": "T", "desc": "...", "short": "..."}], keys upper-case and normalised
(code:10 -> "1", mouse:272 -> "MOUSE_L", ...).
"""

import json
import subprocess
import sys

SHIFT, CTRL, ALT, SUPER = 1, 4, 8, 64

CODES = {
    **{str(10 + i): str((i + 1) % 10) for i in range(10)},
    "20": "MINUS",
    "21": "EQUAL",
    "61": "SLASH",
    "201": "COPILOT",
}
ALIASES = {
    "mouse:272": "MOUSE_L",
    "mouse:273": "MOUSE_R",
    "mouse:274": "MOUSE_M",
    "mouse_up": "WHEEL_UP",
    "mouse_down": "WHEEL_DOWN",
}

SHORT = {
    "Toggle LSD screen mode": "LSD mode",
    "Toggle CRT screen mode": "CRT mode",
    "Toggle invert screen mode": "Invert mode",
    "Toggle glitch screen mode": "Glitch mode",
    "Toggle Matrix screen mode": "Matrix mode",
    "Toggle wave screen mode": "Wave mode",
    "Toggle cyberpunk screen mode": "Cyberpunk mode",
    "Audio controls": "Audio",
    "Choose audio effects": "Audio FX",
    "Switch audio output": "Audio output",
    "Toggle single-window square aspect": "Square window",
    "Toggle window gaps": "Gaps",
    "Toggle window transparency": "Opacity",
    "Bluetooth controls": "Bluetooth",
    "Wifi controls": "Wi-Fi",
    "Browser (private)": "Private browser",
    "Choose keyboard and mouse sound pack": "Sound pack",
    "Show battery remaining": "Battery",
    "Dismiss all notifications": "Dismiss all",
    "Dismiss last notification": "Dismiss last",
    "Invoke last notification": "Invoke last",
    "Restore last notification": "Restore last",
    "Toggle silencing notifications": "Silence notifs",
    "Capture menu": "Capture",
    "Universal copy": "Copy",
    "Universal cut": "Cut",
    "Focus on below window": "Focus down",
    "Focus on above window": "Focus up",
    "Focus on left window": "Focus left",
    "Focus on right window": "Focus right",
    "Move window to group on bottom": "To group \u2193",
    "Move window to group on top": "To group \u2191",
    "Move window to group on left": "To group \u2190",
    "Move window to group on right": "To group \u2192",
    "Move workspace to down monitor": "Ws to monitor \u2193",
    "Move workspace to up monitor": "Ws to monitor \u2191",
    "Move workspace to left monitor": "Ws to monitor \u2190",
    "Move workspace to right monitor": "Ws to monitor \u2192",
    "Swap window down": "Swap \u2193",
    "Swap window up": "Swap \u2191",
    "Move window left": "Move \u2190",
    "Move window right": "Move \u2192",
    "Move grouped window focus left": "Group focus \u2190",
    "Move grouped window focus right": "Group focus \u2192",
    "Toggle laptop display": "Laptop screen",
    "Toggle laptop display mirroring": "Mirror laptop",
    "Choose screen shader": "Shaders",
    "Toggle Terminus-Bar menu on the main monitor": "Control panel",
    "Emoji picker (Terminus-Bar launcher)": "Emoji",
    "File manager": "Files (yazi)",
    "Full screen": "Fullscreen",
    "Full width": "Maximize",
    "Strata (cwd)": "Strata",
    "Tiled full screen": "Tiled full",
    "Google Messages": "Messages",
    "Google Photos": "Photos",
    "Move active window out of group": "Leave group",
    "Toggle window grouping": "Group",
    "Choose animations and effects": "Motion",
    "Hardware menu": "Hardware",
    "Insert Unicode symbol": "Symbols",
    "Toggle locking on idle": "Idle lock",
    "Toggle window split": "Split",
    "Pick screen color": "Pick color",
    "Mood sounds play/pause": "Mood sounds",
    "Toggle nightlight": "Night light",
    "Pop window out (float & pin)": "Pop out",
    "Toggle menu": "Menu",
    "Toggle Obsidian light/dark": "Obsidian theme",
    "Extract text (OCR) from screenshot": "OCR",
    "Pseudo window": "Pseudo",
    "Screensaver": "Saver",
    "Previous workspace": "Prev ws",
    "Open OMP": "OMP",
    "Set reminder": "Remind",
    "Skwd-wall": "Walls",
    "Toggle Terminus-Bar bar": "Toggle bar",
    "Toggle launcher on the main monitor": "Launcher",
    "Wallpaper panel on the main monitor": "Wallpaper panel",
    "Former workspace": "Last ws",
    "Next window in group": "Next in group",
    "Previous window in group": "Prev in group",
    "Next workspace on the main monitor": "Next ws",
    "Toggle window floating/tiling": "Float / tile",
    "Clipboard manager": "Clipboard",
    "Toggle Hyprwhspr recording": "Dictate",
    "Close window": "Close",
    "Show weather": "Weather",
    "X Post": "Post on X",
    "Scroll active workspace forward": "Next ws",
    "Scroll active workspace backward": "Prev ws",
    "Toggle workspace layout": "Ws layout",
    "Cycle monitor scaling": "Scale",
    "Cycle monitor scaling backwards": "Scale back",
    "Expand window left": "Grow \u2190",
    "Shrink window left": "Shrink \u2190",
    "Expand window down": "Grow \u2193",
    "Shrink window up": "Shrink \u2191",
    "Show SUPER keybindings on a keyboard": "Keys",
}
# Numbered descriptions: (description prefix, keycap prefix); the number follows both.
SHORT_NUMBERED = (
    ("Switch to workspace ", "Ws "),
    ("Move window silently to workspace ", "Send quietly "),
    ("Move window to workspace ", "Send to ws "),
    ("Switch to group window ", "Group tab "),
)


def shorten(desc):
    """Keycap text: a few words; the full description stays in the readout."""
    if desc in SHORT:
        return SHORT[desc]
    for long_prefix, short_prefix in SHORT_NUMBERED:
        number = desc.removeprefix(long_prefix)
        if number != desc and number.isdecimal():
            return short_prefix + number
    return desc


def normalise(key):
    key = key.strip()
    if " + " in key:  # Lua keycode binds print "SUPER + SHIFT + code:10"
        key = key.rsplit(" + ", 1)[1]
    if key.startswith("code:"):
        return CODES.get(key[5:], key)
    return ALIASES.get(key, key.upper())


def parse(text):
    """The records of `hyprctl binds`: one dict of `name: value` lines per `bind` header."""
    binds, current = [], None
    for line in text.splitlines():
        if line.startswith("bind"):
            current = {}
            binds.append(current)
            continue
        name, separator, value = line.strip().partition(":")
        if current is not None and separator:
            current[name] = value.strip()
    return binds


def super_binds(binds):
    """The overlay's entries: global binds that use SUPER, with keys normalised and a keycap text."""
    for bind in binds:
        if bind.get("submap"):
            continue
        try:
            mask = int(bind.get("modmask", "0"))
        except ValueError:
            continue
        if not mask & SUPER or bind.get("key", "").startswith("switch:"):
            continue
        desc = bind.get("description", "")
        if not desc or desc == "null":
            desc = " ".join(
                part for part in (bind.get("dispatcher", ""), bind.get("arg", "")) if part and part != "null"
            )
        mods = [name for name, bit in (("SHIFT", SHIFT), ("CTRL", CTRL), ("ALT", ALT)) if mask & bit]
        yield {"mods": mods, "key": normalise(bind["key"]), "desc": desc, "short": shorten(desc)}


def main():
    text = subprocess.run(["hyprctl", "binds"], capture_output=True, text=True, check=True).stdout
    json.dump(list(super_binds(parse(text))), sys.stdout)


if __name__ == "__main__":
    main()
