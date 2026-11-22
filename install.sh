#!/usr/bin/env bash
# Terminus-Est installer for Arch Linux.
#
#   curl -fsSL https://raw.githubusercontent.com/severian607/Terminus-Est/main/install.sh | bash
#   curl -fsSL .../install.sh | bash -s -- --no-packages     skip pacman and AUR
#
# Installs the packages, clones the repository to ~/Projects/Terminus-Est (units and bindings expect that path),
# links the configuration into place, builds the bar and the Hyprland plugins, renders the skins and enables
# the user units. Anything already at a target path that is not a link is moved to
# ~/.local/state/terminus-est/install-backup/<time>/ first. Running it again updates and relinks.
set -euo pipefail

main() {
  local repo_url=https://github.com/severian607/Terminus-Est.git
  local dir=$HOME/Projects/Terminus-Est
  local backup=$HOME/.local/state/terminus-est/install-backup/$(date +%Y%m%d-%H%M%S)
  local packages=1
  for arg in "$@"; do
    case $arg in
      --no-packages) packages=0 ;;
      *) die "unknown option $arg" ;;
    esac
  done

  [[ $EUID -ne 0 ]] || die "run as your normal user; sudo is asked for when needed"
  [[ -f /etc/arch-release ]] || die "Terminus-Est needs Arch Linux"

  if (( packages )); then
    step "packages"
    local packages_wanted=("${PACKAGES[@]}")
    # rustup provides cargo too; asking pacman for rust there would conflict and abort.
    command -v cargo >/dev/null || packages_wanted+=(rust)
    sudo pacman -S --needed "${packages_wanted[@]}" </dev/tty
    local aur
    aur=$(command -v paru || command -v yay || true)
    if [[ -n $aur ]]; then
      "$aur" -S --needed "${AUR_PACKAGES[@]}" </dev/tty || say "optional AUR packages failed; the rest works without them"
    else
      say "no AUR helper (paru, yay); skipped the optional ${AUR_PACKAGES[*]}"
    fi
  fi

  step "launcher font"
  # No grep -q: it would exit at the first match and SIGPIPE fc-list, which pipefail reports as failure.
  if fc-list : family | grep "Consoleet Terminus" >/dev/null; then
    say "Consoleet Terminus already installed"
  else
    install_font
  fi

  step "repository"
  if [[ -d $dir/.git ]]; then
    git -C "$dir" pull --ff-only --autostash
  elif [[ -e $dir ]]; then
    die "$dir exists and is not a git checkout; move it away first"
  else
    mkdir -p "$(dirname "$dir")"
    git clone "$repo_url" "$dir"
  fi

  step "links"
  link "$dir/config" "$HOME/.config/terminus-est"
  local file
  for file in "$dir"/bin/terminus*; do link "$file" "$HOME/.local/bin/$(basename "$file")"; done
  link "$dir/modules/desktop" "$HOME/.local/share/terminus-est"
  for file in hyprland.lua hyprsunset.conf xdph.conf; do link "$dir/config/hyprland/$file" "$HOME/.config/hypr/$file"; done
  link "$dir/config/generated/hypridle.conf" "$HOME/.config/hypr/hypridle.conf"
  for file in env default; do link "$dir/config/uwsm/$file" "$HOME/.config/uwsm/$file"; done
  for file in hidden-apps.json launcher.json scale.json theme.json; do
    link "$dir/config/terminus-launcher/$file" "$HOME/.config/terminus-launcher/$file"
  done
  link "$dir/config/generated/mako.ini" "$HOME/.config/mako/config"
  link "$dir/config/zed" "$HOME/.config/zed"
  link "$dir/config/swayosd" "$HOME/.config/swayosd"
  link "$dir/wallpapers" "$HOME/Pictures/Wallpapers/Terminus Est"
  for file in "$dir"/config/systemd/user/terminus-* "$dir"/config/systemd/user/swayosd-server.service; do
    link "$file" "$HOME/.config/systemd/user/$(basename "$file")"
  done
  for file in "$dir"/config/dbus-1/services/*; do link "$file" "$HOME/.local/share/dbus-1/services/$(basename "$file")"; done
  for file in "$dir"/modules/terminus-bar-plugins/*/; do
    file=${file%/}
    link "$file" "$HOME/.local/share/terminusbar/plugins/$(basename "$file")"
  done

  step "build and render (the bar takes several minutes the first time)"
  PATH="$dir/bin:$HOME/.local/bin:$PATH" "$dir/bin/terminus-update"
  [[ -x $dir/modules/terminus-bar/build-release/terminusbar ]] ||
    die "the bar did not build; see: ninja -C $dir/modules/terminus-bar/build-release terminusbar"

  # A new machine starts on the bar design from the screenshots; an existing bar state is left alone.
  local bar_state=$HOME/.local/state/terminusbar/settings.toml
  if [[ ! -e $bar_state ]]; then
    mkdir -p "$(dirname "$bar_state")"
    : > "$bar_state"
    "$dir/bin/terminus-bar-style" set tmux || say "could not apply the bar preset; run: terminus-bar-style set tmux"
  fi

  step "user units"
  systemctl --user daemon-reload
  systemctl --user enable "${UNITS[@]}"
  # The wallpaper daemon comes with the optional Skwd Wall.
  systemctl --user enable skwd-walld.service 2>/dev/null || say "skwd-walld is not installed; wallpapers need Skwd Wall (AUR)"
  if [[ -n ${WAYLAND_DISPLAY:-} ]]; then
    systemctl --user restart terminus-bar.service terminus-launcher.service || true
  fi

  step "done"
  [[ -d $backup ]] && say "replaced files are in $backup"
  cat <<EOF
