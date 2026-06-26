# flow

Flow is a small declarative language for typed agent
workflows, a C99 compiler that emits a JSON intermediate
representation, and a C99 runtime that executes the IR and
records the inputs, intermediate values, model identities, and
outputs of every step.

The point of recording all of that is to
let a third party reconstruct, after the fact, why a given
decision came out the way it did — and to re-execute the same
decision against a different model with the inputs captured the
first time, producing a diff against the original outcome.

The compiler, the runtime, the Python and TypeScript bindings, and the VS Code extension all share a diagnostic format, a JSON IR schema, and a fixture corpus, so they live in one tree. A language server and a trace UI are planned and will join the same tree. They release on independent cadences; the IR schema is the seam.

## Quickstart

```sh
./build.sh                                       # build + test flowc and flowd
flowc/flowc examples/hello.flow -o /tmp/hello.ir.json   # compile a flow to IR
flowd/flowd run /tmp/hello.ir.json --input 16          # run it -> 42
```

That is the whole loop: **source → `flowc` → IR → `flowd run` →
output (and a recorded trace)**. New to Flow? Browse
[`examples/`](examples/). Prerequisites and per-subproject builds
are below.

## Project layout

```
flowc/         # ISO C99 compiler frontend; ships `flowc` CLI and libflowc.a
flowd/         # ISO C99 runtime against POSIX.1-2008; ships `flowd` CLI and libflowd.a
examples/      # small, runnable .flow programs to learn from
bindings/
  python/      # Python binding to libflowd via cffi (working)
  typescript/  # TypeScript binding to libflowd via N-API (working)
vscode-flow/   # VS Code extension; grammar and syntax support for .flow files
web/           # WebAssembly playground: compile + run Flow in the browser (see web/README.md)
```

## Building

Each subproject builds on its own; there is no top-level build
system. For convenience, `./build.sh` bootstraps the two C
subprojects in order (and, on macOS, finds a Homebrew Bison if the
system one is too old):

```
./build.sh                 # build + test flowc, then flowd
./build.sh --no-test       # build only
```

To build them by hand — the natural order is `flowc` → `flowd` →
everything else, since the bindings link `libflowd.a` and load IR
produced by `flowc`:

```
(cd flowc       && ./configure && make && make test)
(cd flowd       && ./configure && make && make test)
(cd vscode-flow && ./install.sh)
```

`make test` reports a `PASS`/`FAIL` summary — flowc currently
reports `PASS 56  FAIL 0` and flowd `PASS 43  FAIL 0`.

The bindings under `bindings/python/` and `bindings/typescript/`
are working implementations; each links `libflowd.a` and builds
against it (see their respective READMEs for setup).

Per-subproject toolchain requirements (`flex`, `bison`,
libcurl, OpenSSL libcrypto, Node, Python) are documented in the
subproject READMEs. The C components target Linux x86_64 /
arm64 and macOS x86_64 / arm64; cJSON 1.7.x is vendored at
`flowd/src/cjson/`. CI (`.github/workflows/ci.yml`) builds flowc
and flowd under both GCC and Clang with `-std=c99 -pedantic -Wall
-Wextra -Werror` on every push and pull request, with separate
jobs for the sanitizer matrix (AddressSanitizer + UBSan, then
TSan), the trace/IR byte-identity contract, libFuzzer smoke over
the untrusted-input parsers, mandoc lint, diagnostic-code and
line-coverage ratchets, and a WebAssembly build of the `web/`
playground that is smoke-tested under node.

## Using

Compile a `.flow` source to IR:

```
flowc adjudicate.flow -o adjudicate.ir.json
```

Run the IR. For a quick end-to-end execution from the command
line, `flowd run` executes a flow against deterministic,
type-directed stub tools and writes a full trace:

```
flowd run adjudicate.ir.json --input '{...}'
```

The flow's own logic (expressions, pipelines, conditionals,
`match`) runs for real; only the tool implementations are
stubbed, so this is a smoke/dry execution. With no `--input`,
a default input is synthesized from the flow's parameter types;
`--flow` selects a flow in a multi-flow IR and `--trace-dir`
chooses the trace root (default `./traces`).

For real tool behavior, execution happens through `libflowd`,
directly via its C ABI. (The Python and TypeScript bindings in
`bindings/` wrap that same C ABI and provide a working runtime.)
The host loads the IR with `flowd_load_ir`, registers tool and
model implementations with `flowd_register_tool` /
`flowd_register_model`, calls `flowd_run` with an input, and
receives an output. Either way the runtime writes one trace per
execution under `traces/<flow_name>/<execution_id>/`.

Replay a recorded trace against a different model with the
originally captured inputs. Like execution, replay happens
through the library:

```c
flowd_replay(rt, "adjudicate",
             "traces/adjudicate/exec_2026_05_26_a3f2b1",
             "traces_replay", "openai/gpt-5/2026-11-15");
```

The runtime restores Level 0/1 nodes from the recorded trace
and re-invokes only Level 2 (model) nodes against the new model
with the recorded inputs. Level 3 (mutation) nodes are never
re-invoked: re-running a mutation would repeat its side effect
(resend the email, rewrite the row), so replay restores them
from the trace instead. The IR carries each node's effect level,
so a model upgrade can be re-checked without the caller tracking
which nodes touch the outside world.

Diff an original and its replay to surface per-node
differences — the standalone `flowd` binary takes two trace
directories:

```
flowd diff traces/adjudicate/exec_2026_05_26_a3f2b1 \
           traces_replay/adjudicate/exec_2026_11_20_replay
```

The planned trace UI will render the same data graphically over
any trace directory.

## Reproducibility

The reproducibility contract runs end to end. flowc's IR is
byte-deterministic for a given input. flowd's binary is
byte-identical for a pinned toolchain, and flowd's traces are
byte-identical for identical inputs once the two sources of
per-run variation are pinned: the wall clock
(`SOURCE_DATE_EPOCH`) and the otherwise-random execution-id
suffix (`FLOWD_EXECUTION_ID_SUFFIX` — in production it keeps
concurrent runs' ids unique, so it is pinned only for testing).
A flow that calls a model is nondeterministic by nature and is
excluded from this guarantee. Every downstream consumer —
replay, diff, and the planned trace UI — therefore reads stable
bytes. Reading is not side-effect-free, though: opening a trace
appends an access record to an `audit.log` inside that trace
directory, so consuming a trace adds to it (the recorded
manifest, nodes, and values are unchanged). The test drivers pin
both env vars so goldens stay stable across machines.

## License

MIT. See [LICENSE](LICENSE).
