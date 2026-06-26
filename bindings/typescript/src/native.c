/*
 * native.c — N-API addon bridging Node to libflowd's C ABI.
 *
 * Static-links flowd/libflowd.a (see binding.gyp). The addon is
 * string-in/string-out: JSON marshalling lives in index.ts, so the
 * native layer only moves UTF-8 JSON across the boundary.
 *
 * Tool/model callbacks are invoked SYNCHRONOUSLY: flowd_run is called
 * on the JS thread and re-enters JS via napi_call_function for each
 * node. The registered JS function must therefore be synchronous
 * (return a JSON string). The async worker-thread path described in
 * the README is not wired here.
 */

#include <node_api.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "flowd.h"

/* heap-dup a C string with malloc (the runtime free()s it). */
static char *owned_dup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* ---- small helpers ------------------------------------------------ */

/* Throw a JS Error and return NULL (for use in native methods). */
static napi_value throw_err(napi_env env, const char *msg) {
    napi_throw_error(env, NULL, msg);
    return NULL;
}

/* Read a JS string argument into a freshly malloc'd UTF-8 buffer.
 * Returns NULL for null/undefined. Caller frees with free(). */
static char *js_to_cstr(napi_env env, napi_value v) {
    napi_valuetype t;
    if (napi_typeof(env, v, &t) != napi_ok) return NULL;
    if (t == napi_null || t == napi_undefined) return NULL;
    size_t len = 0;
    if (napi_get_value_string_utf8(env, v, NULL, 0, &len) != napi_ok) return NULL;
    char *buf = (char *)malloc(len + 1);
    if (!buf) return NULL;
    if (napi_get_value_string_utf8(env, v, buf, len + 1, &len) != napi_ok) {
        free(buf);
        return NULL;
    }
    return buf;
}

static napi_value cstr(napi_env env, const char *s) {
    napi_value v;
    napi_create_string_utf8(env, s ? s : "", NAPI_AUTO_LENGTH, &v);
    return v;
}

/* ---- runtime wrapper ---------------------------------------------- */

/* Per-callback context: keeps a strong ref to the JS function so the
 * C trampoline can call it. user_ctx for flowd_register_*. */
typedef struct {
    napi_env env;
    napi_ref fn_ref;
} cb_ctx_t;

/* Provider adapter context: supports_model + invoke each need a JS fn;
 * metrics_ref is optional (the v2 invoke_with_metrics path). */
typedef struct {
    napi_env env;
    napi_ref supports_ref;
    napi_ref invoke_ref;
    napi_ref metrics_ref;   /* NULL when the adapter reports no metrics */
} provider_ctx_t;

typedef struct {
    flowd_runtime *rt;
    napi_env       env;
    cb_ctx_t     **ctxs;   /* tool/model + redactor closures */
    size_t         n_ctxs;
    size_t         cap_ctxs;
    /* provider adapters (borrowed by the runtime; we own the storage) */
    provider_ctx_t **pctxs;
    void           **padapters;   /* malloc'd flowd_provider_adapter_t* */
    size_t           n_prov;
    int            destroyed;
} rt_wrap_t;

static cb_ctx_t *wrap_add_ctx(rt_wrap_t *w, napi_env env, napi_value fn) {
    if (w->n_ctxs == w->cap_ctxs) {
        size_t ncap = w->cap_ctxs ? w->cap_ctxs * 2 : 8;
        cb_ctx_t **g = (cb_ctx_t **)realloc(w->ctxs, ncap * sizeof *g);
        if (!g) return NULL;
        w->ctxs = g;
        w->cap_ctxs = ncap;
    }
    cb_ctx_t *c = (cb_ctx_t *)malloc(sizeof *c);
    if (!c) return NULL;
    c->env = env;
    napi_create_reference(env, fn, 1, &c->fn_ref);
    w->ctxs[w->n_ctxs++] = c;
    return c;
}

