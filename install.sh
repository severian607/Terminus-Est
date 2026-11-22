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

REPO_URL=https://github.com/severian607/Terminus-Est.git
REPO_DIR=$HOME/Projects/Terminus-Est
BACKUP_DIR=$HOME/.local/state/terminus-est/install-backup/$(date +%Y%m%d-%H%M%S)

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
die() {
  printf 'terminus-est install: %s\n' "$*" >&2
  exit 1
}

# Point $2 at $1. A link is replaced; anything else is moved into the backup folder first.
link() {
  local source=$1 target=$2
  if [[ -L $target ]]; then
    [[ $(readlink "$target") == "$source" ]] && return
    rm "$target"
  elif [[ -e $target ]]; then
    mkdir -p "$BACKUP_DIR$(dirname "$target")"
    mv "$target" "$BACKUP_DIR$target"
    say "moved $target to the backup"
  fi
  mkdir -p "$(dirname "$target")"
  ln -s "$source" "$target"
}

# GTK and Ghostty keep their own configuration; one import or include line each makes them follow the palette.
# The lines are added once and the files are otherwise left as they are.
follow_palette() {
  local generated=$HOME/.config/terminus-est/generated file line
  line="@import url(\"file://$generated/gtk-colors.css\");"
  for file in "$HOME/.config/gtk-3.0/gtk.css" "$HOME/.config/gtk-4.0/gtk.css"; do
    mkdir -p "$(dirname "$file")"
    if ! grep -qsF "$generated/gtk-colors.css" "$file"; then
      # @import is only valid before every other rule, so it goes first.
      printf '%s\n%s\n' "$line" "$(cat "$file" 2>/dev/null)" >"$file.terminus-new"
      mv "$file.terminus-new" "$file"
      say "GTK follows the palette: $file"
    fi
  done
  file=$HOME/.config/ghostty/config
  mkdir -p "$(dirname "$file")"
  for line in \
    'config-file = ?"~/.config/terminus-est/generated/appearance-ghostty.conf"' \
    'config-file = ?"~/.local/state/terminus-est/motion/ghostty.conf"' \
    'config-file = ?"~/.config/terminus-est/generated/ghostty-mode.conf"'; do
    if ! grep -qsF "$line" "$file"; then
      printf '%s\n' "$line" >>"$file"
      say "Ghostty include added: ${line#config-file = }"
    fi
  done
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

# Steps, in the order main runs them.

install_packages() {
  step "packages"
  local packages_wanted=("${PACKAGES[@]}") aur
  # rustup provides cargo too; asking pacman for rust there would conflict and abort.
  command -v cargo >/dev/null || packages_wanted+=(rust)
  # The script arrives on stdin under curl | bash, so pacman and sudo prompts read the terminal instead.
  # shellcheck disable=SC2024  # /dev/tty is readable by the user; the redirect needs no root
  sudo pacman -S --needed "${packages_wanted[@]}" </dev/tty
  aur=$(command -v paru || command -v yay || true)
  if [[ -n $aur ]]; then
    "$aur" -S --needed "${AUR_PACKAGES[@]}" </dev/tty || say "optional AUR packages failed; the rest works without them"
  else
    say "no AUR helper (paru, yay); skipped the optional ${AUR_PACKAGES[*]}"
  fi
}

install_launcher_font() {
  step "launcher font"
  # No grep -q: it would exit at the first match and SIGPIPE fc-list, which pipefail reports as failure.
  if fc-list : family | grep "Consoleet Terminus" >/dev/null; then
    say "Consoleet Terminus already installed"
  else
    install_font
  fi
}

fetch_repository() {
  step "repository"
  if [[ -d $REPO_DIR/.git ]]; then
    git -C "$REPO_DIR" pull --ff-only --autostash
  elif [[ -e $REPO_DIR ]]; then
    die "$REPO_DIR exists and is not a git checkout; move it away first"
  else
    mkdir -p "$(dirname "$REPO_DIR")"
    git clone "$REPO_URL" "$REPO_DIR"
  fi
}

link_configuration() {
  local dir=$REPO_DIR file
  step "links"
  link "$dir/config" "$HOME/.config/terminus-est"
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
  follow_palette
  link "$dir/wallpapers" "$HOME/Pictures/Wallpapers/Terminus Est"
  for file in "$dir"/config/systemd/user/terminus-* "$dir"/config/systemd/user/swayosd-server.service; do
    link "$file" "$HOME/.config/systemd/user/$(basename "$file")"
  done
  for file in "$dir"/config/dbus-1/services/*; do link "$file" "$HOME/.local/share/dbus-1/services/$(basename "$file")"; done
  for file in "$dir"/modules/terminus-bar-plugins/*/; do
    file=${file%/}
    link "$file" "$HOME/.local/share/terminusbar/plugins/$(basename "$file")"
  done
}

build_and_render() {
  local dir=$REPO_DIR
  step "build and render (the bar takes several minutes the first time)"
  PATH="$dir/bin:$HOME/.local/bin:$PATH" "$dir/bin/terminus-update"
  [[ -x $dir/modules/terminus-bar/build-release/terminusbar ]] ||
    die "the bar did not build; see: ninja -C $dir/modules/terminus-bar/build-release terminusbar"

  # A new machine starts on the bar design from the screenshots; an existing bar state is left alone.
  local bar_state=$HOME/.local/state/terminusbar/settings.toml
  if [[ ! -e $bar_state ]]; then
    mkdir -p "$(dirname "$bar_state")"
    : >"$bar_state"
    "$dir/bin/terminus-bar-style" set tmux || say "could not apply the bar preset; run: terminus-bar-style set tmux"
  fi
}

enable_user_units() {
  step "user units"
  systemctl --user daemon-reload
  systemctl --user enable "${UNITS[@]}"
  # The wallpaper daemon comes with the optional Skwd Wall.
  systemctl --user enable skwd-walld.service 2>/dev/null || say "skwd-walld is not installed; wallpapers need Skwd Wall (AUR)"
  if [[ -n ${WAYLAND_DISPLAY:-} ]]; then
    systemctl --user restart terminus-bar.service terminus-launcher.service || true
  fi
}

print_next_steps() {
  local dir=$REPO_DIR
  step "done"
  [[ -d $BACKUP_DIR ]] && say "replaced files are in $BACKUP_DIR"
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

main() {
  local want_packages=1 arg
  for arg in "$@"; do
    case $arg in
      --no-packages) want_packages=0 ;;
      *) die "unknown option $arg" ;;
    esac
  done

  [[ $EUID -ne 0 ]] || die "run as your normal user; sudo is asked for when needed"
  [[ -f /etc/arch-release ]] || die "Terminus-Est needs Arch Linux"

  if ((want_packages)); then install_packages; fi
  install_launcher_font
  fetch_repository
  link_configuration
  build_and_render
  enable_user_units
  print_next_steps
}

main "$@"
