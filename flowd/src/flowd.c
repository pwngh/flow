/*
 * flowd.c — implementation of the public C API declared in flowd.h.
 *
 * This file is the thin public surface over the runtime. Every entry
 * in the v1 contract is implemented here:
 *
 *   - flowd_load_ir            load IR from a NUL-terminated buffer
 *   - flowd_register_tool      bind a host tool implementation
 *   - flowd_register_model     bind a host model implementation
 *   - flowd_register_provider  install a gateway provider adapter
 *   - flowd_set_redactor       install a value redactor
 *   - flowd_set_budget         set token/cost/elapsed limits
 *   - flowd_cancel             request cooperative cancellation
 *   - flowd_run / _run_named   execute a flow, writing a trace
 *   - flowd_resume             resume a suspended run from its token
 *   - flowd_replay             re-execute model nodes against a new model
 *   - flowd_last_error_json    read the last load/run error
 *   - flowd_destroy            tear down a runtime
 *
 * The wrappers here own argument validation and the public R-code /
 * NULL contract; the actual work lives elsewhere — registration
 * populates the runtime's impl table, and run/run_named/resume/replay
 * dispatch into the executor (flowd_run_impl, flowd_resume_impl,
 * flowd_replay_impl in exec.{h,c}). The v1 signatures are the locked
 * contract; do not change them.
 *
 * Loading from a path or a length-bounded buffer with rich diagnostic
 * routing happens through flowd_load_ir_file / flowd_load_ir_buffer
 * in ir_load.{h,c}. The public flowd_load_ir wraps that path, builds
 * an internal record-only DiagStream, and on failure stashes a
 * JSON-formatted summary of the first error into a process-global
 * slot consumed by flowd_last_error_json().
 *
 * Thread-safety: that last-error slot (g_last_load_error_json) is a
 * single process-global, shared by flowd_load_ir AND every run path
 * (flowd_run / _run_named / _resume / _replay all write it on failure).
 * flowd_last_error_json ignores its runtime argument and reads/clears
 * that one global. There is no per-handle last-error storage, so the
 * runtime is NOT safe for concurrent API calls across threads even on
 * distinct runtime handles: two threads loading or running in parallel
 * race on the slot. Callers must serialize all flowd_* calls that can
 * set or read the last error.
 */

#include "flowd.h"
#include "exec.h"
#include "gateway.h"
#include "ir_load.h"
#include "runtime_internal.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void flowd_runtime_destroy(flowd_runtime *rt);  /* in ir_load.c */

static char *g_last_load_error_json = NULL;

/* Append `s` to buf[off..cap) as JSON string-body bytes (no surrounding
 * quotes), escaping per RFC 8259. Returns the new offset, or 0 on
 * overflow; a 0 input offset is propagated as 0 so a chain of these can
 * be written straight through and checked once at the end. (off is
 * always > 0 at the first real call, so 0 is an unambiguous failure.) */
static size_t
json_escape_into(char *buf, size_t off, size_t cap, const char *s)
{
    if (off == 0) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (off + 7u >= cap) return 0;   /* room for the longest escape */
        switch (*p) {
            case '"':  buf[off++] = '\\'; buf[off++] = '"';  break;
            case '\\': buf[off++] = '\\'; buf[off++] = '\\'; break;
            case '\b': buf[off++] = '\\'; buf[off++] = 'b';  break;
            case '\f': buf[off++] = '\\'; buf[off++] = 'f';  break;
            case '\n': buf[off++] = '\\'; buf[off++] = 'n';  break;
            case '\r': buf[off++] = '\\'; buf[off++] = 'r';  break;
            case '\t': buf[off++] = '\\'; buf[off++] = 't';  break;
            default:
                if (*p < 0x20) {
                    int m = snprintf(buf + off, cap - off, "\\u%04x", *p);
                    if (m < 0) return 0;
                    off += (size_t)m;
                } else {
                    buf[off++] = (char)*p;
                }
        }
    }
    return off;
}

/* Append an already-safe literal (object punctuation). Same 0-propagation
 * contract as json_escape_into. */
static size_t
json_append_literal(char *buf, size_t off, size_t cap, const char *lit)
{
    if (off == 0) return 0;
    size_t len = strlen(lit);
    if (off + len + 1u >= cap) return 0;
    memcpy(buf + off, lit, len);
    return off + len;
}