static void wrap_teardown(rt_wrap_t *w) {
    if (!w || w->destroyed) return;
    w->destroyed = 1;
    if (w->rt) {
        flowd_destroy(w->rt);
        w->rt = NULL;
    }
    for (size_t i = 0; i < w->n_ctxs; i++) {
        if (w->ctxs[i]) {
            napi_delete_reference(w->ctxs[i]->env, w->ctxs[i]->fn_ref);
            free(w->ctxs[i]);
        }
    }
    free(w->ctxs);
    w->ctxs = NULL;
    w->n_ctxs = w->cap_ctxs = 0;
    for (size_t i = 0; i < w->n_prov; i++) {
        if (w->pctxs && w->pctxs[i]) {
            napi_delete_reference(w->pctxs[i]->env, w->pctxs[i]->supports_ref);
            napi_delete_reference(w->pctxs[i]->env, w->pctxs[i]->invoke_ref);
            if (w->pctxs[i]->metrics_ref)
                napi_delete_reference(w->pctxs[i]->env, w->pctxs[i]->metrics_ref);
            free(w->pctxs[i]);
        }
        if (w->padapters && w->padapters[i]) free(w->padapters[i]);
    }
    free(w->pctxs);
    free(w->padapters);
    w->pctxs = NULL;
    w->padapters = NULL;
    w->n_prov = 0;
}

static void rt_finalize(napi_env env, void *data, void *hint) {
    (void)env; (void)hint;
    rt_wrap_t *w = (rt_wrap_t *)data;
    wrap_teardown(w);
    free(w);
}

/* The shared tool/model trampoline. user_ctx is a cb_ctx_t*. */
static char *tool_trampoline(const char *args_json, char **err_json,
                             void *user_ctx) {
    cb_ctx_t *ctx = (cb_ctx_t *)user_ctx;
    napi_env env = ctx->env;
    napi_handle_scope scope;
    napi_open_handle_scope(env, &scope);

    napi_value fn, undef, arg, result;
    napi_get_reference_value(env, ctx->fn_ref, &fn);
    napi_get_undefined(env, &undef);
    napi_create_string_utf8(env, args_json ? args_json : "{}",
                            NAPI_AUTO_LENGTH, &arg);

    napi_status st = napi_call_function(env, undef, fn, 1, &arg, &result);

    char *ret = NULL;
    if (st != napi_ok) {
        /* The JS callback threw. Must clear, not just read: a pending JS
         * exception would poison the next N-API call, and flowd_run re-enters
         * JS for every later node. The real JS message is NOT propagated --
         * index.ts's wrap has no try/catch -- so the trace records the generic
         * R155 below; exc is discarded deliberately. */
        napi_value exc;
        napi_get_and_clear_last_exception(env, &exc);
        if (err_json) *err_json = owned_dup(
            "{\"severity\":\"error\",\"id\":\"R155\","
            "\"message\":\"tool callback threw\"}");
        napi_close_handle_scope(env, scope);
        return NULL;
    }

    ret = js_to_cstr(env, result);
    if (!ret && err_json) {
        *err_json = owned_dup(
            "{\"severity\":\"error\",\"id\":\"R155\",\"message\":"
            "\"tool callback did not return a JSON string\"}");
    }
    napi_close_handle_scope(env, scope);
    return ret;
}

/* Redactor trampoline: JS fn (Buffer) -> Buffer | null. */
static char *redactor_trampoline(const char *bytes, size_t len,
                                 size_t *out_len, void *user_ctx) {
    cb_ctx_t *ctx = (cb_ctx_t *)user_ctx;
    napi_env env = ctx->env;
    napi_handle_scope scope;
    napi_open_handle_scope(env, &scope);

    napi_value fn, undef, arg, result;
    napi_get_reference_value(env, ctx->fn_ref, &fn);
    napi_get_undefined(env, &undef);
    void *copy = NULL;
    napi_create_buffer_copy(env, len, bytes, &copy, &arg);

    char *ret = NULL;
    if (napi_call_function(env, undef, fn, 1, &arg, &result) == napi_ok) {
        bool is_buf = false;
        napi_is_buffer(env, result, &is_buf);
        if (is_buf) {
            void *data = NULL;
            size_t dlen = 0;
            if (napi_get_buffer_info(env, result, &data, &dlen) == napi_ok) {
                ret = (char *)malloc(dlen ? dlen : 1);
                if (ret) {
                    if (dlen) memcpy(ret, data, dlen);
                    if (out_len) *out_len = dlen;
                }
            }
        }
        /* non-buffer (e.g. null) -> leave bytes untouched (return NULL) */
    } else {
        napi_value exc;
        napi_get_and_clear_last_exception(env, &exc);  /* swallow; leave untouched */
    }
    napi_close_handle_scope(env, scope);
    return ret;
}

