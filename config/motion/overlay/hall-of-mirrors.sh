#!/bin/bash
# label: Hall of mirrors
# emoji: 🪞
# category: Cursor · Overlay
# slot: overlay-mirror
# tags: mirror, repeat, ghost, glitch, windows, retro, pointer, overlay
# description: Copies of your current pointer pile up behind it like the old Windows glitch and stay until you turn it off.
# source: original
# license: original
# demo: overlay

# Runs modules/overlays/pointer-fx (mirror mode) in the foreground (the engine stops it with SIGTERM). The copies show the
# pointer of the cursor theme in use at that moment (state file of terminus-cursor, else XCURSOR_THEME), and keep that
# look if the theme is changed later.

set -euo pipefail

project="$(dirname "$(dirname "$(dirname "$(dirname "$(readlink -f "$0")")")")")"
binary="$project/modules/overlays/pointer-fx/build/pointer-fx"

if [[ ! -x $binary ]]; then
  notify-send -u critical "Overlay effect unavailable" "Build it first: make -C $project/modules/overlays/pointer-fx"
  exit 1
fi

accent=$(sed -n 's/^[[:space:]]*accent = "\(#[0-9a-fA-F]\{6\}\)".*/\1/p' "$project/config/generated/appearance.lua" 2>/dev/null | head -n1 || true)

exec "$binary" mirror --color "${accent:-#ffcccd}"
