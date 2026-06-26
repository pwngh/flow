/*
 * flowd.h — public C API for the Flow runtime.
 *
 * libflowd loads a Flow IR document, lets the caller register
 * tool and model implementations, executes flows against typed
 * inputs, and writes a complete trace of every step to disk. The
 * trace directory layout is traces/<flow_name>/<execution_id>/
 * with one manifest.json, one nodes/<node_id>.json per node, and
 * a content-addressed values/<sha256>.json for any payload above
 * the inline threshold.
 *
 * Reentrancy: non-reentrant per flowd_runtime handle — serialize
 * calls to a given runtime. The library is also NOT safe to use
 * concurrently across threads even on distinct runtimes: the
 * last-error slot read by flowd_last_error_json is a single
 * process-global, written (free()/strdup()) by every entry point
 * with no lock and no thread-local storage. Two threads in the
 * library at once race on that pointer. Callers that need
 * concurrency must serialize all library calls process-wide.
 *
 * This is the static public surface. Dynamic-load entry points —
 * compiling a .flow source instead of loading a pre-compiled IR
 * document — live in flowd_dynamic.h, added later without
 * rewriting any function declared here.
 */

#ifndef FLOWD_H
#define FLOWD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flowd_runtime flowd_runtime;

/*
 * Effect levels mirror the IR's declared effect on each tool.
 * They govern runtime behavior at each node: caching, retry,
 * parallelism, and replay strategy all derive from the level.
 */
typedef enum {
    FLOWD_EFFECT_PURE          = 0,
    FLOWD_EFFECT_DETERMINISTIC = 1,
    FLOWD_EFFECT_MODEL         = 2,
    FLOWD_EFFECT_MUTATION      = 3
} flowd_effect_level;

/*
 * Tool implementation callback.
 *
 *   args_json   UTF-8, null-terminated JSON object of the tool's
 *               declared input parameters.
 *   err_json    on error, set *err_json to a heap-allocated JSON
 *               error object. The runtime calls free() on it.
 *   user_ctx    opaque pointer passed at registration time.
 *
 * Returns the output as a heap-allocated, null-terminated JSON
 * string; the runtime calls free() on it after consuming. On
 * error, return NULL and set *err_json.
 */
typedef char *(*flowd_tool_fn)(const char *args_json,
                               char      **err_json,
                               void       *user_ctx);

/* Model implementation callback. Same ownership as flowd_tool_fn. */
typedef char *(*flowd_model_fn)(const char *args_json,
                                char      **err_json,
                                void       *user_ctx);

/*
 * Provider adapter for the model gateway. Hosts register one
 * adapter per provider (OpenAI, Anthropic, an in-process stub,
 * etc.); the gateway dispatches each model call to the first
 * adapter whose `supports_model` returns non-zero.
 *
 *   provider_name   returns a static, lower-case provider id used
 *                   in trace records (e.g. "openai", "anthropic").
 *                   The string lives for the adapter's lifetime.
 *   supports_model  returns non-zero if this adapter can serve the
 *                   given model identifier (the string inside
 *                   `effect model("...")` on the tool decl).
 *   invoke          the load-bearing operation. Receives the model
 *                   id, the request JSON (the tool's input as
 *                   canonical JSON), and the adapter's user_ctx.
 *                   Returns the response JSON on success
 *                   (heap-allocated; runtime frees), or NULL on
 *                   failure with *err_msg set (heap-allocated;
 *                   runtime frees).
 *   user_ctx        opaque pointer; passed unchanged to each call.
 *                   This is where credentials (API keys, OAuth
 *                   tokens) live — never in flow inputs, never
 *                   in the tool's declared params. This is the
 *                   three-layer secrets pattern (convention /
 *                   adapter discipline / redactor hook).
 *
 * Adapter responsibilities — non-conforming adapters that violate
 * any of these contaminate traces and break the secrets invariant:
 *
 *   - On failure, *err_msg MUST NOT include credential bytes.
 *     Sanitize provider error responses before quoting them.
 *   - The returned response JSON MUST NOT include credential
 *     bytes. The runtime writes the response into the trace
 *     verbatim.
 *   - The adapter MUST NOT log credentials to stderr, syslog, or
 *     any host-collected diagnostic output.
 *
 * The adapter struct lifetime must exceed the runtime's. The
 * runtime borrows the pointer; the host owns the storage.
 */