/* Format one recorded diagnostic from `s` as a single JSON object string:
 * the first record whose severity is DIAG_ERROR, or the first record of
 * any severity if none is an error. Returns NULL when no diagnostic is
 * recorded or on allocation failure. The returned buffer is heap-owned
 * and free()-able by the caller. */
static char *
diag_to_json_blob(const DiagStream *s)
{
    if (diag_count(s) == 0) return NULL;
    /* Prefer the first ERROR so the surfaced blob names the actual
     * failure even when warnings/notes were recorded ahead of it; only
     * if none is an error do we fall back to the first record. */
    const Diagnostic *pick = NULL;
    for (size_t i = 0; i < diag_count(s); i++) {
        const Diagnostic *d = diag_at(s, i);
        if (d->severity == DIAG_ERROR) { pick = d; break; }
    }
    if (!pick) pick = diag_at(s, 0);

    const char *id   = pick->id   ? pick->id   : "";
    const char *msg  = pick->message ? pick->message : "";
    const char *file = pick->loc.file ? pick->loc.file : "";
    const char *sev  = pick->severity == DIAG_ERROR   ? "error"   :
                       pick->severity == DIAG_WARNING ? "warning" : "note";

    /* Upper bound for the escape passes: 6× each variable char + frame. */
    size_t cap = (strlen(msg) + strlen(file) + strlen(id)) * 6u + 96u;
    char  *buf = malloc(cap);
    if (!buf) return NULL;

    /* Every variable field is escaped. id is a fixed R-code literal
     * today, but file comes from a caller-supplied SrcLoc and could
     * carry a quote/backslash that would otherwise emit malformed JSON. */
    int n = snprintf(buf, cap, "{\"severity\":\"%s\",\"id\":\"", sev);
    if (n < 0 || (size_t)n >= cap) { free(buf); return NULL; }
    size_t off = (size_t)n;

    off = json_escape_into  (buf, off, cap, id);
    off = json_append_literal(buf, off, cap, "\",\"file\":\"");
    off = json_escape_into  (buf, off, cap, file);
    off = json_append_literal(buf, off, cap, "\",\"message\":\"");
    off = json_escape_into  (buf, off, cap, msg);
    off = json_append_literal(buf, off, cap, "\"}");
    if (off == 0) { free(buf); return NULL; }
    buf[off] = '\0';
    return buf;
}

flowd_runtime *
flowd_load_ir(const char *ir_json)
{
    free(g_last_load_error_json);
    g_last_load_error_json = NULL;

    if (!ir_json) {
        g_last_load_error_json = strdup(
            "{\"severity\":\"error\",\"id\":\"R150\","
            "\"message\":\"flowd_load_ir: null input\"}");
        return NULL;
    }

    DiagStream *diag = diag_create();
    diag_record_only(diag);  /* public API: no stderr noise */
    diag_set_max_errors(diag, 1);

    flowd_runtime *rt = flowd_load_ir_buffer(
        ir_json, strlen(ir_json), "<buffer>", diag);

    if (!rt) {
        g_last_load_error_json = diag_to_json_blob(diag);
        if (!g_last_load_error_json) {
            g_last_load_error_json = strdup(
                "{\"severity\":\"error\",\"id\":\"R150\","
                "\"message\":\"load failed (no diagnostic recorded)\"}");
        }
    }
    diag_destroy(diag);
    return rt;
}

/* Push a new entry into rt->impls, growing the array as needed. */
static tool_impl_t *
impls_alloc_slot(flowd_runtime *rt)
{
    if (rt->n_impls == rt->cap_impls) {
        size_t ncap = rt->cap_impls ? rt->cap_impls * 2u : 8u;
        tool_impl_t *grown = arena_alloc_zero(rt->arena,
                                              ncap * sizeof *grown);
        if (rt->impls) {
            memcpy(grown, rt->impls, rt->n_impls * sizeof *grown);
        }
        rt->impls    = grown;
        rt->cap_impls = ncap;
    }
    return &rt->impls[rt->n_impls++];
}

