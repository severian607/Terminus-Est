# Install all base packages
mapfile -t packages < <(grep -v '^#' "$TERMINUS_INSTALL/terminus-base.packages" | grep -v '^$')
terminus-pkg-add "${packages[@]}"
