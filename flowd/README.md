# flowd

The Flow runtime. Loads a Flow IR document compiled by `flowc`,
lets a host register typed tool and model implementations, and
executes flows against typed inputs while writing a complete
trace of every step to disk. Written in portable ISO C99 against
POSIX.1-2008. Ships as `libflowd.a` for in-process embedding —
the forthcoming Python and TypeScript bindings will link against
it — and as a standalone `flowd` binary that runs flows from a
compiled IR and handles trace replay and trace diffing.

The public C API in `src/flowd.h` is the locked contract for
v1. The runtime behind it — IR loading, execution,
suspension/resume, replay, the model gateway, and the trace
store — is implemented and exercised by `make test`.

## Building

Requires a C99 compiler and a POSIX.1-2008 environment. The
model gateway links `libcurl`, the content-addressed store
links OpenSSL `libcrypto`, and JSON parsing uses a vendored
`cJSON 1.7.x` at `src/cjson/`; `./configure` refuses to
proceed without them.

```
./configure
make
make test
```

`./configure` probes the local toolchain and writes
`config.mk`. `make` builds the `flowd` binary in the project
root. `make test` runs the C API tests under `tests/api/`,
the loader fixtures under `tests/fixtures/`, the compiled
corpus under `tests/corpus/`, and the trace-diff suite under
`tests/diff/`, and prints a one-line summary like
`PASS n  FAIL 0`.

## CLI

```
flowd run    adjudicate.ir.json --input '{...}'
flowd replay exec_2026_05_26_a3f2b1 exec_2026_11_20_replay
flowd diff   exec_2026_05_26_a3f2b1 exec_2026_11_20_replay
```

`flowd run <ir.json>` executes a flow end to end and writes a
trace under `--trace-dir` (default `./traces`). It registers a
deterministic, type-directed **stub** for every tool the IR
declares, so the chain `flowc x.flow -o x.ir.json && flowd run
x.ir.json` is runnable for any program. The flow's own logic —
expressions, pipelines, conditionals, `match`, `pick` — runs for
real; only the tool implementations are stand-ins, so this is a
smoke/dry execution. `--flow` selects a flow in a multi-flow IR;
`--input` supplies the flow input as JSON (a default is
synthesized from the parameter types when omitted). For real
tool behavior, link `libflowd` and register implementations (see
below).

`replay` and `diff` take two trace directories and print the
per-node divergences between them as JSON — typically an
original and its replay. `flowd diff` exits 0 unless a trace
cannot be read; `flowd replay` asserts strict equality and
exits 0 only when there are no divergences. Re-execution
itself — restoring Level 0 / 1 nodes from their recorded
outputs and re-invoking Level 2 (model) nodes against a new
model with the originally captured inputs, never re-invoking
Level 3 (mutation) nodes — happens through `flowd_replay` in
the C API, which writes the new trace this binary then
compares.

For production use the host owns the tool implementations: flow
execution runs through the C library (`flowd_run`), called
directly from a host program. Language bindings in
`../bindings/` will layer on top of the same `libflowd.a` seam;
they are still in progress and do not yet expose a working
runtime surface.

## Library use

```c
#include "flowd.h"

flowd_runtime *rt = flowd_load_ir(ir_json);
flowd_register_tool(rt, "fetch_credit",
                    FLOWD_EFFECT_DETERMINISTIC,
                    "(string) -> CreditHistory",
                    &fetch_credit_impl, "v1", NULL);
char *suspension = NULL;
char *output = flowd_run(rt, input_json, "traces", &suspension);
```

`src/flowd.h` is the bindings' seam: every callback ownership
rule, every R-series error code, and every effect-level
constant is documented there.

## Reproducibility

Identical source under a pinned toolchain produces a
byte-identical `flowd` binary. Identical runtime against
identical inputs produces a byte-identical trace. The trace
directory layout is `traces/<flow_name>/<execution_id>/` with
one `manifest.json`, one `nodes/n<N>.json` per node (node ids
are `n0`, `n1`, ...), and a content-addressed
`values/<sha256>.json` for any payload above the inline
threshold.

## Project layout

```
src/
  flowd.h        # public C API; the bindings' seam
  flowd.c        # public C API implementation
  main.c         # CLI entry: run, replay, diff
  ir_load.c      # IR loader: JSON -> runtime tables
  exec.c         # flow executor: run, replay, resume
  value.c        # tagged-union value type
  trace.c        # trace writer + reader
  gateway.c      # model gateway
  adapters/      # provider adapters (anthropic.c)
  util.c         # foundation utilities, diagnostics
  cjson/         # vendored cJSON 1.7.x
tests/api/       # C API tests, one binary per subsystem
tests/fixtures/  # IR loader fixtures with expected goldens
tests/corpus/    # IR corpus built from flowc's fixtures
tests/diff/      # trace diff/replay tests
tests/run.sh     # POSIX sh test driver, invoked by `make test`
configure        # POSIX sh probe script; generates config.mk
Makefile         # POSIX make, no GNU extensions
```
