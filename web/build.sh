#!/bin/sh
# web/build.sh — build the two WebAssembly modules behind the playground.
#
# Two modules, not one: flowc and flowd each define their own arena_/diag_
# symbols, so linking both into a single module collides. Keeping them apart
# also mirrors the native tools — the IR string is the seam between them.
#
# flowd is built with -DFLOWD_BUILTIN_SHA256 and without the anthropic adapter,
# so it links neither OpenSSL nor libcurl: nothing a browser lacks. The flow's
# logic runs for real against type-directed stub tools (stub_host).
set -eu
cd "$(dirname "$0")/.."

command -v emcc >/dev/null 2>&1 || {
    echo "emcc not found. Install Emscripten (https://emscripten.org) and"
    echo "activate it (emsdk_env), then re-run web/build.sh."
    exit 1
}

# The flex/bison sources are gitignored build artifacts; regenerate them.
# flowc needs bison >= 3, so prefer a Homebrew one where the system bison is 2.3.
bison_major=$(bison --version 2>/dev/null | sed -n '1s/.*) \([0-9]*\).*/\1/p')
BISON=bison
if [ "${bison_major:-0}" -lt 3 ]; then
    for p in /opt/homebrew/opt/bison/bin /usr/local/opt/bison/bin; do
        [ -x "$p/bison" ] && BISON="$p/bison" && break
    done
fi
"$BISON" -d -o flowc/src/parse.tab.c flowc/src/parse.y
flex -o flowc/src/lex.yy.c flowc/src/lex.l

COMMON="-O2 -D_POSIX_C_SOURCE=200809L -s MODULARIZE=1 -s ALLOW_MEMORY_GROWTH=1 -s ENVIRONMENT=web,node"
RT="['ccall','UTF8ToString','stringToUTF8','lengthBytesUTF8']"

echo "building web/flowc.wasm ..."
emcc $COMMON -Iflowc/src \
    -s EXPORT_NAME=createFlowc \
    -s "EXPORTED_FUNCTIONS=['_flow_compile','_flow_compile_error','_malloc','_free']" \
    -s "EXPORTED_RUNTIME_METHODS=$RT" \
    web/flowc_wasm.c \
    flowc/src/api.c flowc/src/util.c flowc/src/ast.c flowc/src/ast_dump.c \
    flowc/src/resolve.c flowc/src/check.c flowc/src/ir.c \
    flowc/src/parse.tab.c flowc/src/lex.yy.c \
    -o web/flowc.js

echo "building web/flowd.wasm ..."
emcc $COMMON -Iflowd/src -DFLOWD_BUILTIN_SHA256 \
    -s EXPORT_NAME=createFlowd -s FORCE_FILESYSTEM=1 \
    -s "EXPORTED_FUNCTIONS=['_flow_run','_flow_run_hosted','_flow_replay_hosted','_flow_resume_hosted','_flow_tool_defaults','_flow_run_error','_flow_suspension_token','_malloc','_free']" \
    -s "EXPORTED_RUNTIME_METHODS=['ccall','UTF8ToString','stringToUTF8','lengthBytesUTF8','FS']" \
    web/flowd_wasm.c \
    flowd/src/util.c flowd/src/sha256_builtin.c flowd/src/stub_host.c \
    flowd/src/value.c flowd/src/ir_load.c flowd/src/exec.c flowd/src/trace.c \
    flowd/src/gateway.c flowd/src/flowd.c flowd/src/cjson/cJSON.c \
    -o web/flowd.js

echo "done. serve this directory and open index.html, e.g.:  python3 -m http.server"
