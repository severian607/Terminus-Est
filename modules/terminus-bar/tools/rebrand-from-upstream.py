#!/usr/bin/env python3
"""Mechanical noctalia -> TerminusBar rebrand. Run at the root of a pristine upstream checkout.

Renames a fixed set of paths, then applies ordered replacement rules to every
text file (binary files are skipped) and restores the strings that keep the
upstream name. Safe to run twice. Deterministic; no other inputs.
"""

import os
import re
import sys
from pathlib import Path

RENAMES = [
    ("noctalia.scm", "terminusbar.scm"),
    ("assets/noctalia-wallpaper.png", "assets/terminusbar-wallpaper.png"),
    ("assets/noctalia.svg", "assets/terminusbar.svg"),
    ("assets/dev.noctalia.Noctalia.desktop", "assets/dev.terminus.TerminusBar.desktop"),
    ("assets/fonts/noctalia-tabler.ttf", "assets/fonts/terminusbar-tabler.ttf"),
]

# The keyring identity, on-disk magic, upstream service hosts and organisation
# keep the upstream name (see the lookarounds below).
RULES = [
    # Identifiers and paths first (most specific).
    (r"dev\.noctalia\.Noctalia", "dev.terminus.TerminusBar"),
    (r"dev\.noctalia\.(?!Secret)", "dev.terminus.TerminusBar."),
    (r"/dev/noctalia/", "/dev/terminus/TerminusBar/"),
    (r"\.config/noctalia\b", ".config/terminus-est/terminus-bar"),
    # General case rules; upstream hosts, organisation and magic stay.
    (r"NOCTALIA(?!ENC1)", "TERMINUSBAR"),
    (r"Noctalia", "TerminusBar"),
    (r"noctalia(?!\.dev|-dev\b)", "terminusbar"),
]

# Exact strings that must keep the upstream name: the keyring schema and attributes (renaming them would orphan
# the stored secrets), the plugin-vote hash prefix and the OAuth client name. The general
# rules above cannot tell them apart, so they are restored afterwards.
KEEP = [
    ("dev.terminusbar.Secret", "dev.noctalia.Secret"),
    ('.application = "terminusbar"', '.application = "noctalia"'),
    ('application == "terminusbar"', 'application == "noctalia"'),
    ('"terminusbar-recommend-v1', '"noctalia-recommend-v1'),
    ('"client":"terminusbar-shell"', '"client":"noctalia-shell"'),
]

COMPILED = [(re.compile(p), r) for p, r in RULES]


def is_text(data: bytes) -> bool:
    if b"\0" in data:
        return False
    try:
        data.decode("utf-8")
    except UnicodeDecodeError:
        return False
    return True


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    for src, dst in RENAMES:
        # A second run finds the paths already renamed.
        if not (root / src).exists():
            continue
        (root / dst).parent.mkdir(parents=True, exist_ok=True)
        os.rename(root / src, root / dst)
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d != ".git"]
        for name in sorted(filenames):
            path = Path(dirpath) / name
            if path.is_symlink():
                continue
            data = path.read_bytes()
            if not is_text(data):
                continue
            text = data.decode("utf-8")
            for pattern, repl in COMPILED:
                text = pattern.sub(repl, text)
            for renamed, original in KEEP:
                text = text.replace(renamed, original)
            if text.encode("utf-8") != data:
                path.write_bytes(text.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
