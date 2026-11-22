if [[ -n ${TERMINUS_ONLINE_INSTALL:-} ]]; then
  # Use Arch repositories and preserve the installation's selected mirrors.
  sudo cp -f "$TERMINUS_DESKTOP_PATH/default/pacman/pacman.conf" /etc/pacman.conf
  terminus-update-keyring
  sudo pacman -Syu --noconfirm
  terminus-pkg-add base-devel
fi