/*
 * Result struct for the v2 invocation path (`invoke_with_metrics`).
 * The adapter fills these fields; the runtime owns the heap-allocated
 * strings and frees them with free().
 *
 *   response_json   on success, the model's response as canonical
 *                   JSON. NULL on failure.
 *   err_msg         on failure, a heap-allocated message. NULL on
 *                   success. Same "noretry:" prefix convention as
 *                   the v1 `invoke` callback.
 *   tokens_in       prompt tokens reported by the provider, or 0 if
 *                   the provider did not report a count. Aggregated
 *                   into the budget's tokens_used and surfaced via
 *                   gateway_result_meta_t.
 *   tokens_out      completion tokens reported by the provider.
 *   cost_cents      dollar cost in cents (e.g. 2.5 = 2.5¢). Adapters
 *                   that cannot compute cost set this to 0.0.
 *
 * Adapters that don't have a metrics path should not declare
 * `invoke_with_metrics`; the gateway falls back to the v1 `invoke`
 * callback and tokens/cost stay zero.
 */
typedef struct {
    char    *response_json;
    char    *err_msg;
    uint64_t tokens_in;
    uint64_t tokens_out;
    double   cost_cents;
} flowd_adapter_response_t;

typedef struct {
    const char *(*provider_name)(void);
    int (*supports_model)(const char *model_id, void *user_ctx);
    char *(*invoke)(const char *model_id,
                    const char *request_json,
                    char      **err_msg,
                    void       *user_ctx);
    void *user_ctx;

    /* Optional v2 invocation that returns provider
     * metrics alongside the response. If non-NULL the gateway
     * prefers this entry point over `invoke`. Adapters MUST set
     * either `invoke` or `invoke_with_metrics` (or both — when
     * both are present, v2 wins). The result struct is owned by
     * the runtime; the adapter populates fields in place. */
    void (*invoke_with_metrics)(const char *model_id,
                                const char *request_json,
                                flowd_adapter_response_t *result,
                                void       *user_ctx);
} flowd_provider_adapter_t;

/*
 * Load an IR document. ir_json is UTF-8, null-terminated. Returns
 * NULL on any error; flowd_last_error_json(NULL) then returns a
 * diagnostic (caller frees with free()).
 */
flowd_runtime *flowd_load_ir(const char *ir_json);

/*
 * Register a tool implementation against the IR's declared tool
 * of the same name. Returns 0 on success or an R15x code:
 *
 *   R151   effect-level mismatch with the IR's declaration
 *   R152   no tool of this name in the IR
 *   R153   signature mismatch (reserved; never emitted — see below)
 *
 * signature is the tool's input/output type written in Flow's
 * surface syntax, e.g. "(string) -> CreditHistory". It is currently
 * INERT: the implementation accepts it for ABI stability but does
 * not cross-check it against the IR's tool decl, so a signature that
 * disagrees with the IR is silently accepted and R153 is never
 * returned. The runtime relies on the type checker's guarantees over
 * the compiled IR instead. impl_version is host-chosen and recorded
 * in every trace this runtime produces; the runtime accepts version
 * drift across executions but records it.
 */
int flowd_register_tool(flowd_runtime     *rt,
                        const char        *name,
                        flowd_effect_level level,
                        const char        *signature,
                        flowd_tool_fn      implementation,
                        const char        *impl_version,
                        void              *user_ctx);

/*
 * Register a model implementation. Models are always
 * FLOWD_EFFECT_MODEL; the level is implied. Returns 0 or an R15x
 * code as for flowd_register_tool.
 */
int flowd_register_model(flowd_runtime *rt,
                         const char    *name,
                         const char    *signature,
                         flowd_model_fn implementation,
                         const char    *impl_version,
                         void          *user_ctx);

/*
 * Register a provider adapter with the gateway. Adapters are tried
 * in registration order; the first whose `supports_model` returns
 * non-zero handles each model call. The adapter struct is borrowed
 * by the runtime — keep it alive until flowd_destroy.
 *
 * When an adapter is registered, model calls route through the
 * gateway by preference. flowd_register_model remains supported
 * as a fallback path for hosts that prefer per-model function
 * pointers; the gateway is consulted first.
 *
 * Returns 0 on success or 150 if the adapter struct is malformed
 * (missing required function pointers).
 */
int flowd_register_provider(flowd_runtime                  *rt,
                            const flowd_provider_adapter_t *adapter);

