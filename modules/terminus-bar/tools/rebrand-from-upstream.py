#!/usr/bin/env python3
"""Mechanical noctalia -> TerminusBar rebrand. Run at the root of a pristine upstream checkout.

Renames a fixed set of paths, then applies ordered replacement rules to every
text file (binary files are skipped). Deterministic; no other inputs.
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
            if text.encode("utf-8") != data:
                path.write_bytes(text.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