/* ---- exported native methods -------------------------------------- */

static rt_wrap_t *unwrap(napi_env env, napi_value v) {
    void *data = NULL;
    if (napi_get_value_external(env, v, &data) != napi_ok) return NULL;
    return (rt_wrap_t *)data;
}

/* loadIr(irJson: string) -> External */
static napi_value n_load_ir(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (argc < 1) return throw_err(env, "loadIr(irJson) requires 1 argument");
    char *ir = js_to_cstr(env, argv[0]);
    if (!ir) return throw_err(env, "loadIr: irJson must be a string");

    flowd_runtime *rt = flowd_load_ir(ir);
    free(ir);
    if (!rt) {
        char *err = flowd_last_error_json(NULL);
        napi_value e = throw_err(env, err ? err : "flowd_load_ir failed");
        free(err);
        return e;
    }
    rt_wrap_t *w = (rt_wrap_t *)calloc(1, sizeof *w);
    w->rt = rt;
    w->env = env;
    napi_value ext;
    napi_create_external(env, w, rt_finalize, NULL, &ext);
    return ext;
}

/* registerTool(rt, name, level:int, signature, fn, implVersion) -> int */
static napi_value n_register_tool(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value a[6];
    napi_get_cb_info(env, info, &argc, a, NULL, NULL);
    rt_wrap_t *w = unwrap(env, a[0]);
    if (!w || w->destroyed) return throw_err(env, "registerTool: bad runtime");
    char *name = js_to_cstr(env, a[1]);
    int32_t level = 0;
    napi_get_value_int32(env, a[2], &level);
    char *sig = js_to_cstr(env, a[3]);
    char *ver = js_to_cstr(env, a[5]);
    cb_ctx_t *ctx = wrap_add_ctx(w, env, a[4]);
    int rc = flowd_register_tool(w->rt, name, (flowd_effect_level)level,
                                 sig ? sig : "", tool_trampoline,
                                 ver, ctx);
    free(name); free(sig); free(ver);
    napi_value out;
    napi_create_int32(env, rc, &out);
    return out;
}

/* registerModel(rt, name, signature, fn, implVersion) -> int */
static napi_value n_register_model(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value a[5];
    napi_get_cb_info(env, info, &argc, a, NULL, NULL);
    rt_wrap_t *w = unwrap(env, a[0]);
    if (!w || w->destroyed) return throw_err(env, "registerModel: bad runtime");
    char *name = js_to_cstr(env, a[1]);
    char *sig = js_to_cstr(env, a[2]);
    char *ver = js_to_cstr(env, a[4]);
    cb_ctx_t *ctx = wrap_add_ctx(w, env, a[3]);
    int rc = flowd_register_model(w->rt, name, sig ? sig : "",
                                  tool_trampoline, ver, ctx);
    free(name); free(sig); free(ver);
    napi_value out;
    napi_create_int32(env, rc, &out);
    return out;
}

/* setRedactor(rt, fn|null) -> void */
static napi_value n_set_redactor(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value a[2];
    napi_get_cb_info(env, info, &argc, a, NULL, NULL);
    rt_wrap_t *w = unwrap(env, a[0]);
    if (!w || w->destroyed) return throw_err(env, "setRedactor: bad runtime");
    napi_valuetype t;
    napi_typeof(env, a[1], &t);
    if (t == napi_null || t == napi_undefined) {
        flowd_set_redactor(w->rt, NULL, NULL);
    } else {
        cb_ctx_t *ctx = wrap_add_ctx(w, env, a[1]);
        flowd_set_redactor(w->rt, redactor_trampoline, ctx);
    }
    return NULL;
}

