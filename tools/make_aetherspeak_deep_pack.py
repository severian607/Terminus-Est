#!/usr/bin/env python3
"""Build "Aetherspeak Deep": the Aetherspeak Mechvibes pack dropped into a deep-voice register, and install it.

usage: make_aetherspeak_deep_pack.py [--semitones 7] [--out DIR]
       default out: ~/.local/share/mechvibes/soundpacks/keyboard/aetherspeak-deep

Needs the stock Aetherspeak pack (mv-353e190917ea49aaa3d0cafbe6e67999) and sox. Each sample is pitched down with
sox `pitch` (duration unchanged, so the key timings carry over), given a low-shelf body boost and a gentle
treble roll-off, then peak-normalised.
"""

import argparse
import json
import subprocess
from pathlib import Path

KEYBOARD = Path.home() / ".local/share/mechvibes/soundpacks/keyboard"
SRC = KEYBOARD / "mv-353e190917ea49aaa3d0cafbe6e67999"

ap = argparse.ArgumentParser()
ap.add_argument("--semitones", type=float, default=7)
ap.add_argument("--out", type=Path, default=KEYBOARD / "aetherspeak-deep")
args = ap.parse_args()

args.out.mkdir(parents=True, exist_ok=True)
for src in sorted(SRC.glob("*.ogg")):
    subprocess.run(["sox", str(src), str(args.out / src.name), "gain", "-6", "pitch", str(-args.semitones * 100),
                    "bass", "+8", "120", "treble", "-4", "3500", "norm", "-1"], check=True)

config = json.loads((SRC / "config.json").read_text())
config.update(id="aetherspeak-deep", name="Aetherspeak Deep", author="Terminus-Est",
              description="Aetherspeak, pitched down into a deep voice.")
(args.out / "config.json").write_text(json.dumps(config, indent=2))
print(f"wrote {len(list(args.out.glob('*.ogg')))} samples to {args.out}")
