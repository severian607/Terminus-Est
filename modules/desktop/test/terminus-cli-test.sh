#!/bin/bash

set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
CLI="$TMPDIR/desktop-core"
ln -s "$ROOT/bin/desktop-core" "$CLI"

fail() {
  printf 'not ok - %s\n' "$1" >&2
  exit 1
}

# The primary dispatcher must never become a desktop helper or be called by it.
cat >"$TMPDIR/terminus-est" <<'SH'
#!/bin/bash
exit 99
SH

cat >"$TMPDIR/terminus-weird-test" <<'SH'
#!/bin/bash
# ordinary comments are fine
# terminus:this malformed line should be ignored
# terminus:summary=Exercise malformed metadata routing
# terminus:made-up=value
printf 'weird:%s\n' "$*"
SH

cat >"$TMPDIR/terminus-partial-meta-test" <<'SH'
#!/bin/bash
# terminus:summary=Exercise partial metadata routing
# terminus:made-up=value
printf 'partial:%s\n' "$*"
SH

cat >"$TMPDIR/terminus-body-metadata-test" <<'SH'
#!/bin/bash
printf 'body:%s\n' "$*"
# terminus:group=wrong
# terminus:name=wrong
SH
chmod +x "$TMPDIR"/terminus-est*

[[ $("$CLI" weird test 'two words') == 'weird:two words' ]] || fail 'malformed metadata preserves dispatch and arguments'
[[ $("$CLI" partial meta test value) == 'partial:value' ]] || fail 'partial metadata preserves filename routing'
[[ $("$CLI" body metadata test value) == 'body:value' ]] || fail 'body comments cannot redefine routing'

"$CLI" commands --json | jq -e '
  ([.commands[].binary] | sort) == [
    "terminus-body-metadata-test", "terminus-partial-meta-test", "terminus-weird-test"
  ]
' >/dev/null || fail 'discovery excludes the primary dispatcher'

output=$("$CLI" weird test --help)
[[ $output != *'weird:'* ]] || fail 'help executes the requested helper'

if "$CLI" nonexistent route >/dev/null 2>&1; then
  fail 'unknown routes must fail'
else
  [[ $? == 127 ]] || fail 'unknown routes must return command-not-found status'
fi

printf 'ok - desktop command discovery, routing, argument preservation and safe help\n'
