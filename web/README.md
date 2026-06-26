# Flow playground (WebAssembly)

A static page that compiles and runs Flow entirely in the browser: edit a
`.flow` program, see its IR, its output, and the recorded trace. No server.

## Why it's safe to run untrusted input

`flowd run` here uses **type-directed stub tools** (`flowd/src/stub_host.c`):
the flow's own logic — bindings, pipelines, `match`, branches — executes for
real, but every leaf tool returns a default value of its declared output type.
There is no network and no side effect, so evaluating an arbitrary program is a
pure, deterministic computation. The only thing worth bounding is CPU/memory.

## How it's built

Two WebAssembly modules, because flowc and flowd each define their own
`arena_`/`diag_` symbols and would collide in one link — and because the IR is
the seam between them, exactly as in the native CLI chain:

```
source ──▶ flowc.wasm (flow_compile) ──▶ IR ──▶ flowd.wasm (flow_run) ──▶ output + trace
```

`flowd.wasm` is compiled with `-DFLOWD_BUILTIN_SHA256` (the vendored SHA-256 in
`flowd/src/sha256_builtin.c`, byte-identical to OpenSSL — the test suite checks
this) and **without** the anthropic adapter, so it pulls in neither OpenSSL nor
libcurl. The trace is written to Emscripten's in-memory filesystem and read
back by the page.

The glue is `web/flowc_wasm.c` and `web/flowd_wasm.c` — one small entry point
each. It is exercised natively (linked against `libflowc.a` / `libflowd.a`) as
part of normal development, so the WASM build differs only in the compiler.

## Build & serve

Needs [Emscripten](https://emscripten.org) (`emcc`) plus `flex` and
`bison >= 3` (the same parser-generator the native build uses).

```sh
web/build.sh                 # → web/flowc.{js,wasm}, web/flowd.{js,wasm}
python3 -m http.server -d web # then open http://localhost:8000
```

A `file://` open will not work — the modules are fetched over HTTP, so the page
must be served.
