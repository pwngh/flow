/* web/flowd_wasm.c — the flowd half of the playground, exported to JS.
 *
 * Loads IR, stubs every tool (stub_host), and runs the first flow. Links
 * libflowd only — built with -DFLOWD_BUILTIN_SHA256 and without the anthropic
 * adapter, so it pulls in neither OpenSSL nor libcurl. The run is offline and
 * side-effect-free: the flow's logic executes for real, the leaf tools return
 * type-directed defaults.
 */
#include "flowd.h"
#include "ir_load.h"     /* flow_t, flowd_flow_*, tool_t, flowd_tool_* */
#include "stub_host.h"
#include "cjson/cJSON.h"

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_err[2048];
static char g_susp[512];

/* Last run's error (empty string if the last run produced output). */
const char *flow_run_error(void) { return g_err; }

/* Last hosted run/resume's suspension token (empty unless it suspended). The
 * page reads this to tell a suspension apart from an error and to resume. */
const char *flow_suspension_token(void) { return g_susp; }

/* Load IR; on failure return NULL with g_err set to the load diagnostic. */
static flowd_runtime *
load_ir_or_err(const char *ir)
{
    flowd_runtime *rt = flowd_load_ir(ir);
    if (rt == NULL) {
        char *e = flowd_last_error_json(NULL);   /* read-and-clear: we own it */
        snprintf(g_err, sizeof g_err, "%s", e ? e : "IR failed to load");
        free(e);
    }
    return rt;
}

/* Run `ir` against type-directed stubs. `input` is the flow input as JSON, or
 * empty to synthesize a default from the parameter types. `trace_dir` is where
 * to write the provenance trace, or empty for no trace. Returns the malloc'd
 * output (JS frees it) or NULL with flow_run_error() set. */
char *
flow_run(const char *ir, const char *input, const char *trace_dir)
{
    g_err[0] = '\0';
    g_susp[0] = '\0';            /* keep the token invariant even for the non-hosted path */

    flowd_runtime *rt = load_ir_or_err(ir);
    if (rt == NULL)
        return NULL;

    flowd_install_stub_host(rt);

    const flow_t *flow = flowd_flow_count(rt) > 0 ? flowd_flow_at(rt, 0) : NULL;
    if (flow == NULL) {
        snprintf(g_err, sizeof g_err, "IR declares no flows");
        flowd_destroy(rt);
        return NULL;
    }

    char *in = (input && *input) ? strdup(input)
                                 : flowd_default_input_json(rt, flow);
    const char *td = (trace_dir && *trace_dir) ? trace_dir : NULL;

    char *susp = NULL;
    char *out  = flowd_run_named(rt, flow->name, in, td, &susp);
    if (out == NULL) {
        if (susp != NULL) {
            snprintf(g_err, sizeof g_err,
                     "flow suspended awaiting human approval (token %s)", susp);
            free(susp);
        } else {
            char *e = flowd_last_error_json(rt);  /* read-and-clear: we own it */
            snprintf(g_err, sizeof g_err, "%s", e ? e : "run failed");
            free(e);
        }
    }

    free(in);
    flowd_destroy(rt);
    return out;   /* malloc'd, independent of rt */
}

/* ---------------------------------------------------------------------------
 * flow_run_hosted — run with a JS-backed host instead of the C stub.
 *
 * Every tool's callback asks the page, via globalThis.flowdServeTool(name,
 * args), for an output; when the page declines (returns null/undefined) the
 * tool falls back to the *exact* type-directed stub default. With the page
 * declining everything this is byte-identical to flow_run — it is the seam the
 * editable tool cards (and replay) build on. Still fully offline: the page
 * supplies values, nothing reaches the network.
 * ------------------------------------------------------------------------- */

/* Returns a malloc'd JSON string the runtime then frees, or 0 when the page
 * declines (no globalThis.flowdServeTool, or it returns null/throws). */
EM_JS(char *, flowd_js_serve_tool, (const char *name, const char *args), {
    var f = (typeof globalThis !== "undefined") ? globalThis.flowdServeTool : null;
    if (typeof f !== "function") return 0;
    try {
        var r = f(UTF8ToString(name), UTF8ToString(args));
        if (r === null || r === undefined) return 0;
        var s = (typeof r === "string") ? r : JSON.stringify(r);
        var len = lengthBytesUTF8(s) + 1;
        var ptr = _malloc(len);
        stringToUTF8(s, ptr, len);
        return ptr;
    } catch (e) {
        return 0;   /* a throwing host declines (-> stub default), never aborts the run */
    }
});

/* user_ctx for a non-model tool: just the runtime (for the fallback default)
 * and the tool name (runtime-owned, valid for the run). */
typedef struct { flowd_runtime *rt; const char *name; } js_tool_ctx;

