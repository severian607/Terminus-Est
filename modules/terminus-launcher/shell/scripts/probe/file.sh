#!/bin/sh
# What the launcher shows for a selected file:
#
#   line 1   its MIME type
#   line 2   the desktop ids registered for that type, default first, space-separated (may be empty)
#   line 3   ---
#   then     up to 12 lines of the file when it is text (tabs as spaces), nothing otherwise
#
#   file.sh <path>
p="$1"
mime=$(file --mime-type -b -- "$p" 2>/dev/null)
echo "${mime:-unknown}"
gio mime "$mime" 2>/dev/null | awk '
    /^Default application/ { sub(/.*: /, ""); if ($0 != "") { list = $0; seen[$0] = 1 } next }
    /^Registered applications:/ { reg = 1; next }
    /^[A-Z]/ { reg = 0 }
    reg && NF { gsub(/^[ \t]+/, ""); if (!($0 in seen)) { list = list (list == "" ? "" : " ") $0; seen[$0] = 1 } }
    END { print list }'
echo ---
case "$mime" in
    text/* | application/json | application/xml | application/javascript | application/x-shellscript | application/toml | inode/x-empty)
        head -c 4096 -- "$p" 2>/dev/null | head -n 12 | expand -t 4
        ;;
esac
