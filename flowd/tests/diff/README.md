# Differential harness — flowc → flowd round-trip

For a fixed set of `.flow` stems (the sources live in
`../../../flowc/tests/fixtures/`, shared with the flowc test
suite — see "Stems covered" below), the harness:

1. Compiles via `flowc` to produce `ir_version 1.0` IR JSON.
2. Loads through flowd's `--canonical-dump <path>` flag, which
   emits a byte-deterministic JSON canonical form of the loaded
   runtime state (types, tools, flows, variant_index).
3. Diffs against `expected/<stem>.canonical.json`.

The harness is the strongest early-warning system for IR drift
between compiler and runtime. If a flowc refactor changes the
emitted IR in a way the loader handles differently — even if both
sides still produce valid output — the canonical-dump diff will
catch it.

The `expected/*.canonical.json` goldens are generated once by
hand-verifying the first clean run, then frozen. Updating one is
a deliberate, manual step — recompile the stem to IR
(`flowc <stem>.flow -o <stem>.ir.json`), regenerate with
`flowd --canonical-dump <stem>.ir.json`, overwrite the golden, and
review the diff. The harness itself only ever diffs against the
frozen goldens; it never accepts a new result automatically.

The canonical-dump JSON shape is documented in
`src/ir_load.h` under `flowd_canonical_dump_json`. The shape is
distinct from the canonical *value* form in `src/value.c` (which
canonicalizes *values*, not *runtime state*).

## Stems covered

Three picked for shape coverage; add more as new IR shapes appear:

- `onboard` — records, tools, a flow, no variants. The canonical
  v1 example.
- `sum_smoke` — sum type with two variants, variant_index entries.
- `pipeline_v1_smoke` — list types, multi-param flow, pipeline
  bindings (which the canonical-dump intentionally elides — the
  test confirms that elision).