/* Build {status, value?, token?, error?} from a run/resume result. */
static napi_value run_result(napi_env env, rt_wrap_t *w, char *out, char *susp) {
    napi_value obj;
    napi_create_object(env, &obj);
    if (out) {
        napi_set_named_property(env, obj, "status", cstr(env, "ok"));
        napi_set_named_property(env, obj, "value", cstr(env, out));
        free(out);
    } else if (susp) {
        napi_set_named_property(env, obj, "status", cstr(env, "suspended"));
        napi_set_named_property(env, obj, "token", cstr(env, susp));
        free(susp);
    } else {
        char *err = flowd_last_error_json(w->rt);
        napi_set_named_property(env, obj, "status", cstr(env, "error"));
        napi_set_named_property(env, obj, "error",
                                cstr(env, err ? err : "flow execution failed"));
        free(err);
    }
    return obj;
}

/* run(rt, inputJson, traceDir, flowName|null) -> result object */
static napi_value n_run(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value a[4];
    napi_get_cb_info(env, info, &argc, a, NULL, NULL);
    rt_wrap_t *w = unwrap(env, a[0]);
    if (!w || w->destroyed) return throw_err(env, "run: bad runtime");
    char *input = js_to_cstr(env, a[1]);
    char *trace = js_to_cstr(env, a[2]);
    char *flow = js_to_cstr(env, a[3]);
    char *susp = NULL;
    char *out;
    if (flow) {
        out = flowd_run_named(w->rt, flow, input ? input : "null",
                              trace ? trace : "traces", &susp);
    } else {
        out = flowd_run(w->rt, input ? input : "null",
                        trace ? trace : "traces", &susp);
    }
    napi_value res = run_result(env, w, out, susp);
    free(input); free(trace); free(flow);
    return res;
}

/* resume(rt, token, decisionJson) -> result object */
static napi_value n_resume(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value a[3];
    napi_get_cb_info(env, info, &argc, a, NULL, NULL);
    rt_wrap_t *w = unwrap(env, a[0]);
    if (!w || w->destroyed) return throw_err(env, "resume: bad runtime");
    char *token = js_to_cstr(env, a[1]);
    char *decision = js_to_cstr(env, a[2]);
    char *susp = NULL;
    char *out = flowd_resume(w->rt, token ? token : "",
                             decision ? decision : "null", &susp);
    napi_value res = run_result(env, w, out, susp);
    free(token); free(decision);
    return res;
}

/* replay(rt, flowName, originalTraceDir, newTraceDir, newModelId|null) -> result */
static napi_value n_replay(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value a[5];
    napi_get_cb_info(env, info, &argc, a, NULL, NULL);
    rt_wrap_t *w = unwrap(env, a[0]);
    if (!w || w->destroyed) return throw_err(env, "replay: bad runtime");
    char *flow = js_to_cstr(env, a[1]);
    char *orig = js_to_cstr(env, a[2]);
    char *newd = js_to_cstr(env, a[3]);
    char *model = js_to_cstr(env, a[4]);  /* NULL => same-model restore */

    char *out = flowd_replay(w->rt, flow ? flow : "", orig ? orig : "",
                             newd ? newd : "", model);
    /* Replay never suspends, so the ok/error shape is exactly
     * run_result's with a NULL suspension. */
    napi_value res = run_result(env, w, out, NULL);
    free(flow); free(orig); free(newd); free(model);
    return res;
}

/* destroy(rt) -> void */
static napi_value n_destroy(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value a[1];
    napi_get_cb_info(env, info, &argc, a, NULL, NULL);
    rt_wrap_t *w = unwrap(env, a[0]);
    if (w) wrap_teardown(w);
    return NULL;
}

/* ---- provider adapters -------------------------------------------- */

