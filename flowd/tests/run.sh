#!/bin/sh
#
# run.sh — flowd test driver.
#
# Runs the value-model API test, then
# loops over IR fixtures under tests/fixtures/. Each fixture is a
# .ir.json file that the driver loads via `flowd --load-ir <path>`
# and diffs against expected sidecars:
#
#   <stem>.expected.out    diffed against stdout
#   <stem>.expected.err    diffed against stderr
#   <stem>.expected.exit   single-decimal expected exit code
#
# All sidecars are optional. When .expected.out / .expected.err are
# absent, the corresponding stream must be empty. When .expected.exit
# is absent the expected code is 0.
#
# Path normalization: the absolute project root is stripped from
# stderr before diffing, so diagnostics like
# `/long/path/foo.ir.json: error: ...` become `foo.ir.json: error: ...`
# and diffs stay stable across machines.
#
# Conformant POSIX sh. No bashisms. Determinism: pins
# SOURCE_DATE_EPOCH=0 and LC_ALL=C.

set -u

cd "$(dirname "$0")/.." || exit 2
PROJ=$(pwd)

SOURCE_DATE_EPOCH=0
export SOURCE_DATE_EPOCH
LC_ALL=C
export LC_ALL

PASS=0
FAIL=0

# ---------------------------------------------------------------------
# API tests — C binaries under tests/api/.
# ---------------------------------------------------------------------

run_api_test() {
    name=$1
    bin=$2
    if [ ! -x "$bin" ]; then
        echo "test/run.sh: $bin not built; run 'make' first" >&2
        FAIL=$((FAIL + 1))
        return
    fi
    out_tmp=$(mktemp 2>/dev/null) || { echo "mktemp failed" >&2; exit 2; }
    if "$bin" > "$out_tmp" 2>&1; then
        PASS=$((PASS + 1))
        printf '%s  ok\n' "$name"
    else
        FAIL=$((FAIL + 1))
        printf '%s  FAIL\n' "$name"
        sed 's/^/    /' < "$out_tmp"
    fi
    rm -f "$out_tmp"
}

run_api_test "api/test_value (value model)"   tests/api/test_value
run_api_test "api/test_exec  (executor)"      tests/api/test_exec
run_api_test "api/test_trace (trace+replay)"  tests/api/test_trace
run_api_test "api/test_gateway (gateway)"     tests/api/test_gateway
run_api_test "api/test_suspension"            tests/api/test_suspension
run_api_test "api/test_anthropic"             tests/api/test_anthropic
run_api_test "api/test_budget_cancel"         tests/api/test_budget_cancel
run_api_test "api/test_replay_restore"        tests/api/test_replay_restore

# ---------------------------------------------------------------------
# IR fixture tests — diff stdout/stderr/exit against sidecars.
# ---------------------------------------------------------------------

