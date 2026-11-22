#!/bin/bash
# label: Lightning trail
# emoji: ⚡
# category: Cursor · Overlay
# slot: overlay-trail
# tags: lightning, electric, bolt, spark, shader, glsl, trail, pointer, overlay
# description: A jittering white-hot electric bolt with a blue glow and pulses running along the pointer's path.
# source: original
# license: original
# demo: overlay

# Runs modules/overlays/pointer-gl in the foreground (the engine stops it with SIGTERM): a GLSL fragment shader
# draws the effect along the pointer's recent path. The colour is the theme accent from
# config/generated/appearance.lua, read when the effect starts (only the plasma effect uses it).

set -euo pipefail

project="$(dirname "$(dirname "$(dirname "$(dirname "$(readlink -f "$0")")")")")"
binary="$project/modules/overlays/pointer-gl/build/pointer-gl"

if [[ ! -x $binary ]]; then
  notify-send -u critical "Overlay effect unavailable" "Build it first: make -C $project/modules/overlays/pointer-gl"
  exit 1
fi

accent=$(sed -n 's/^[[:space:]]*accent = "\(#[0-9a-fA-F]\{6\}\)".*/\1/p' "$project/config/generated/appearance.lua" 2>/dev/null | head -n1 || true)

exec "$binary" lightning --color "${accent:-#ffcccd}"
