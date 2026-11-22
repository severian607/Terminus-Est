# Set default XCompose that is triggered with CapsLock
tee ~/.XCompose >/dev/null <<EOF
# Run terminus-restart-xcompose to apply changes

# Include fast emoji access
include "%H/.local/share/terminus-est/default/xcompose"

# Identification
<Multi_key> <space> <n> : "$TERMINUS_USER_NAME"
<Multi_key> <space> <e> : "$TERMINUS_USER_EMAIL"
EOF
