#!/bin/sh
# build.sh — convenience bootstrap for the Flow monorepo.
#
# There is no unified build system (each subproject builds and releases
# on its own; see README.md). This script is just a friendly front door
# for newcomers: it builds and tests the two C subprojects in the right
# order — flowc, then flowd — and prints a summary.
#
#   ./build.sh           build + test flowc and flowd
#   ./build.sh --no-test build only, skip the test suites
#
# On macOS the system Bison (2.3) is too old for flowc; if a newer Bison
# is installed via Homebrew we put it on PATH automatically for this run.

set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
RUN_TESTS=1
[ "${1:-}" = "--no-test" ] && RUN_TESTS=0

# --- macOS: prefer a Homebrew Bison >= 3.0 if the one on PATH is too old.
bison_major=0
if command -v bison >/dev/null 2>&1; then
    bison_major=$(bison --version 2>/dev/null | sed -n '1s/.*) \([0-9]*\).*/\1/p')
    [ -z "$bison_major" ] && bison_major=0
fi
if [ "$bison_major" -lt 3 ]; then
    for p in /opt/homebrew/opt/bison/bin /usr/local/opt/bison/bin; do
        if [ -x "$p/bison" ]; then
            PATH="$p:$PATH"; export PATH
            echo "build.sh: using newer Bison at $p"
            break
        fi
    done
fi

build_one() {
    name=$1
    echo
    echo "=== $name ==="
    cd "$ROOT/$name"
    ./configure
    make
    if [ "$RUN_TESTS" -eq 1 ]; then
        make test
    fi
}

build_one flowc
build_one flowd

echo
echo "build.sh: done. The binaries are flowc/flowc and flowd/flowd."
echo "Try the examples:"
echo "  ./flowc/flowc examples/hello.flow -o /tmp/hello.ir.json"
echo "  ./flowd/flowd run /tmp/hello.ir.json --input 16     # -> 42"
