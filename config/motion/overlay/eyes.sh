#!/bin/bash
# label: Googly eyes
# emoji: 👀
# category: Cursor · Overlay
# slot: overlay-eyes
# tags: eyes, googly, xeyes, fun
# description: Two googly eyes sit in the bottom right corner and their pupils follow the pointer.
# source: original
# license: original
# demo: overlay

# Runs a Quickshell overlay (modules/overlays/pointerfx/eyes.qml) in the foreground. Click-through,
# no keyboard focus. Takes the accent colour from config/generated/appearance.lua at start.

set -euo pipefail

root="$(cd "$(dirname "$(readlink -f "$0")")/../../.." && pwd)"
dir="$root/modules/overlays/pointerfx"

if ! command -v qs >/dev/null || ! command -v python3 >/dev/null || [[ ! -f $dir/eyes.qml ]]; then
  notify-send -u critical "Motion: eyes" "Quickshell, python3 or eyes.qml is missing."
  exit 1
fi

accent=$(sed -n 's/^ *accent = "\(#[0-9a-fA-F]\{6\}\)".*/\1/p' "$root/config/generated/appearance.lua" 2>/dev/null | head -n1 || true)

export POINTERFX_DIR="$dir"
export POINTERFX_ACCENT="${accent:-#ffcccd}"
exec qs -p "$dir/eyes.qml"
