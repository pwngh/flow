# flowc

A compiler frontend for Flow, a small declarative language for typed
agent workflows. Reads a `.flow` source file, type-checks it, and emits
a JSON intermediate representation. Written in portable ISO C99 against
POSIX.1-2008 with `flex` and `bison` as its only build dependencies.

This is the v0.1 reference implementation. It does not generate code in
any host language, link a runtime, or execute flows. Those are jobs for
downstream tooling that reads the IR.

## Building

Requires a C99 compiler, `flex`, and `bison` 3.0 or later. On macOS the
system bison is too old; install a current one via Homebrew and let
`configure` find it on `PATH`.

```
./configure
make
make test
```

`./configure` probes the local toolchain and writes `config.mk`. `make`
builds the `flowc` binary in the project root. `make test` runs the
fixture suite under `tests/fixtures/` and prints a one-line summary
like `PASS 40  FAIL 0`.

## Using

```
flowc tests/fixtures/onboard.flow
```

Writes the Flow IR to standard output. With `-o <file>`, writes to a
file instead. Diagnostics always go to standard error.

The compilation pipeline has five phases. Each is inspectable:

```
flowc --dump=tokens   file.flow    # token stream
flowc --dump=ast      file.flow    # parsed AST
flowc --dump=resolved file.flow    # post name resolution
flowc --dump=checked  file.flow    # post type checking
flowc --dump=ir       file.flow    # the emitted IR
flowc --check-only    file.flow    # validate, suppress IR
flowc --help                       # full option list
```

## Reproducibility

The IR is byte-deterministic for a given input. `compiled_at` honors
[`SOURCE_DATE_EPOCH`](https://reproducible-builds.org/specs/source-date-epoch/);
the test driver pins it to zero so golden files stay stable across
machines. `LC_NUMERIC` is forced to `"C"` at startup so `%g` produces
the same bytes regardless of the inherited locale.

## Project layout

```
src/             # C99 source: lex.l, parse.y, ast/resolve/check/ir + util
tests/fixtures/  # .flow inputs and expected.{out,err,exit} goldens
tests/api/       # C API tests against libflowc.a, run by `make test`
tests/corpus/    # 20 realistic .flow programs + golden .ir.json,
                 #   shared with downstream tooling in this tree
tests/run.sh     # POSIX sh test driver, invoked by `make test`
configure        # POSIX sh probe script; generates config.mk
Makefile         # POSIX make, no GNU extensions
```
