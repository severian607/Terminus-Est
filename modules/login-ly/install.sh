#!/bin/bash
# Install the Terminus-Est Ly build (1.4.1-1.2) and switch the login screen to it.
# Run as your normal user; sudo is asked for the root steps. Nothing is restarted:
# the new login screen appears the next time ly-kmsconvt@tty2 starts (next boot or logout).
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
pkg="$here/ly-1.4.1-1.2-x86_64.pkg.tar.zst"
config=/etc/ly/config.ini
rollback_pkg=${LY_ROLLBACK_PKG:-"$HOME/Projects/Sakura Mochi Login/ly-eintr-fix/ly-1.4.1-1.1-x86_64.pkg.tar.zst"}

if [[ ! -f "$pkg" ]]; then
  echo "missing $pkg" >&2
  echo "build it first:  cd '$here' && ZIG=\$HOME/.cache/terminus-est/zig-x86_64-linux-0.16.0/zig makepkg -f" >&2
  exit 1
fi

stamp=$(date +%Y%m%d-%H%M%S)
backup="$config.bak-$stamp"

echo "==> backing up $config to $backup"
sudo cp -a "$config" "$backup"

echo "==> installing $pkg"
sudo pacman -U --noconfirm "$pkg"

# Set KEY = VALUE in config.ini: replace an existing line, otherwise append. Idempotent.
set_key() {
  local key=$1 value=$2
  if sudo grep -qE "^${key}[[:space:]]*=" "$config"; then
    sudo sed -i -E "s|^${key}[[:space:]]*=.*|${key} = ${value}|" "$config"
  else
    echo "${key} = ${value}" | sudo tee -a "$config" >/dev/null
  fi
}

echo "==> setting keys in $config"
set_key password_style constellation
set_key animation terminus
set_key border_fg 0x0096574E

echo
echo "Done. The new login screen shows up the next time ly-kmsconvt@tty2 starts"
echo "(next boot, or after logging out). Nothing was restarted."
echo
echo "Roll back:"
echo "  sudo pacman -U '$rollback_pkg'"
echo "  sudo cp -a '$backup' $config"
echo "Broken login screen? Ctrl+Alt+F3, log in on the console, run the two commands above."