static char *
js_serve_or_default(flowd_runtime *rt, const char *name, const char *args)
{
    char *out = flowd_js_serve_tool(name, args ? args : "{}");
    if (out != NULL) return out;
    out = flowd_stub_default_for_tool(rt, name);          /* page declined -> stub default */
    if (out != NULL) return out;
    out = malloc(5u);
    if (out) memcpy(out, "null", 5u);
    return out;
}

static char *
js_tool_impl(const char *args_json, char **err_json, void *user_ctx)
{
    (void)err_json;
    js_tool_ctx *c = (js_tool_ctx *)user_ctx;
    return js_serve_or_default(c->rt, c->name, args_json);
}

/* Model tools and `pick using` selectors go through one provider, exactly like
 * the stub gateway: a model-effect tool is found by its model id and served by
 * name; a `pick` request (no tool carries the id) falls through to index 0. */
typedef struct { flowd_runtime *rt; } js_gw_ctx;

static const char *js_gw_name(void) { return "flowd-stub"; }
static int js_gw_supports(const char *model_id, void *ctx)
{ (void)model_id; (void)ctx; return 1; }

static char *
js_gw_invoke(const char *model_id, const char *req_json, char **err_msg, void *ctx)
{
    (void)err_msg;
    js_gw_ctx *c = (js_gw_ctx *)ctx;
    size_t nt = flowd_tool_count(c->rt);
    for (size_t i = 0; i < nt; i++) {
        const tool_t *t = flowd_tool_at(c->rt, i);
        if (t->level == EFFECT_MODEL && t->model_id != NULL
            && strcmp(t->model_id, model_id) == 0)
            return js_serve_or_default(c->rt, t->name, req_json);
    }
    char *s = malloc(12u);
    if (s) memcpy(s, "{\"index\":0}", 12u);
    return s;
}

/* The malloc'd state behind a JS host: per-tool contexts plus the model
 * provider. Shared by flow_run_hosted and flow_replay_hosted; freed after the
 * run/replay (the callbacks only fire while flowd is executing, never after). */
typedef struct {
    js_tool_ctx **ctxs;
    size_t        nt;
    js_gw_ctx                *gctx;
    flowd_provider_adapter_t *gw;
} js_host;

/* Register a JS-backed impl for every tool. Returns 0, or -1 on OOM with g_err
 * set (the runtime is left for the caller to destroy). */
static int
install_js_host(flowd_runtime *rt, js_host *h)
{
    h->nt = flowd_tool_count(rt);
    h->ctxs = NULL;
    h->gctx = NULL;
    h->gw = NULL;
    if (h->nt > 0) {
        h->ctxs = calloc(h->nt, sizeof *h->ctxs);
        if (h->ctxs == NULL) { snprintf(g_err, sizeof g_err, "out of memory"); return -1; }
    }
    for (size_t i = 0; i < h->nt; i++) {
        const tool_t *t = flowd_tool_at(rt, i);
        if (t->level == EFFECT_MODEL) continue;
        flowd_effect_level lvl =
            t->level == EFFECT_PURE          ? FLOWD_EFFECT_PURE :
            t->level == EFFECT_DETERMINISTIC ? FLOWD_EFFECT_DETERMINISTIC :
                                               FLOWD_EFFECT_MUTATION;
        js_tool_ctx *c = malloc(sizeof *c);
        if (c == NULL) continue;       /* leave it unregistered: a clean error, not a crash */
        c->rt = rt;
        c->name = t->name;             /* runtime-owned, valid for the run */
        h->ctxs[i] = c;
        flowd_register_tool(rt, t->name, lvl, "", js_tool_impl, "hosted", c);
    }
    h->gctx = malloc(sizeof *h->gctx);
    h->gw   = malloc(sizeof *h->gw);
    if (h->gctx == NULL || h->gw == NULL) {
        snprintf(g_err, sizeof g_err, "out of memory");
        free(h->gctx); h->gctx = NULL;
        free(h->gw);   h->gw = NULL;
        if (h->ctxs) { for (size_t i = 0; i < h->nt; i++) free(h->ctxs[i]); free(h->ctxs); h->ctxs = NULL; }
        return -1;
    }
    h->gctx->rt = rt;
    h->gw->provider_name       = js_gw_name;
    h->gw->supports_model      = js_gw_supports;
    h->gw->invoke              = js_gw_invoke;
    h->gw->invoke_with_metrics = NULL;
    h->gw->user_ctx            = h->gctx;
    flowd_register_provider(rt, h->gw);
    return 0;
}

static void
free_js_host(js_host *h)
{
    if (h->ctxs) { for (size_t i = 0; i < h->nt; i++) free(h->ctxs[i]); free(h->ctxs); }
    free(h->gctx);
    free(h->gw);
}

