#!/bin/bash
# label: Crosshair
# emoji: ✛
# category: Cursor · Overlay
# slot: overlay-crosshair
# tags: crosshair, lines, dashed, find
# description: Thin dashed lines run across the monitor through the pointer, horizontal and vertical.
# source: original
# license: original
# demo: overlay

# Runs a Quickshell overlay (modules/overlays/pointerfx/crosshair.qml) in the foreground. Click-through,
# no keyboard focus. Takes the accent colour from config/generated/appearance.lua at start.

set -euo pipefail

root="$(cd "$(dirname "$(readlink -f "$0")")/../../.." && pwd)"
dir="$root/modules/overlays/pointerfx"

if ! command -v qs >/dev/null || ! command -v python3 >/dev/null || [[ ! -f $dir/crosshair.qml ]]; then
  notify-send -u critical "Motion: crosshair" "Quickshell, python3 or crosshair.qml is missing."
  exit 1
fi

accent="$(sed -n 's/^ *accent = "\(#[0-9a-fA-F]\{6\}\)".*/\1/p' "$root/config/generated/appearance.lua" | head -n 1)"

export POINTERFX_DIR="$dir"
export POINTERFX_ACCENT="${accent:-#ffcccd}"
exec qs -p "$dir/crosshair.qml"
