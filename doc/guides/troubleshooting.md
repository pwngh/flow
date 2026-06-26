# Troubleshooting

Common problems, in the order you're likely to hit them.

## Building

**`configure: error: GNU Bison >= 3.0` (or the build fails in `parse.y`)**
macOS ships Bison 2.3, which is too old. Install a current one and let
`configure` find it:

```sh
brew install bison
export PATH="$(brew --prefix bison)/bin:$PATH"
```

Or point at it explicitly: `YACC=$(brew --prefix bison)/bin/bison ./configure`.
`./build.sh` does this for you automatically.

**`configure: error: OpenSSL libcrypto with SHA256 is required`**
Install OpenSSL and re-run: `brew install openssl@3` (macOS) or
`sudo apt-get install libssl-dev` (Debian/Ubuntu). `configure` probes
the Homebrew prefixes automatically.

**`configure: error: ... flex ...`**
Install `flex` (`brew install flex` / `apt-get install flex`).

**libcurl not found**
`brew install curl` (macOS) or `apt-get install libcurl4-openssl-dev`.

## Compiling a `.flow` (flowc)

Diagnostics are `file:line:col: severity[CODE]: message`. A few common ones:

| Code | Meaning | Fix |
|------|---------|-----|
| `E110` | syntax error | check the grammar in `man doc/man5/flow.5`; e.g. records are `type T = { ... }` (the `=` is required), tools need an `effect` clause |
| `E124` | reference to unknown name | the binding/parameter isn't in scope |
| `E198` | tool missing `effect` clause | add `effect deterministic` / `model("…")` / `mutation` / `pure` under the tool |
| `E140`/`E144` | type mismatch | return/argument type doesn't match the declaration |

Get the full diagnostic as JSON with `-fdiagnostics-format=json`, and
stop after N errors with `-fmax-errors=N` (default 1).

Reserved words can't be used as identifiers or field names — notably
`count`, `sum`, `min`, `max`, `initial`, `row`, `it`. Full list in
`man doc/man5/flow.5`.

## Running a flow (flowd run)

**`R155: ... missing field '…'` / `expected int` etc.**
Your `--input` doesn't match the flow's declared input type. Omit
`--input` to let `flowd run` synthesize a type-correct default, or match
the type exactly. Inspect the expected shape with
`flowd --load-ir <ir>` or `flowc --dump=checked <src>`.

**`R110: pick on empty list`**
The flow ran a `pick`/`top` on an empty list. Under `flowd run`, tool
outputs are stubs, and a `where` stage may filter the single stub row to
nothing. This is correct runtime behavior, not a bug — supply real data
by registering tools via `libflowd`, or feed an input that survives the
filter.

**`R101: no implementation registered for tool '…'`**
You're executing via the C API (`flowd_run`) without registering that
tool. `flowd run` registers stubs for you; a host program must call
`flowd_register_tool` / `flowd_register_model` for each tool.

## Still stuck?

- The language reference is `man doc/man5/flow.5`; the IR and trace
  formats are `man doc/man5/flow-ir.5` and `man doc/man5/flow-trace.5`.
- Real, compiling programs to learn from: `flowc/tests/corpus/`.
