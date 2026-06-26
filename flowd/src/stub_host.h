/* src/stub_host.h
 *
 * Type-directed stub host. Installs an implementation for every tool in a
 * loaded IR so a flow runs end to end with no host code: each tool returns a
 * default value of its declared output type. This is what makes `flowd run`
 * and the WASM playground executable offline — deterministic, no provider, no
 * side effects. The flow's own logic (bindings, pipelines, match, branches)
 * still runs for real; only the leaf tools are stand-ins.
 */
#ifndef FLOWD_STUB_HOST_H
#define FLOWD_STUB_HOST_H

#include "flowd.h"
#include "ir_load.h"   /* flow_t */

/* Install stubs for every tool on a freshly loaded runtime: non-model tools
 * via flowd_register_tool, model tools and `pick using` selectors via a stub
 * gateway adapter. All state is arena-owned (lives until flowd_destroy), so
 * there is nothing to free. Call once, after load and before flowd_run. */
void flowd_install_stub_host(flowd_runtime *rt);

/* Synthesize a flow's default input from its parameter types: a bare value
 * for the implicit-`it` form, else an object keyed by parameter name. Heap
 * string, caller frees. */
char *flowd_default_input_json(flowd_runtime *rt, const flow_t *flow);

/* The type-directed default JSON for a named tool's declared output (heap
 * string, caller frees), or NULL if no tool by that name. Lets an alternative
 * host (e.g. the WASM playground's JS-backed one) fall back to the same value
 * the stub would produce when it has no override for a tool. */
char *flowd_stub_default_for_tool(flowd_runtime *rt, const char *tool_name);

#endif /* FLOWD_STUB_HOST_H */
