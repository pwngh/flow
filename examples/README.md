# Examples

Small, self-contained Flow programs you can compile and run immediately.
Each is heavily commented. Build the tools first (`./build.sh` from the
repo root), then:

```sh
flowc/flowc examples/hello.flow -o /tmp/hello.ir.json
flowd/flowd run /tmp/hello.ir.json --input 16     # -> 42
```

| File | Shows | Try |
|------|-------|-----|
| [`hello.flow`](hello.flow)   | a flow with no tools — pure in-language computation | `flowd run hello.ir.json --input 16` → `42` |
| [`triage.flow`](triage.flow) | types, a model-backed tool with an `effect` clause, and an `if/else` branch; writes a trace with a model_call node | `flowd run triage.ir.json --input 750` → `2` |

`flowd run` registers a deterministic **stub** for every declared tool,
so these run with no host code. For real tool behavior, link `libflowd`
and register implementations — see [../flowd/README.md](../flowd/README.md).

For the language reference see `man ../doc/man5/flow.5`. For a tour of
larger, realistic programs, see `../flowc/tests/corpus/`.
