#!/bin/sh
#
# scripts/diag_coverage.sh
#
# Fails if any diagnostic code EMITTED in src is asserted by no test —
# the "every diagnostic has a test" ratchet. A code is "emitted" when it
# is the id argument of a diag_emit / exec_err / emit_err call in src. It
# is "tested" when it appears in a fixture .expected.err or is matched
# (strstr/strcmp) by an api test.
#
# Codes that are merely referenced (a reserved id in a comment or a
# try/else recoverable-set check) are NOT counted as emitted, so reserved
# ids like W203 / R102 need no allowlist — they simply never appear.
#
# Conformant POSIX sh.

set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
rc=0

# --- emitted: the id immediately after the severity (diag_emit) or as the
#     2nd argument of exec_err / emit_err. ------------------------------
emitted_flowc=$(
  grep -rhoE 'DIAG_(ERROR|WARNING|NOTE), *"[EW][0-9]{3}"' \
    "$root"/flowc/src/*.c "$root"/flowc/src/*.l "$root"/flowc/src/*.y 2>/dev/null \
  | grep -oE '[EW][0-9]{3}' | sort -u
)
emitted_flowd=$(
  grep -rhoE '(exec_err|emit_err)\([^,]*, *"R[0-9]{3}"|DIAG_ERROR, *"R[0-9]{3}"' \
    "$root"/flowd/src/*.c 2>/dev/null \
  | grep -oE 'R[0-9]{3}' | sort -u
)

# --- tested: fixtures (error[<id>]) + api-test string literals. --------
tested_flowc=$(
  { grep -rhoE '(error|warning)\[[EW][0-9]{3}\]' "$root"/flowc/tests/fixtures/*.expected.err 2>/dev/null | grep -oE '[EW][0-9]{3}'
    grep -rhoE '"[EW][0-9]{3}"' "$root"/flowc/tests/api/*.c 2>/dev/null | grep -oE '[EW][0-9]{3}'
  } | sort -u
)
tested_flowd=$(
  { grep -rhoE '"R[0-9]{3}"' "$root"/flowd/tests/api/*.c 2>/dev/null | grep -oE 'R[0-9]{3}'
    grep -rhoE 'R[0-9]{3}' "$root"/flowd/tests/fixtures/*.expected.err 2>/dev/null | grep -oE 'R[0-9]{3}'
  } | sort -u
)

check() {
  name=$1; emitted=$2; tested=$3
  miss=$(printf '%s\n' "$emitted" | while IFS= read -r c; do
    [ -n "$c" ] || continue
    printf '%s\n' "$tested" | grep -qxF "$c" || printf '%s\n' "$c"
  done)
  n=$(printf '%s\n' "$emitted" | grep -c . || true)
  if [ -n "$miss" ]; then
    echo "$name: EMITTED but untested diagnostic codes:" >&2
    printf '%s\n' "$miss" | sed 's/^/  /' >&2
    rc=1
  else
    echo "$name: all $n emitted diagnostic codes have a test"
  fi
}

check flowc "$emitted_flowc" "$tested_flowc"
check flowd "$emitted_flowd" "$tested_flowd"
exit $rc