run_fixture() {
    input=$1
    stem=${input%.ir.json}
    name=$(basename "$stem")

    exp_out="$stem.expected.out"
    exp_err="$stem.expected.err"
    exp_exit_file="$stem.expected.exit"

    if [ -f "$exp_exit_file" ]; then
        expected_exit=$(cat "$exp_exit_file")
    else
        expected_exit=0
    fi

    out_tmp=$(mktemp 2>/dev/null) || { echo "mktemp failed" >&2; exit 2; }
    err_tmp=$(mktemp 2>/dev/null) || { echo "mktemp failed" >&2; exit 2; }

    ./flowd --load-ir "$input" > "$out_tmp" 2> "$err_tmp"
    actual_exit=$?

    # Normalize stderr: strip the absolute project root so diagnostic
    # paths read as fixture-relative. The trailing slash is stripped
    # with the prefix to avoid leaving a stray leading "/".
    norm_err=$(mktemp 2>/dev/null) || { echo "mktemp failed" >&2; exit 2; }
    sed "s|${PROJ}/||g" < "$err_tmp" > "$norm_err"

    fail_reasons=""

    # stdout
    if [ -f "$exp_out" ]; then
        if ! diff -u "$exp_out" "$out_tmp" >/dev/null 2>&1; then
            fail_reasons="$fail_reasons stdout"
        fi
    else
        if [ -s "$out_tmp" ]; then
            fail_reasons="$fail_reasons stdout-nonempty"
        fi
    fi

    # stderr
    if [ -f "$exp_err" ]; then
        if ! diff -u "$exp_err" "$norm_err" >/dev/null 2>&1; then
            fail_reasons="$fail_reasons stderr"
        fi
    else
        if [ -s "$norm_err" ]; then
            fail_reasons="$fail_reasons stderr-nonempty"
        fi
    fi

    # exit
    if [ "$actual_exit" != "$expected_exit" ]; then
        fail_reasons="$fail_reasons exit($actual_exit!=$expected_exit)"
    fi

    if [ -z "$fail_reasons" ]; then
        PASS=$((PASS + 1))
        printf 'fixture/%s  ok\n' "$name"
    else
        FAIL=$((FAIL + 1))
        printf 'fixture/%s  FAIL%s\n' "$name" "$fail_reasons"
        if echo "$fail_reasons" | grep -q stdout; then
            printf '  --- stdout diff:\n'
            diff -u "$exp_out" "$out_tmp" 2>&1 | sed 's/^/    /'
        fi
        if echo "$fail_reasons" | grep -q stderr; then
            printf '  --- stderr diff:\n'
            diff -u "$exp_err" "$norm_err" 2>&1 | sed 's/^/    /'
        fi
    fi

    rm -f "$out_tmp" "$err_tmp" "$norm_err"
}

