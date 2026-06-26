#!/bin/sh
#
# tests/run.sh -- flowc test driver
#
# Walks tests/fixtures/, runs ./flowc on each input file, and diffs
# captured stdout, stderr, and exit code against the corresponding
# .expected.* sidecar files.  Prints a one-line summary and exits
# non-zero if any fixture fails.
#
# Conventions
# -----------
#
#   Input file:   any file under tests/fixtures/ whose name does NOT
#                 contain the substring `.expected.`.  Typically
#                 named <stem>.flow; non-.flow inputs are permitted
#                 (and used to exercise the W101 path).
#
#   Sidecars:     <stem>.expected.out    diffed against stdout
#                 <stem>.expected.err    diffed against stderr
#                 <stem>.expected.exit   single decimal exit code
#                 <stem>.args            extra CLI args, word-split
#                                        and passed before the input
#                                        path (e.g. "--dump=tokens").
#
#   All sidecars are optional.  When a sidecar is absent:
#     - missing .expected.out and stdout is non-empty -> fail
#     - missing .expected.err and stderr is non-empty -> fail
#     - missing .expected.exit -> expected exit is 0
#   This prevents output from silently escaping the test net.
#
# Path normalization
# ------------------
#
# Before diffing, the absolute project root is stripped from stderr,
# so diagnostics like `/long/checkout/path/foo.flow:1:1: ...` become
# `foo.flow:1:1: ...` and diffs stay stable across machines and CI
# runners.  This matters as soon as anything in the pipeline reports
# paths -- which is everything from step 1 onward.
#
# Portability
# -----------
#
# Conforms to POSIX.1-2008 sh.  No bashisms (`[[`, `set -o pipefail`,
# arrays).  `mktemp` is used the same way it is in ./configure.

set -u

# Always operate from the project root, regardless of how the
# driver was invoked (`./tests/run.sh`, `make test`, etc.).
cd "$(dirname "$0")/.." || exit 2

# Determinism for IR fixtures: pin compiled_at to the Unix epoch.
# flowc honors SOURCE_DATE_EPOCH (reproducible-builds convention)
# in src/ir.c; setting it here means golden files are byte-stable
# regardless of when or where the test driver runs.
SOURCE_DATE_EPOCH=0
export SOURCE_DATE_EPOCH

FLOWC=./flowc
FIXTURES=tests/fixtures
PASS=0
FAIL=0

if [ ! -x "$FLOWC" ]; then
    echo "tests/run.sh: $FLOWC not built; run 'make' first" >&2
    exit 2
fi

if [ ! -d "$FIXTURES" ]; then
    echo "tests/run.sh: $FIXTURES not found" >&2
    exit 2
fi

PROJ=$(pwd)

for input in "$FIXTURES"/*; do
    # The glob may not match anything if fixtures/ is empty; in that
    # case `$input` is the literal pattern and is not a regular file.
    [ -f "$input" ] || continue

    # Skip sidecar files; they are matched up to inputs by stem.
    case "$input" in
        *.expected.*|*.args) continue ;;
    esac

    stem=${input%.*}
    name=$(basename "$stem")

    exp_out="$stem.expected.out"
    exp_err="$stem.expected.err"
    exp_exit_file="$stem.expected.exit"

    if [ -f "$exp_exit_file" ]; then
        expected_exit=$(cat "$exp_exit_file")
    else
        expected_exit=0
    fi

    out_tmp=$(mktemp 2>/dev/null) || { echo "tests/run.sh: mktemp failed" >&2; exit 2; }
    err_tmp=$(mktemp 2>/dev/null) || { echo "tests/run.sh: mktemp failed" >&2; exit 2; }

    # Optional extra CLI args (word-split intentionally).
    if [ -f "$stem.args" ]; then
        args=$(cat "$stem.args")
    else
        args=
    fi

    # shellcheck disable=SC2086  # word-splitting of $args is deliberate.
    "$FLOWC" $args "$input" >"$out_tmp" 2>"$err_tmp"
    actual_exit=$?

    # Strip the absolute project root from stderr so diffs are stable
    # across checkout locations.  Relative paths pass through unchanged.
    sed "s|$PROJ/||g" "$err_tmp" > "$err_tmp.norm"
    mv "$err_tmp.norm" "$err_tmp"

    failures=""

    if [ "$actual_exit" -ne "$expected_exit" ]; then
        failures="$failures exit($actual_exit!=$expected_exit)"
    fi

    if [ -f "$exp_out" ]; then
        if ! diff -u "$exp_out" "$out_tmp" >/dev/null 2>&1; then
            failures="$failures stdout"
        fi
    elif [ -s "$out_tmp" ]; then
        failures="$failures unexpected-stdout"
    fi

    if [ -f "$exp_err" ]; then
        if ! diff -u "$exp_err" "$err_tmp" >/dev/null 2>&1; then
            failures="$failures stderr"
        fi
    elif [ -s "$err_tmp" ]; then
        failures="$failures unexpected-stderr"
    fi

    if [ -z "$failures" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        echo "FAIL: $name [$failures ]"
        if [ -f "$exp_out" ] && ! diff -u "$exp_out" "$out_tmp" >/dev/null 2>&1; then
            echo "--- stdout diff ($name) ---"
            diff -u "$exp_out" "$out_tmp"
        fi
        if [ -f "$exp_err" ] && ! diff -u "$exp_err" "$err_tmp" >/dev/null 2>&1; then
            echo "--- stderr diff ($name) ---"
            diff -u "$exp_err" "$err_tmp"
        fi
        if [ ! -f "$exp_out" ] && [ -s "$out_tmp" ]; then
            echo "--- unexpected stdout ($name) ---"
            cat "$out_tmp"
        fi
        if [ ! -f "$exp_err" ] && [ -s "$err_tmp" ]; then
            echo "--- unexpected stderr ($name) ---"
            cat "$err_tmp"
        fi
    fi

    rm -f "$out_tmp" "$err_tmp"
done

echo "PASS $PASS  FAIL $FAIL"
[ "$FAIL" -eq 0 ]