int
flowd_register_tool(flowd_runtime     *rt,
                    const char        *name,
                    flowd_effect_level level,
                    const char        *signature,
                    flowd_tool_fn      implementation,
                    const char        *impl_version,
                    void              *user_ctx)
{
    (void)signature;  /* Limitation: the host-supplied signature is not
                       * cross-checked against the IR's tool decl. The
                       * runtime relies on the type checker's guarantees
                       * over the compiled IR, so a signature mismatch is
                       * not independently diagnosed here (no R153). The
                       * parameter is accepted to keep the v1 ABI stable. */
    if (rt == NULL || name == NULL || implementation == NULL) return 150;

    const tool_t *t = flowd_tool_by_name(rt, name);
    if (t == NULL) {
        return 152;       /* R152: tool name not in IR */
    }
    /* R151: effect-level mismatch. The host's level argument must
     * match the IR's declared level for the tool. */
    flowd_effect_level ir_level =
        t->level == EFFECT_PURE          ? FLOWD_EFFECT_PURE          :
        t->level == EFFECT_DETERMINISTIC ? FLOWD_EFFECT_DETERMINISTIC :
        t->level == EFFECT_MODEL         ? FLOWD_EFFECT_MODEL         :
                                           FLOWD_EFFECT_MUTATION;
    if (level != ir_level) return 151;
    /* Model-level tools must be registered through flowd_register_model,
     * which fills model_fn — the slot the executor dispatches model calls
     * from (exec.c selects model_fn for a model call). A plain tool fn
     * stored here would never be reached, so reject the misuse rather
     * than accept it into an unreachable slot. */
    if (ir_level == FLOWD_EFFECT_MODEL) return 151;

    tool_impl_t *slot = impls_alloc_slot(rt);
    slot->name         = arena_strdup(rt->arena, name);
    slot->level        = level;
    slot->fn           = implementation;
    slot->model_fn     = NULL;
    slot->impl_version = impl_version
        ? arena_strdup(rt->arena, impl_version) : NULL;
    slot->user_ctx     = user_ctx;
    return 0;
}

int
flowd_register_provider(flowd_runtime                  *rt,
                        const flowd_provider_adapter_t *adapter)
{
    if (rt == NULL || adapter == NULL) return 150;
    if (rt->gateway == NULL) return 150;
    return gateway_register_adapter(rt->gateway, adapter);
}

int
flowd_set_redactor(flowd_runtime    *rt,
                   flowd_redactor_fn redactor,
                   void             *user_ctx)
{
    if (rt == NULL) return 150;
    rt->redactor     = redactor;
    rt->redactor_ctx = user_ctx;
    return 0;
}

int
flowd_set_budget(flowd_runtime *rt,
                 uint64_t tokens_limit,
                 double   cost_cents_limit,
                 uint64_t elapsed_ms_limit)
{
    if (rt == NULL) return 150;
    rt->budget = (budget_t){
        .tokens_limit     = tokens_limit,
        .cost_cents_limit = cost_cents_limit,
        .elapsed_ms_limit = elapsed_ms_limit,
        .tokens_used      = 0,
        .cost_cents_used  = 0.0,
        .elapsed_ms_used  = 0,
    };
    rt->has_budget = true;
    return 0;
}

int
flowd_cancel(flowd_runtime *rt)
{
    if (rt == NULL) return 150;
    rt->cancel_requested = 1;
    return 0;
}

int
flowd_register_model(flowd_runtime *rt,
                     const char    *name,
                     const char    *signature,
                     flowd_model_fn implementation,
                     const char    *impl_version,
                     void          *user_ctx)
{
    (void)signature;  /* Limitation: as with flowd_register_tool, the
                       * signature is not validated against the IR; the
                       * type checker's guarantees over the IR stand in.
                       * Accepted to keep the v1 ABI stable. */
    if (rt == NULL || name == NULL || implementation == NULL) return 150;

    const tool_t *t = flowd_tool_by_name(rt, name);
    if (t == NULL) return 152;
    if (t->level != EFFECT_MODEL) return 151;

    tool_impl_t *slot = impls_alloc_slot(rt);
    slot->name         = arena_strdup(rt->arena, name);
    slot->level        = FLOWD_EFFECT_MODEL;
    slot->fn           = NULL;
    slot->model_fn     = implementation;
    slot->impl_version = impl_version
        ? arena_strdup(rt->arena, impl_version) : NULL;
    slot->user_ctx     = user_ctx;
    return 0;
}