Next:
  - Log in to the "Hyprland (uwsm-managed)" session.
  - Set your outputs in $dir/config/hyprland/monitors.lua, or per machine in
    $dir/config/hosts/\$(cat /etc/hostname)/hyprland/monitors.lua.
  - Turn on the bar's vitals plugin once it runs:  terminus-bar msg plugins enable terminus/vitals
  - Optional, update on every pacman upgrade:
      sed "s/@USER@/\$USER/g" $dir/config/pacman/terminus-est.hook | sudo install -Dm644 /dev/stdin /etc/pacman.d/hooks/terminus-est.hook
EOF
}

PACKAGES=(
  # build
  base-devel git meson ninja just pkgconf libvterm
  wayland wayland-protocols libglvnd freetype2 fontconfig cairo pango harfbuzz libxkbcommon glib2
  libsecret libsodium sdbus-cpp libpipewire wireplumber polkit pam curl libwebp libjxl libsndfile librsvg
  libqalculate libxml2 md4c tomlplusplus libical nlohmann-json stb jemalloc pixman
  # session
  hyprland uwsm xdg-desktop-portal-hyprland hypridle hyprlock hyprpolkitagent hyprsunset hyprpicker
  quickshell mako swayosd
  # tools and pickers
  python python-gobject gtk4 gtk4-layer-shell python-tomlkit python-evdev python-numpy
  mpv btop jq libnotify brightnessctl playerctl wl-clipboard grim slurp wf-recorder wtype socat
  # terminal, editor, fonts
  ghostty zed otf-monaspace-nerd ttf-jetbrains-mono-nerd
)
AUR_PACKAGES=(skwd-wall-v2 mechvibes python-sounddevice python-terminaltexteffects)
UNITS=(
  terminus-bar.service terminus-launcher.service terminus-audio.service terminus-motion.service
  terminus-shaders.service terminus-symbols.service terminus-keysounds.service terminus-vitals-log.service
  terminus-strata-skin.path terminus-skwd-colors.path
  swayosd-server.service hypridle.service hyprpolkitagent.service
)
FONT_URL=https://inai.de/files/consoleet/consoleet-terminus-4.49.1.tar.zst
FONT_SHA256=207fb58272584429b9b83dcd21706c94522cfaf83676adae523664a96a69f121

say() { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
die() { printf 'terminus-est install: %s\n' "$*" >&2; exit 1; }

# Point $2 at $1. A link is replaced; anything else is moved into the backup folder first.
link() {
  local source=$1 target=$2
  if [[ -L $target ]]; then
    [[ $(readlink "$target") == "$source" ]] && return
    rm "$target"
  elif [[ -e $target ]]; then
    mkdir -p "$backup$(dirname "$target")"
    mv "$target" "$backup$target"
    say "moved $target to the backup"
  fi
  mkdir -p "$(dirname "$target")"
  ln -s "$source" "$target"
}

install_font() {
  local tmp
  tmp=$(mktemp -d)
  curl -fsSL "$FONT_URL" -o "$tmp/font.tar.zst"
  echo "$FONT_SHA256  $tmp/font.tar.zst" | sha256sum --check --quiet || die "font download checksum mismatch"
  mkdir -p "$HOME/.local/share/fonts/ConsoleetTerminus"
  tar --zstd -xf "$tmp/font.tar.zst" -C "$HOME/.local/share/fonts/ConsoleetTerminus" --strip-components=1 \
    --wildcards '*.otf' '*/OFL.txt'
  rm -rf "$tmp"
  fc-cache -f "$HOME/.local/share/fonts" >/dev/null
  say "installed Consoleet Terminus (OFL) to ~/.local/share/fonts/ConsoleetTerminus"
}

main "$@"
