if [[ $(plymouth-set-default-theme) != "terminus-est" ]]; then
  sudo cp -r "$HOME/.local/share/terminus-est/default/plymouth" /usr/share/plymouth/themes/terminus-est/
  sudo plymouth-set-default-theme terminus-est
fi
