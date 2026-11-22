# Set identification from install inputs
if [[ -n ${TERMINUS_USER_NAME//[[:space:]]/} ]]; then
  git config --global user.name "$TERMINUS_USER_NAME"
fi

if [[ -n ${TERMINUS_USER_EMAIL//[[:space:]]/} ]]; then
  git config --global user.email "$TERMINUS_USER_EMAIL"
fi
