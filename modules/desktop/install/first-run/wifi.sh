if ! ping -c3 -W1 1.1.1.1 >/dev/null 2>&1; then
  notify-send "    Update System" "When you have internet, run 'terminus update' in a terminal." -u critical
  notify-send "󰖩    Set Up Wi-Fi" "Open the network tab of the control center (Super+Ctrl+W)." -u critical
else
  notify-send "    Update System" "Run 'terminus update' in a terminal to update the system." -u critical
fi