/*
 * Redactor: host-supplied callback that transforms any value's
 * canonical bytes before the trace writer persists them. Layer 3
 * of the three-layer secrets pattern.
 *
 *   bytes      input bytes (not NUL-terminated; use len)
 *   len        number of input bytes
 *   out_len    out-parameter: bytes written to the returned buffer
 *   user_ctx   pointer passed at registration time
 *
 * Returns a heap-allocated transformed buffer (the runtime frees it),
 * or NULL to leave the original bytes untouched. The hash chain in
 * the trace's content-addressed store runs on the redacted form, so
 * two traces whose pre-redaction values differed but whose redacted
 * forms match share content addresses.
 *
 * The hook fires on:
 *   - invocation inputs (the tool's args JSON)
 *   - invocation outputs (the value the tool returned)
 *   - the synthesized input/output node values
 *
 * Hosts implement pattern-matching for known credential shapes;
 * flowd ships no built-in patterns.
 */
typedef char *(*flowd_redactor_fn)(const char *bytes, size_t len,
                                   size_t     *out_len,
                                   void       *user_ctx);

/*
 * Install a redactor on the runtime. Replaces any prior redactor;
 * NULL clears. Every trace_writer the runtime subsequently opens
 * (for flowd_run, flowd_run_named, flowd_replay, flowd_resume) is
 * wired with this redactor automatically. Hosts that need finer
 * control per-execution open writers manually via trace.h.
 *
 * The redactor and user_ctx must outlive the runtime. Returns 0
 * on success.
 */
int flowd_set_redactor(flowd_runtime    *rt,
                       flowd_redactor_fn redactor,
                       void             *user_ctx);

/*
 * Set a cumulative budget for runs on this runtime. Each limit caps a
 * run's total across all model calls; a limit of 0 means "no limit" for
 * that dimension. Enforcement is a pre-call check against already-
 * accumulated usage using strict ">": the model call that first pushes
 * cumulative usage past a limit still completes (and is accounted and
 * cached); it is the NEXT model call whose pre-check fails with R161,
 * causing the run to return NULL (inspect flowd_last_error_json).
 * Consequently a run whose only model call by itself exceeds a limit
 * never trips the check and returns its output normally. Usage resets
 * at the start of each run, so the budget is per-run. Token/cost figures
 * come from metrics-reporting provider adapters; cached model calls
 * count too. Returns 0, or 150 on a NULL runtime.
 */
int flowd_set_budget(flowd_runtime *rt,
                     uint64_t       tokens_limit,
                     double         cost_cents_limit,
                     uint64_t       elapsed_ms_limit);

/*
 * Request cooperative cancellation of the in-progress run on this
 * runtime. The executor checks at each binding boundary and, if set,
 * aborts the run: flowd_run returns NULL and the trace is sealed with
 * status "cancelled" (R160). Intended to be called while a run is in
 * flight — from a signal handler, another thread, or a tool callback
 * (it sets a sig_atomic_t flag). The flag is cleared at the start of
 * every run, so it must be set during the run it should cancel.
 * Returns 0, or 150 on a NULL runtime.
 */
int flowd_cancel(flowd_runtime *rt);

/*
 * Run a flow.
 *
 *   input_json         flow input as JSON (UTF-8, null-terminated).
 *   trace_dir          root under which one
 *                      traces/<flow_name>/<execution_id>/ directory
 *                      is written for this execution.
 *   suspension_token   on suspension, set to a heap-allocated
 *                      token (caller frees). The token identifies
 *                      the suspended execution; passing it to
 *                      flowd_resume produces a new sibling trace
 *                      linked to this one via `resumed_from`. The
 *                      original trace's manifest stays
 *                      "suspended" forever; resumption never
 *                      mutates it. On completion or error,
 *                      *suspension_token is set to NULL.
 *
 * Returns the flow's output as a heap-allocated JSON string on
 * completion (caller frees), or NULL on suspension or error.
 * Inspect *suspension_token to distinguish suspension (non-NULL)
 * from error (NULL).
 */
char *flowd_run(flowd_runtime *rt,
                const char    *input_json,
                const char    *trace_dir,
                char         **suspension_token);

/*
 * Run a specific named flow from a multi-flow IR. Behaves like
 * flowd_run otherwise. Returns NULL with R155 in last_error_json
 * if `flow_name` isn't declared in the loaded IR.
 */