/* provider_name() takes no user_ctx, so a single C function can't tell
 * adapters apart. Use a fixed pool of distinct accessors, each returning
 * its slot's name. Caps concurrently-registered providers per process. */
#define MAX_PROVIDERS 8
static char *g_provider_names[MAX_PROVIDERS];
static int   g_provider_count = 0;

#define MK_PNAME(i) static const char *pname_##i(void) { return g_provider_names[i]; }
MK_PNAME(0) MK_PNAME(1) MK_PNAME(2) MK_PNAME(3)
MK_PNAME(4) MK_PNAME(5) MK_PNAME(6) MK_PNAME(7)
static const char *(*const PNAME_TBL[MAX_PROVIDERS])(void) = {
    pname_0, pname_1, pname_2, pname_3, pname_4, pname_5, pname_6, pname_7,
};

static int provider_supports(const char *model_id, void *user_ctx) {
    provider_ctx_t *ctx = (provider_ctx_t *)user_ctx;
    napi_env env = ctx->env;
    napi_handle_scope scope;
    napi_open_handle_scope(env, &scope);
    napi_value fn, undef, arg, result;
    napi_get_reference_value(env, ctx->supports_ref, &fn);
    napi_get_undefined(env, &undef);
    napi_create_string_utf8(env, model_id ? model_id : "", NAPI_AUTO_LENGTH, &arg);
    int ok = 0;
    if (napi_call_function(env, undef, fn, 1, &arg, &result) == napi_ok) {
        bool b = false;
        napi_get_value_bool(env, result, &b);
        ok = b ? 1 : 0;
    } else {
        napi_value exc;
        napi_get_and_clear_last_exception(env, &exc);
    }
    napi_close_handle_scope(env, scope);
    return ok;
}

static char *provider_invoke(const char *model_id, const char *request_json,
                             char **err_msg, void *user_ctx) {
    provider_ctx_t *ctx = (provider_ctx_t *)user_ctx;
    napi_env env = ctx->env;
    napi_handle_scope scope;
    napi_open_handle_scope(env, &scope);
    napi_value fn, undef, argv[2], result;
    napi_get_reference_value(env, ctx->invoke_ref, &fn);
    napi_get_undefined(env, &undef);
    napi_create_string_utf8(env, model_id ? model_id : "", NAPI_AUTO_LENGTH, &argv[0]);
    napi_create_string_utf8(env, request_json ? request_json : "{}",
                            NAPI_AUTO_LENGTH, &argv[1]);
    char *ret = NULL;
    if (napi_call_function(env, undef, fn, 2, argv, &result) == napi_ok) {
        ret = js_to_cstr(env, result);
        if (!ret && err_msg)
            *err_msg = owned_dup("provider invoke did not return a string");
    } else {
        napi_value exc;
        napi_get_and_clear_last_exception(env, &exc);
        if (err_msg) *err_msg = owned_dup("provider invoke threw");
    }
    napi_close_handle_scope(env, scope);
    return ret;
}

/* v2 path: JS metrics fn returns {response: string, tokensIn, tokensOut,
 * costCents}; fill the adapter response struct the runtime owns. */
