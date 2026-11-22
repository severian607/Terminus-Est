#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -eEo pipefail

# Define Terminus-Est locations
export TERMINUS_DESKTOP_PATH="$HOME/.local/share/terminus-est"
export TERMINUS_INSTALL="$TERMINUS_DESKTOP_PATH/install"
export TERMINUS_INSTALL_LOG_FILE="/var/log/terminus-install.log"
export PATH="$TERMINUS_DESKTOP_PATH/bin:$PATH"

# Install
source "$TERMINUS_INSTALL/helpers/all.sh"
source "$TERMINUS_INSTALL/preflight/all.sh"
source "$TERMINUS_INSTALL/packaging/all.sh"
source "$TERMINUS_INSTALL/config/all.sh"
source "$TERMINUS_INSTALL/login/all.sh"
source "$TERMINUS_INSTALL/post-install/all.sh"
