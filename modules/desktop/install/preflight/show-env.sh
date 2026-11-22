# Show installation environment variables
gum log --level info "Installation Environment:"

env | grep -E "^(TERMINUS_CHROOT_INSTALL|TERMINUS_ONLINE_INSTALL|TERMINUS_USER_NAME|TERMINUS_USER_EMAIL|USER|HOME|TERMINUS_DESKTOP_PATH)=" | sort | while IFS= read -r var; do
  gum log --level info "  $var"
done