static void provider_invoke_metrics(const char *model_id, const char *request_json,
                                    flowd_adapter_response_t *result, void *user_ctx) {
    provider_ctx_t *ctx = (provider_ctx_t *)user_ctx;
    napi_env env = ctx->env;
    napi_handle_scope scope;
    napi_open_handle_scope(env, &scope);
    napi_value fn, undef, argv[2], ret, v;
    napi_get_reference_value(env, ctx->metrics_ref, &fn);
    napi_get_undefined(env, &undef);
    napi_create_string_utf8(env, model_id ? model_id : "", NAPI_AUTO_LENGTH, &argv[0]);
    napi_create_string_utf8(env, request_json ? request_json : "{}", NAPI_AUTO_LENGTH, &argv[1]);

    if (napi_call_function(env, undef, fn, 2, argv, &ret) != napi_ok) {
        napi_value exc;
        napi_get_and_clear_last_exception(env, &exc);
        result->response_json = NULL;
        result->err_msg = owned_dup("provider invokeWithMetrics threw");
        napi_close_handle_scope(env, scope);
        return;
    }
    napi_get_named_property(env, ret, "response", &v);
    result->response_json = js_to_cstr(env, v);
    if (!result->response_json) {
        result->err_msg = owned_dup("invokeWithMetrics: response was not a string");
        napi_close_handle_scope(env, scope);
        return;
    }
    result->err_msg = NULL;
    int64_t ti = 0, to = 0;
    double cc = 0.0;
    napi_get_named_property(env, ret, "tokensIn", &v);
    napi_get_value_int64(env, v, &ti);
    napi_get_named_property(env, ret, "tokensOut", &v);
    napi_get_value_int64(env, v, &to);
    napi_get_named_property(env, ret, "costCents", &v);
    napi_get_value_double(env, v, &cc);
    result->tokens_in = ti < 0 ? 0 : (uint64_t)ti;
    result->tokens_out = to < 0 ? 0 : (uint64_t)to;
    result->cost_cents = cc;
    napi_close_handle_scope(env, scope);
}

/* registerProvider(rt, name, supportsFn, invokeFn, metricsFn|null) -> int */
static napi_value n_register_provider(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value a[5];
    napi_get_cb_info(env, info, &argc, a, NULL, NULL);
    rt_wrap_t *w = unwrap(env, a[0]);
    if (!w || w->destroyed) return throw_err(env, "registerProvider: bad runtime");
    if (g_provider_count >= MAX_PROVIDERS)
        return throw_err(env, "registerProvider: too many providers (max 8 per process)");

    int slot = g_provider_count++;
    g_provider_names[slot] = js_to_cstr(env, a[1]);

    provider_ctx_t *ctx = (provider_ctx_t *)malloc(sizeof *ctx);
    ctx->env = env;
    ctx->metrics_ref = NULL;
    napi_create_reference(env, a[2], 1, &ctx->supports_ref);
    napi_create_reference(env, a[3], 1, &ctx->invoke_ref);

    flowd_provider_adapter_t *ad =
        (flowd_provider_adapter_t *)calloc(1, sizeof *ad);
    ad->provider_name = PNAME_TBL[slot];
    ad->supports_model = provider_supports;
    ad->invoke = provider_invoke;
    ad->user_ctx = ctx;
    ad->invoke_with_metrics = NULL;

    napi_valuetype mt;
    napi_typeof(env, a[4], &mt);
    if (mt == napi_function) {
        napi_create_reference(env, a[4], 1, &ctx->metrics_ref);
        ad->invoke_with_metrics = provider_invoke_metrics;
    }

    /* track for teardown (the adapter is borrowed; storage must persist) */
    size_t i = w->n_prov;
    w->pctxs = (provider_ctx_t **)realloc(w->pctxs, (i + 1) * sizeof *w->pctxs);
    w->padapters = (void **)realloc(w->padapters, (i + 1) * sizeof *w->padapters);
    w->pctxs[i] = ctx;
    w->padapters[i] = ad;
    w->n_prov = i + 1;

    int rc = flowd_register_provider(w->rt, ad);
    napi_value out;
    napi_create_int32(env, rc, &out);
    return out;
}

static napi_value Init(napi_env env, napi_value exports) {
    const struct {
        const char *name;
        napi_callback fn;
    } fns[] = {
        {"loadIr", n_load_ir},
        {"registerTool", n_register_tool},
        {"registerModel", n_register_model},
        {"setRedactor", n_set_redactor},
        {"registerProvider", n_register_provider},
        {"run", n_run},
        {"resume", n_resume},
        {"replay", n_replay},
        {"destroy", n_destroy},
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) {
        napi_value f;
        napi_create_function(env, fns[i].name, NAPI_AUTO_LENGTH, fns[i].fn,
                             NULL, &f);
        napi_set_named_property(env, exports, fns[i].name, f);
    }
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
