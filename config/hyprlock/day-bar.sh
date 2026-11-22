#!/bin/sh
# Share of the day elapsed, one cell per two hours.
h=$(date +%-H); n=$((h / 2)); bar=""; i=0
while [ $i -lt 12 ]; do if [ $i -lt $n ]; then bar="${bar}#"; else bar="${bar}."; fi; i=$((i+1)); done
printf '[ %s ]  %02d%%' "$bar" "$((($(date +%-H) * 60 + $(date +%-M)) * 100 / 1440))"
