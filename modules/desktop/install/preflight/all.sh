source $TERMINUS_INSTALL/preflight/guard.sh
source $TERMINUS_INSTALL/preflight/begin.sh
run_logged $TERMINUS_INSTALL/preflight/show-env.sh
run_logged $TERMINUS_INSTALL/preflight/pacman.sh
run_logged $TERMINUS_INSTALL/preflight/first-run-mode.sh
run_logged $TERMINUS_INSTALL/preflight/disable-mkinitcpio.sh
