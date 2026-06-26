#!/bin/sh
#
# tests/diff/run_diff.sh — differential test harness.
#
# Compiles each known stem from $FLOWC_FIXTURES via $FLOWC, runs the
# resulting IR through `flowd --canonical-dump`, and diffs against
# the frozen golden under tests/diff/expected/. Exits non-zero on any
# diff; prints a per-stem PASS/FAIL line.
#
# Invoked by tests/run.sh after the corpus smoke phase. Designed to
# run standalone too: `cd flowd && ./tests/diff/run_diff.sh`.
#
# Determinism: pins SOURCE_DATE_EPOCH=0 and LC_ALL=C.

set -u

cd "$(dirname "$0")/../.." || exit 2

SOURCE_DATE_EPOCH=0
export SOURCE_DATE_EPOCH
LC_ALL=C
export LC_ALL

FLOWC=${FLOWC:-../flowc/flowc}
FLOWC_FIXTURES=${FLOWC_FIXTURES:-../flowc/tests/fixtures}
EXPECTED=tests/diff/expected
WORK=tests/diff/work

# Stems for which we keep a golden. Three picked for shape coverage.
STEMS="onboard sum_smoke pipeline_v1_smoke"

PASS=0
FAIL=0

if [ ! -x "$FLOWC" ]; then
    echo "diff/  SKIP  (FLOWC=$FLOWC not executable)" >&2
    printf '\ndiff PASS 0  FAIL 0\n'
    exit 0
fi

mkdir -p "$WORK"

for stem in $STEMS; do
    src="$FLOWC_FIXTURES/$stem.flow"
    if [ ! -f "$src" ]; then
        FAIL=$((FAIL + 1))
        printf 'diff/%s  FAIL  missing source: %s\n' "$stem" "$src"
        continue
    fi
    exp="$EXPECTED/$stem.canonical.json"
    if [ ! -f "$exp" ]; then
        FAIL=$((FAIL + 1))
        printf 'diff/%s  FAIL  missing golden: %s\n' "$stem" "$exp"
        continue
    fi

    ir="$WORK/$stem.ir.json"
    if ! "$FLOWC" "$src" -o "$ir" >/dev/null 2>&1; then
        FAIL=$((FAIL + 1))
        printf 'diff/%s  FAIL  flowc rejected source\n' "$stem"
        continue
    fi

    actual="$WORK/$stem.canonical.json"
    if ! ./flowd --canonical-dump "$ir" > "$actual" 2>/dev/null; then
        FAIL=$((FAIL + 1))
        printf 'diff/%s  FAIL  flowd rejected IR\n' "$stem"
        continue
    fi

    if diff -u "$exp" "$actual" >/dev/null 2>&1; then
        PASS=$((PASS + 1))
        printf 'diff/%s  ok\n' "$stem"
    else
        FAIL=$((FAIL + 1))
        printf 'diff/%s  FAIL  canonical-dump diverges from golden:\n' \
               "$stem"
        diff -u "$exp" "$actual" | sed 's/^/    /' | head -40
    fi
done

printf '\ndiff PASS %d  FAIL %d\n' "$PASS" "$FAIL"
[ "$FAIL" = "0" ]