FIXTURES=tests/fixtures
if [ -d "$FIXTURES" ]; then
    for input in "$FIXTURES"/*.ir.json; do
        [ -f "$input" ] || continue
        run_fixture "$input"
    done
fi

# ---------------------------------------------------------------------
# Corpus smoke — compile each cleanly-compiling flowc fixture into IR,
# load it via flowd, assert success. No expected-output diff; the test
# is "the loader handles every real-world IR shape the compiler emits."
#
# The corpus is regenerated on every run from flowc's source fixtures
# at $FLOWC_FIXTURES (default ../flowc/tests/fixtures). FLOWC overrides
# the compiler path. The generated IR lives under tests/corpus/ and is
# treated as a build artifact (gitignored, removed by `make clean`).
#
# Stem list pinned to the 20 fixtures that produce v1 IR (verified
# 2026-06-24). The remaining 20 .flow files in the flowc directory are
# mostly error-path diagnostic fixtures that produce no IR; one passing
# _smoke fixture (per_row_match_smoke) is intentionally omitted here.
# ---------------------------------------------------------------------

FLOWC=${FLOWC:-../flowc/flowc}
FLOWC_FIXTURES=${FLOWC_FIXTURES:-../flowc/tests/fixtures}
CORPUS=tests/corpus

CORPUS_STEMS="
    aggregator_smoke arith_smoke bool_smoke check_smoke
    empty_list_context if_smoke ir_check_only ir_smoke
    list_lit_smoke match_bind_smoke match_smoke onboard
    parse_smoke pipeline_v1_smoke resolve_smoke row_smoke
    subflow_smoke sum_smoke tokens_smoke try_else_smoke
"

if [ -x "$FLOWC" ] && [ -d "$FLOWC_FIXTURES" ]; then
    mkdir -p "$CORPUS"
    for stem in $CORPUS_STEMS; do
        src="$FLOWC_FIXTURES/$stem.flow"
        if [ ! -f "$src" ]; then
            FAIL=$((FAIL + 1))
            printf 'corpus/%s  FAIL  missing source: %s\n' "$stem" "$src"
            continue
        fi
        ir="$CORPUS/$stem.ir.json"
        # Compile silently; any flowc failure is a setup error.
        if ! "$FLOWC" "$src" -o "$ir" >/dev/null 2>&1; then
            FAIL=$((FAIL + 1))
            printf 'corpus/%s  FAIL  flowc rejected source\n' "$stem"
            continue
        fi
        # Load via flowd. We only assert exit 0 and empty stderr.
        err_tmp=$(mktemp 2>/dev/null) || exit 2
        if ./flowd --load-ir "$ir" >/dev/null 2> "$err_tmp"; then
            if [ -s "$err_tmp" ]; then
                FAIL=$((FAIL + 1))
                printf 'corpus/%s  FAIL  unexpected stderr:\n' "$stem"
                sed 's/^/    /' < "$err_tmp"
            else
                PASS=$((PASS + 1))
                printf 'corpus/%s  ok\n' "$stem"
            fi
        else
            FAIL=$((FAIL + 1))
            printf 'corpus/%s  FAIL  loader rejected IR:\n' "$stem"
            sed 's/^/    /' < "$err_tmp"
        fi
        rm -f "$err_tmp"
    done
else
    printf 'corpus/  SKIP  (FLOWC=%s or FLOWC_FIXTURES=%s missing)\n' \
        "$FLOWC" "$FLOWC_FIXTURES"
fi

# ---------------------------------------------------------------------
# Run smoke — `flowd run` executes a compiled flow end to end against
# type-directed stub tools and writes a trace. This locks in the
# flowc -> flowd CLI chain. The stem list is the subset that completes
# under stub data; other real flows legitimately error under stubs
# (e.g. `pick` on a list a `where` stage emptied), which is correct
# runtime behavior, not a regression. Reuses the IR the corpus section
# generated above under $CORPUS.
# ---------------------------------------------------------------------
RUN_STEMS="arith_smoke bool_smoke if_smoke match_smoke sum_smoke try_else_smoke"
if [ -x ./flowd ] && [ -f "$CORPUS/arith_smoke.ir.json" ]; then
    run_trace=$(mktemp -d 2>/dev/null) || { echo "tests/run.sh: mktemp failed" >&2; exit 2; }
    for stem in $RUN_STEMS; do
        ir="$CORPUS/$stem.ir.json"
        if [ ! -f "$ir" ]; then
            FAIL=$((FAIL + 1)); printf 'run/%s  FAIL  missing IR\n' "$stem"
            continue
        fi
        out=$(./flowd run "$ir" --trace-dir "$run_trace/$stem" 2>/dev/null)
        rc=$?
        if [ "$rc" = "0" ] && [ -n "$out" ]; then
            PASS=$((PASS + 1)); printf 'run/%s  ok\n' "$stem"
        else
            FAIL=$((FAIL + 1))
            printf 'run/%s  FAIL  (exit %s) %s\n' "$stem" "$rc" "$out"
        fi
    done
    rm -rf "$run_trace"
else
    printf 'run/  SKIP  (flowd or corpus IR missing)\n'
fi

# ---------------------------------------------------------------------
# Differential harness — compile each known stem through flowc, then
# run flowd --canonical-dump, then diff against the frozen golden
# under tests/diff/expected/. Catches IR drift between compiler and
# loader that the corpus smoke would miss (corpus just asserts
# "loads", not "loads to the same thing").
# ---------------------------------------------------------------------

DIFF_DRIVER=tests/diff/run_diff.sh
if [ -x "$DIFF_DRIVER" ] && [ -x "$FLOWC" ]; then
    diff_out=$(mktemp 2>/dev/null) || exit 2
    if FLOWC="$FLOWC" FLOWC_FIXTURES="$FLOWC_FIXTURES" "$DIFF_DRIVER" \
            > "$diff_out" 2>&1; then
        # Per-stem PASS lines; count and surface them.
        while IFS= read -r line; do
            case "$line" in
                diff/*' ok')   PASS=$((PASS + 1)); echo "$line" ;;
                diff/*' FAIL'*) FAIL=$((FAIL + 1)); echo "$line" ;;
                'diff PASS '*) : ;;  # consumed by counters above
                *) [ -n "$line" ] && echo "$line" ;;
            esac
        done < "$diff_out"
    else
        # Driver itself failed; surface output and count as one FAIL.
        FAIL=$((FAIL + 1))
        printf 'diff/  FAIL  harness driver returned nonzero:\n'
        sed 's/^/    /' < "$diff_out"
    fi
    rm -f "$diff_out"
fi

printf '\nPASS %d  FAIL %d\n' "$PASS" "$FAIL"
[ "$FAIL" = "0" ]
