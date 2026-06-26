#!/bin/sh
#
# scripts/coverage.sh <subproject> <floor-percent>
#
# Builds <subproject> (flowc or flowd) instrumented for coverage, runs
# its test suite, and reports first-party line coverage. Exits non-zero
# if coverage is below <floor-percent> — a ratchet that fails CI on a
# regression. Generated parser/lexer (parse.tab.c, lex.yy.c) and
# vendored cJSON are excluded; they are not first-party code.
#
# gcov flavor is selected via $GCOV (default: gcov). On macOS use
# `GCOV='xcrun llvm-cov gcov'`. flowd's corpus tests need flowc built
# first, so build flowc before running this for flowd.
#
# Conformant POSIX sh. No bashisms.

set -eu

proj=${1:?usage: coverage.sh <flowc|flowd> <floor>}
floor=${2:?usage: coverage.sh <flowc|flowd> <floor>}
GCOV=${GCOV:-gcov}

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root/$proj"

# macOS: prefer a Homebrew Bison >= 3.0 if the one on PATH is too old, the
# same way build.sh does. ./configure below needs a modern bison for flowc;
# the system bison on macOS is typically 2.3 and fails configure. (flowd
# does not use bison, so this is a no-op there.)
bison_major=0
if command -v bison >/dev/null 2>&1; then
    bison_major=$(bison --version 2>/dev/null | sed -n '1s/.*) \([0-9]*\).*/\1/p')
    [ -z "$bison_major" ] && bison_major=0
fi
if [ "$bison_major" -lt 3 ]; then
    for p in /opt/homebrew/opt/bison/bin /usr/local/opt/bison/bin; do
        if [ -x "$p/bison" ]; then
            PATH="$p:$PATH"; export PATH
            break
        fi
    done
fi

# The instrumented build below is configured into config.mk and baked into
# every .o. Restore the normal config and drop the instrumented objects on
# exit, so a later plain `make` (or a manual link against the archive) is not
# silently coverage-instrumented. Runs even if a step below fails.
trap 'make clean >/dev/null 2>&1; ./configure >/dev/null 2>&1; \
      find . \( -name "*.gcda" -o -name "*.gcno" \) -delete 2>/dev/null' EXIT

# Force a clean instrumented rebuild — reusing non-coverage objects would
# silently skip the --coverage recompile and produce no .gcda data.
make clean >/dev/null 2>&1 || true
# make clean keeps *.gcda/*.gcno; stale coverage data from a prior run has a
# different counter layout, so the instrumented binary corrupts the merge
# ("mismatched number of counters") — losing data and flooding stderr.
find . \( -name '*.gcda' -o -name '*.gcno' \) -delete 2>/dev/null || true
CFLAGS='--coverage -O0 -g' LDFLAGS='--coverage' ./configure >/dev/null
make >/dev/null
# Exercise the code to emit .gcda. Coverage is a metric, not a correctness
# gate (the build/sanitize jobs gate behaviour); the tests still run, we
# just don't fail on their exit. An instrumented binary can also write
# libgcov profiling notes to stderr, which the test harness (it requires
# empty stderr) flags — GCOV_ERROR_FILE silences those where the runtime
# honours it, and the `|| true` tolerates them where it doesn't.
GCOV_ERROR_FILE=/dev/null make test >/dev/null 2>&1 || true

# Emit .gcov reports next to the objects.
$GCOV -o src src/*.c >/dev/null 2>&1 || true
[ -d src/adapters ] && $GCOV -o src/adapters src/adapters/*.c >/dev/null 2>&1 || true

# Sum covered/total executable lines across first-party .gcov reports.
# In .gcov, each line is "<count>:<lineno>:<src>"; <count> is a number
# (executed), "#####" (not executed), or "-" (non-executable).
sum=$(
  for g in *.gcov; do
    [ -f "$g" ] || continue
    if [ "$g" = parse.tab.c.gcov ] || [ "$g" = lex.yy.c.gcov ] \
       || [ "$g" = cJSON.c.gcov ]; then continue; fi
    awk -F: '
      { gsub(/ /, "", $1) }
      $1 ~ /^[0-9]+$/ { cov++; tot++ }
      $1 == "#####"   {        tot++ }
      END { printf "%d %d\n", cov+0, tot+0 }
    ' "$g"
  done | awk '{ c+=$1; t+=$2 } END { printf "%d %d\n", c, t }'
)
cov=$(echo "$sum" | cut -d' ' -f1)
tot=$(echo "$sum" | cut -d' ' -f2)

if [ "$tot" -eq 0 ]; then
  echo "coverage.sh: no .gcov data for $proj" >&2
  exit 2
fi

pct=$(awk -v c="$cov" -v t="$tot" 'BEGIN { printf "%.1f", 100*c/t }')
echo "$proj line coverage: $pct% ($cov/$tot first-party lines; floor $floor%)"

awk -v p="$pct" -v f="$floor" 'BEGIN { exit !(p+0 >= f+0) }' || {
  echo "coverage.sh: $proj coverage $pct% is below floor $floor%" >&2
  exit 1
}
