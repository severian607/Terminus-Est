#!/bin/bash
# label: Pointer trail
# emoji: ☄️
# category: Cursor · Overlay
# slot: overlay-trail
# tags: trail, glow, comet, pointer, overlay
# description: A glowing accent-coloured streak follows the pointer and fades out behind it.
# source: original
# license: original
# demo: overlay

# Runs modules/overlays/pointer-fx in the foreground (the engine stops it with SIGTERM). The colour is the theme
# accent from config/generated/appearance.lua, read when the effect starts.

set -euo pipefail

project="$(dirname "$(dirname "$(dirname "$(dirname "$(readlink -f "$0")")")")")"
binary="$project/modules/overlays/pointer-fx/build/pointer-fx"

if [[ ! -x $binary ]]; then
  notify-send -u critical "Overlay effect unavailable" "Build it first: make -C $project/modules/overlays/pointer-fx"
  exit 1
fi

accent=$(sed -n 's/^[[:space:]]*accent = "\(#[0-9a-fA-F]\{6\}\)".*/\1/p' "$project/config/generated/appearance.lua" 2>/dev/null | head -n1 || true)

exec "$binary" trail --color "${accent:-#ffcccd}"