char *
flowd_run(flowd_runtime *rt,
          const char    *input_json,
          const char    *trace_dir,
          char         **suspension_token)
{
    if (suspension_token) *suspension_token = NULL;

    free(g_last_load_error_json);
    g_last_load_error_json = NULL;

    DiagStream *diag = diag_create();
    diag_record_only(diag);
    diag_set_max_errors(diag, 0);

    char *out = NULL;
    char *susp_token = NULL;
    exec_status_t st = flowd_run_impl(rt, 0u, input_json, trace_dir,
                                      &out, &susp_token, diag);

    if (st == EXEC_SUSPENDED) {
        if (suspension_token) *suspension_token = susp_token;
        else                  free(susp_token);
    } else if (st != EXEC_OK) {
        free(susp_token);
        g_last_load_error_json = diag_to_json_blob(diag);
        if (g_last_load_error_json == NULL) {
            g_last_load_error_json = strdup(
                "{\"severity\":\"error\",\"id\":\"R155\","
                "\"message\":\"flow run failed (no diagnostic recorded)\"}");
        }
        free(out);
        out = NULL;
    } else {
        free(susp_token);
    }

    diag_destroy(diag);
    return out;
}

char *
flowd_replay(flowd_runtime *rt,
             const char    *flow_name,
             const char    *original_trace_dir,
             const char    *new_trace_dir,
             const char    *new_model_id)
{
    free(g_last_load_error_json);
    g_last_load_error_json = NULL;

    if (!rt || !flow_name || !original_trace_dir || !new_trace_dir) {
        g_last_load_error_json = strdup(
            "{\"severity\":\"error\",\"id\":\"R155\","
            "\"message\":\"flowd_replay: null required argument\"}");
        return NULL;
    }

    /* Resolve flow_name to its IR index. Linear scan, not a lookup
     * table: a program holds only a handful of flows, so the scan is
     * cheaper than maintaining an index. */
    size_t idx = (size_t)-1;
    size_t n = flowd_flow_count(rt);
    for (size_t i = 0; i < n; i++) {
        const flow_t *f = flowd_flow_at(rt, i);
        if (f && strcmp(f->name, flow_name) == 0) { idx = i; break; }
    }
    if (idx == (size_t)-1) {
        char buf[256];
        snprintf(buf, sizeof buf,
            "{\"severity\":\"error\",\"id\":\"R155\","
            "\"message\":\"replay: flow '%s' not found in IR\"}",
            flow_name);
        g_last_load_error_json = strdup(buf);
        return NULL;
    }

    DiagStream *diag = diag_create();
    diag_record_only(diag);
    diag_set_max_errors(diag, 0);

    char *out = NULL;
    exec_status_t st = flowd_replay_impl(rt, idx,
                                         original_trace_dir,
                                         new_trace_dir,
                                         new_model_id,
                                         &out, diag);
    if (st != EXEC_OK) {
        g_last_load_error_json = diag_to_json_blob(diag);
        if (!g_last_load_error_json) {
            g_last_load_error_json = strdup(
                "{\"severity\":\"error\",\"id\":\"R155\","
                "\"message\":\"replay failed\"}");
        }
        free(out); out = NULL;
    }
    diag_destroy(diag);
    return out;
}