char *
flow_run_hosted(const char *ir, const char *input, const char *trace_dir)
{
    g_err[0] = '\0';
    g_susp[0] = '\0';

    flowd_runtime *rt = load_ir_or_err(ir);
    if (rt == NULL)
        return NULL;

    js_host host;
    if (install_js_host(rt, &host) != 0) { flowd_destroy(rt); return NULL; }

    const flow_t *flow = flowd_flow_count(rt) > 0 ? flowd_flow_at(rt, 0) : NULL;
    char *out = NULL;
    if (flow == NULL) {
        snprintf(g_err, sizeof g_err, "IR declares no flows");
    } else {
        char *in = (input && *input) ? strdup(input)
                                     : flowd_default_input_json(rt, flow);
        const char *td = (trace_dir && *trace_dir) ? trace_dir : NULL;
        char *susp = NULL;
        out = flowd_run_named(rt, flow->name, in, td, &susp);
        if (out == NULL) {
            if (susp != NULL) {        /* not an error: the token rides in g_susp */
                snprintf(g_susp, sizeof g_susp, "%s", susp);
                free(susp);
            } else {
                char *e = flowd_last_error_json(rt);
                snprintf(g_err, sizeof g_err, "%s", e ? e : "run failed");
                free(e);
            }
        }
        free(in);
    }

    flowd_destroy(rt);
    free_js_host(&host);
    return out;
}

/* Model-versioned replay against a recorded trace, served by the same JS host.
 * `model_id` non-empty re-invokes every model_call with the recorded inputs —
 * the page passes the IR's model id so the edited tool card supplies the new
 * answer, while deterministic/mutation nodes are restored from the trace (never
 * re-fired). Writes a new trace under `new_trace_dir`; returns the replay output
 * (JS frees) or NULL with flow_run_error() set. */
char *
flow_replay_hosted(const char *ir, const char *flow_name,
                   const char *orig_trace_dir, const char *new_trace_dir,
                   const char *model_id)
{
    g_err[0] = '\0';
    g_susp[0] = '\0';            /* replay never suspends; clear so no prior token leaks */

    flowd_runtime *rt = load_ir_or_err(ir);
    if (rt == NULL)
        return NULL;

    js_host host;
    if (install_js_host(rt, &host) != 0) { flowd_destroy(rt); return NULL; }

    const char *mid = (model_id && *model_id) ? model_id : NULL;
    char *out = flowd_replay(rt, flow_name, orig_trace_dir, new_trace_dir, mid);
    if (out == NULL) {
        char *e = flowd_last_error_json(rt);
        snprintf(g_err, sizeof g_err, "%s", e ? e : "replay failed");
        free(e);
    }

    flowd_destroy(rt);
    free_js_host(&host);
    return out;
}

/* Resume a suspended run with a human decision, served by the same JS host.
 * `decision` must conform to the suspended tool's declared output type (R155
 * otherwise). Writes a new sibling trace under `trace_dir` linked to the
 * suspended one via `resumed_from`; the suspended trace stays "suspended".
 * Returns the resumed output (JS frees), or NULL — flow_suspension_token() is
 * set if it suspended again, else flow_run_error() carries the cause. */
char *
flow_resume_hosted(const char *ir, const char *token, const char *decision,
                   const char *trace_dir)
{
    g_err[0] = '\0';
    g_susp[0] = '\0';

    flowd_runtime *rt = load_ir_or_err(ir);
    if (rt == NULL)
        return NULL;

    js_host host;
    if (install_js_host(rt, &host) != 0) { flowd_destroy(rt); return NULL; }

    (void)trace_dir;   /* flowd_resume derives the new trace dir from the token */
    char *susp = NULL;
    char *out = flowd_resume(rt, token, decision, &susp);
    if (out == NULL) {
        if (susp != NULL) {            /* suspended again: chain via g_susp */
            snprintf(g_susp, sizeof g_susp, "%s", susp);
            free(susp);
        } else {
            char *e = flowd_last_error_json(rt);
            snprintf(g_err, sizeof g_err, "%s", e ? e : "resume failed");
            free(e);
        }
    }

    flowd_destroy(rt);
    free_js_host(&host);
    return out;
}

/* The type-directed default for every tool in `ir`, as a JSON object mapping
 * tool name -> default output value. The page uses it to pre-fill the editable
 * tool cards with exactly what an unedited run produces. Returns malloc'd JSON
 * (JS frees) or NULL if the IR fails to load. */
char *
flow_tool_defaults(const char *ir)
{
    flowd_runtime *rt = flowd_load_ir(ir);
    if (rt == NULL) return NULL;

    cJSON *map = cJSON_CreateObject();
    size_t nt = flowd_tool_count(rt);
    for (size_t i = 0; i < nt; i++) {
        const tool_t *t = flowd_tool_at(rt, i);
        char *def = flowd_stub_default_for_tool(rt, t->name);
        cJSON *v = def ? cJSON_Parse(def) : NULL;
        free(def);
        cJSON_AddItemToObject(map, t->name, v ? v : cJSON_CreateNull());
    }
    char *out = cJSON_PrintUnformatted(map);
    cJSON_Delete(map);
    flowd_destroy(rt);
    return out;
}
