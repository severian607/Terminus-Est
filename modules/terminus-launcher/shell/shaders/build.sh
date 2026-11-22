#!/bin/sh
# Compiles the launcher's shaders to .qsb (Qt Shader Baker, package qt6-shadertools). The .qsb files are kept in the
# repository, so this only needs running after editing a .frag.
set -eu
dir="$(dirname "$(readlink -f "$0")")"
qsb="${QSB:-/usr/lib/qt6/bin/qsb}"
for f in "$dir"/*.frag; do
    "$qsb" --qt6 -o "$f.qsb" "$f"
done
