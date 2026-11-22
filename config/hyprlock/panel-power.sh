#!/bin/sh
# Lock-screen readout: power and network. No SSID or address is shown on a locked screen.
cap=$(cat /sys/class/power_supply/BAT0/capacity)
st=$(cat /sys/class/power_supply/BAT0/status)
n=$((cap / 10)); bar=""; i=0
while [ $i -lt 10 ]; do if [ $i -lt $n ]; then bar="${bar}#"; else bar="${bar}."; fi; i=$((i+1)); done
wifi=$(nmcli -t -f ACTIVE,SIGNAL dev wifi 2>/dev/null | awk -F: '$1=="yes"{print $2"%"; f=1} END{if(!f) print "offline"}')
row() { printf '│ %-5s %-16s│\n' "$1" "$2"; }
printf '┌─ POWER ───────────────┐\n'
row bat "$bar $cap%"
row state "$st"
row wifi "$wifi"
printf '└───────────────────────┘'