char *flowd_run_named(flowd_runtime *rt,
                      const char    *flow_name,
                      const char    *input_json,
                      const char    *trace_dir,
                      char         **suspension_token);

/*
 * Re-execute a previously recorded flow run, writing a new trace.
 *
 *   flow_name        the named flow in the loaded IR to re-execute.
 *                    Must match the recorded flow's name (replay
 *                    against a different flow is an error).
 *   original_trace_dir
 *                    path to the original trace directory (the
 *                    `traces/<flow>/<execution_id>/` produced by an
 *                    earlier flowd_run).
 *   new_trace_dir    root under which the replay's new trace is
 *                    written. Convention: distinct from
 *                    original_trace_dir's root so the original is
 *                    preserved.
 *   new_model_id     NULL = same-model replay (every model_call
 *                    restores its recorded output, no provider
 *                    invocations). Non-NULL = model-versioned
 *                    replay: every model_call re-invokes via the
 *                    gateway against this new model_id, with the
 *                    same inputs the original recorded.
 *
 * Returns the replay's output as a heap-allocated JSON string on
 * success (caller frees), or NULL on error. Inspect
 * flowd_last_error_json for the cause.
 *
 * Every node in the new trace carries a `replay_of` cross-reference
 * pointing at the corresponding node in the original: nodes restored
 * from the original record it with reason "restored_from_trace", and
 * under model-versioned replay each re-invoked model_call records it
 * with reason "re_invoked". (This per-node linkage is specific to
 * replay; see flowd_resume for how resume differs.)
 */
char *flowd_replay(flowd_runtime *rt,
                   const char    *flow_name,
                   const char    *original_trace_dir,
                   const char    *new_trace_dir,
                   const char    *new_model_id);

/*
 * Resume a suspended flow.
 *
 * Resumption is a separate execution: the runtime writes a new
 * trace directory linked to the original by the `resumed_from` field
 * on the new manifest and by per-node `replay_of` cross-references on
 * the nodes it reconstructs from the original — the synthesized input
 * node, the nodes restored up to (and including) the suspended
 * await point, and the synthesized output node. Nodes that run live
 * after the suspension point carry NO `replay_of` (unlike replay,
 * where every node is linked). The original suspended trace is
 * unchanged — its manifest stays
 * "suspended" forever: the trace store is append-only, so history
 * is extended (a new sibling trace), never edited in place.
 *
 *   token            from a prior flowd_run / flowd_resume's
 *                    suspension_token out-parameter.
 *   decision_json    JSON value injected at the suspension point.
 *                    Must conform to the suspended tool's declared
 *                    output type; type mismatch is R155.
 *                    For the built-in await_human_approval tool,
 *                    this is the approval payload: approver,
 *                    decision, rationale, timestamp, policy_ref.
 *   suspension_token if the resumed flow suspends again at a later
 *                    point, *suspension_token receives a new token
 *                    pointing at the new suspension. Otherwise
 *                    *suspension_token is set to NULL.
 *
 * Same return contract as flowd_run: the resumed execution's
 * output JSON on completion (caller frees), or NULL on further
 * suspension or error.
 */
char *flowd_resume(flowd_runtime *rt,
                   const char    *token,
                   const char    *decision_json,
                   char         **suspension_token);

/*
 * Return the most recent error as a JSON object (heap-allocated,
 * caller frees). Returns NULL when no error is pending.
 *
 * The error slot is a single process-global, not per-handle: the rt
 * argument is ignored (pass NULL or any runtime — the result is the
 * same), and there is no distinction between a load error and a
 * run/resume/replay error. Every entry point (flowd_load_ir,
 * flowd_run, flowd_run_named, flowd_replay, flowd_resume) overwrites
 * this one slot, so the value returned is whichever of those failed
 * most recently across the whole process. The call is read-and-clear:
 * it hands the caller the stored pointer and resets the slot to NULL,
 * so a second call without an intervening failure returns NULL. This
 * shared global is why concurrent use across threads is unsafe (see
 * the Reentrancy note at the top of this header).
 */
char *flowd_last_error_json(flowd_runtime *rt);

/* Destroy a runtime handle. Safe to call with NULL. */
void flowd_destroy(flowd_runtime *rt);

#ifdef __cplusplus
}
#endif

#endif /* FLOWD_H */
