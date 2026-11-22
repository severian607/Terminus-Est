#!/bin/sh
# Memory of the given processes and all their descendants, in bytes (proportional set size, so shared pages are
# split between the processes using them instead of counted for each). Prints one number; 0 if none are readable.
#
#   memory.sh <pid>...
[ $# -gt 0 ] || { echo 0; exit 0; }
ps -e -o pid=,ppid= | awk -v roots="$*" '
    BEGIN { n = split(roots, r, " "); for (i = 1; i <= n; i++) want[r[i]] = 1 }
    { parent[$1] = $2; pids[++count] = $1 }
    END {
        # A process belongs when it or any ancestor is a root.
        for (i = 1; i <= count; i++) {
            p = pids[i]; q = p; depth = 0
            while (q != "" && q > 1 && depth < 64) {
                if (q in want) { print p; break }
                q = parent[q]; depth++
            }
        }
    }' | while read -r pid; do
    awk '/^Pss:/ { print $2; exit }' "/proc/$pid/smaps_rollup" 2>/dev/null
done | awk '{ kb += $1 } END { printf "%d\n", kb * 1024 }'