char *
flowd_run_named(flowd_runtime *rt,
                const char    *flow_name,
                const char    *input_json,
                const char    *trace_dir,
                char         **suspension_token)
{
    if (suspension_token) *suspension_token = NULL;
    free(g_last_load_error_json);
    g_last_load_error_json = NULL;

    if (rt == NULL || flow_name == NULL) {
        g_last_load_error_json = strdup(
            "{\"severity\":\"error\",\"id\":\"R155\","
            "\"message\":\"flowd_run_named: null runtime or flow_name\"}");
        return NULL;
    }

    /* Resolve flow_name to its IR index. Linear scan, not a lookup
     * table: flow counts are tiny, so the scan beats maintaining one. */
    size_t idx = (size_t)-1;
    size_t n = flowd_flow_count(rt);
    for (size_t i = 0; i < n; i++) {
        const flow_t *f = flowd_flow_at(rt, i);
        if (f && strcmp(f->name, flow_name) == 0) { idx = i; break; }
    }
    if (idx == (size_t)-1) {
        char buf[256];
        snprintf(buf, sizeof buf,
            "{\"severity\":\"error\",\"id\":\"R155\","
            "\"message\":\"flow '%s' not found in IR\"}",
            flow_name ? flow_name : "(null)");
        g_last_load_error_json = strdup(buf);
        return NULL;
    }

    DiagStream *diag = diag_create();
    diag_record_only(diag);
    diag_set_max_errors(diag, 0);

    char *out = NULL;
    char *susp_token = NULL;
    exec_status_t st = flowd_run_impl(rt, idx, input_json, trace_dir,
                                      &out, &susp_token, diag);
    if (st == EXEC_SUSPENDED) {
        if (suspension_token) *suspension_token = susp_token;
        else                  free(susp_token);
    } else if (st != EXEC_OK) {
        free(susp_token);
        g_last_load_error_json = diag_to_json_blob(diag);
        if (!g_last_load_error_json) {
            g_last_load_error_json = strdup(
                "{\"severity\":\"error\",\"id\":\"R155\","
                "\"message\":\"named flow run failed\"}");
        }
        free(out); out = NULL;
    } else {
        free(susp_token);
    }
    diag_destroy(diag);
    return out;
}

char *
flowd_resume(flowd_runtime *rt,
             const char    *token,
             const char    *decision_json,
             char         **suspension_token)
{
    if (suspension_token) *suspension_token = NULL;
    free(g_last_load_error_json);
    g_last_load_error_json = NULL;

    if (!rt || !token || !decision_json) {
        g_last_load_error_json = strdup(
            "{\"severity\":\"error\",\"id\":\"R155\","
            "\"message\":\"flowd_resume: null required argument\"}");
        return NULL;
    }

    /* Resumption writes a new trace directory; the original suspended
     * trace is append-only and untouched. The new trace is a sibling
     * of the original under the same <traces>/<flow>/ parent: we pass
     * the <traces> root here and let the writer apply its normal
     * "<root>/<flow>/<exec_id>" layout, so there is no double nesting.
     * The writer tags the execution id "_resumed" and mints a fresh
     * six-hex suffix, so resuming the same token twice yields two
     * distinct directories rather than colliding. The exact ancestor
     * is also recorded in the manifest's resumed_from field.
     *
     * The token path is
     *   <traces>/<flow>/<exec_id>/suspensions/<id>.json
     * so stripping four trailing components yields <traces>. */
    size_t tlen = strlen(token);
    char *root = malloc(tlen + 1u);
    if (!root) {
        g_last_load_error_json = strdup(
            "{\"severity\":\"error\",\"id\":\"R155\","
            "\"message\":\"flowd_resume: out of memory\"}");
        return NULL;
    }
    memcpy(root, token, tlen + 1u);
    for (int i = 0; i < 4; i++) {
        char *slash = strrchr(root, '/');
        if (slash) *slash = '\0';
        else { root[0] = '\0'; break; }
    }
    /* If the token was shallower than four components (e.g. a relative
     * path), root in the CWD rather than at the filesystem root. */
    const char *root_arg = root[0] ? root : ".";

    DiagStream *diag = diag_create();
    diag_record_only(diag);
    diag_set_max_errors(diag, 0);

    char *out = NULL;
    char *susp_token = NULL;
    exec_status_t st = flowd_resume_impl(rt, token, decision_json,
                                         root_arg,
                                         &out, &susp_token, diag);
    free(root);

    if (st == EXEC_SUSPENDED) {
        if (suspension_token) *suspension_token = susp_token;
        else                  free(susp_token);
    } else if (st != EXEC_OK) {
        free(susp_token);
        g_last_load_error_json = diag_to_json_blob(diag);
        if (!g_last_load_error_json) {
            g_last_load_error_json = strdup(
                "{\"severity\":\"error\",\"id\":\"R155\","
                "\"message\":\"resume failed\"}");
        }
        free(out); out = NULL;
    } else {
        free(susp_token);
    }
    diag_destroy(diag);
    return out;
}

char *
flowd_last_error_json(flowd_runtime *rt)
{
    (void)rt;
    if (g_last_load_error_json == NULL) return NULL;
    char *out = g_last_load_error_json;
    g_last_load_error_json = NULL;
    return out;
}

void
flowd_destroy(flowd_runtime *rt)
{
    flowd_runtime_destroy(rt);
}
