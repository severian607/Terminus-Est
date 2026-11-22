#!/bin/sh
# Lock-screen readout: system. Fixed 24-column box so it lines up in a monospace font.
up=$(uptime -p | sed 's/^up //; s/ hours\?/h/; s/ minutes\?/m/; s/ days\?/d/; s/,//g')
up=$(printf %s "$up" | cut -c1-16)
load=$(cut -d' ' -f1 /proc/loadavg)
mem=$(awk '/MemTotal/{t=$2}/MemAvailable/{a=$2}END{printf "%.1f/%.1fG",(t-a)/1048576,t/1048576}' /proc/meminfo)
row() { printf '│ %-5s %-16s│\n' "$1" "$2"; }
printf '┌─ SYSTEM ──────────────┐\n'
row up "$up"
row load "$load"
row mem "$mem"
printf '└───────────────────────┘'
