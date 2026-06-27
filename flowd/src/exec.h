/* src/exec.h
 *
 * Flow executor.
 *
 * Walks a flow's `bindings` and `return` cJSON expression tree
 * against an environment of named values, producing a result value
 * the public API returns to the host as JSON.
 *
 * Surface:
 *   - env_t: lexically-scoped name → value_t* map.
 *   - JSON ↔ value_t conversion against the loaded type registry.
 *   - Expression evaluation in eval_expr, dispatching every
 *     supported expression kind (literals, paths, constructors,
 *     list literals, bin/unary ops, conditionals, try/else, calls,
 *     subflow calls, match, and pipelines); unknown kinds return an
 *     exec error.
 *   - flowd_run_impl: the engine behind the public flowd_run.
 *
 * Lifetime: flowd_run_impl creates a fresh arena per run and destroys
 * it on exit; every value the executor constructs lives in that arena.
 * Successive runs against the same runtime stay independent because
 * they share no arena — one run's arena_destroy can never invalidate
 * another's values.
 */

#ifndef FLOWD_EXEC_H
#define FLOWD_EXEC_H

#include <stdbool.h>
#include <stddef.h>

#include "flowd.h"
#include "ir_load.h"
#include "util.h"
#include "value.h"


/* ====================================================================
 * Execution result
 *
 * Outcomes from a flow run:
 *   OK    — `value` carries the produced value_t*.
 *   ERROR — diagnostic emitted to the stream; `value` is NULL.
 *
 * SUSPENDED is raised when a suspending tool (currently only the
 * built-in await_human_approval) is invoked; the run is paused and
 * resumed later via flowd_resume_impl. CANCELLED comes from the
 * cooperative cancel check run at each binding boundary (see
 * run_flow in exec.c); both carry `value == NULL`.
 * ==================================================================== */

typedef enum {
    EXEC_OK,
    EXEC_ERROR,
    EXEC_SUSPENDED,
    EXEC_CANCELLED
} exec_status_t;

typedef struct {
    exec_status_t  status;
    value_t       *value;        /* set when status == EXEC_OK */
} exec_result_t;


/* ====================================================================
 * Environment
 *
 * Linked-list frames. Each frame owns a flat array of (name, value*)
 * bindings; lookup walks current → parent → grandparent. New
 * scopes (match arms, pipeline row bindings, sub-flow invocations)
 * push a fresh frame.
 *
 * Frames are append-only — the runtime never mutates an existing
 * binding, because rebinding (`flow_a = ...; flow_a = ...`) is a
 * checker error caught at compile time. A frame is thus scanned
 * back-to-front so lookup still returns the most-recent binding even
 * if a name somehow appears twice.
 * ==================================================================== */

typedef struct env_frame env_frame_t;

typedef struct {
    Arena       *arena;          /* per-run arena */
    env_frame_t *top;
} env_t;

void           env_init  (env_t *e, Arena *a);
env_frame_t   *env_push  (env_t *e);
void           env_pop   (env_t *e, env_frame_t *prior_top);
void           env_bind  (env_t *e, const char *name, value_t *v);
const value_t *env_lookup(const env_t *e, const char *name);


/* ====================================================================
 * value_t ↔ JSON conversion
 *
 * JSON in: parse a cJSON tree against a declared type_id and produce
 * an arena-owned value_t. Validates shape (record fields present,
 * primitive types match, list element types) and emits diagnostics
 * on mismatch.
 *
 * JSON out: serialize a value_t to a NUL-terminated heap-allocated
 * string in canonical form. Caller frees with free().
 * ==================================================================== */

value_t *value_from_json(Arena                 *arena,
                         const type_registry_t *types,
                         type_id_t              expected,
                         const struct cJSON    *json,
                         DiagStream            *diag,
                         const char            *json_path);

char    *value_to_json_canonical(const value_t *v);


/* ====================================================================
 * Engine entry
 *
 * Runs the flow at flows[flow_idx] against the given input JSON.
 * The executor expects flow_idx==0 (the convention "first flow is the
 * entry point"); the parameter is here so it can later grow a named
 * variant without an API break.
 *
 * `input_json` must be a JSON object keyed by parameter name when
 * the flow has more than one parameter, or — for single-parameter
 * flows — either that object form OR the value directly. The single-
 * implicit-`it` case is the common one; the host may pass either
 * `{"it": <value>}` or `<value>` and both work.
 *
 * On success, *out_json points to a heap-allocated NUL-terminated
 * JSON string (caller frees). On failure or not-implemented, the
 * diag stream carries the cause and *out_json is set to NULL.
 * ==================================================================== */

exec_status_t flowd_run_impl(flowd_runtime *rt,
                             size_t         flow_idx,
                             const char    *input_json,
                             const char    *trace_dir,
                             char         **out_json,
                             /* On EXEC_SUSPENDED, *out_suspension_token
                              * is set to a heap-allocated token (the
                              * path to suspensions/<id>.json) that the
                              * caller can pass to flowd_resume — but ONLY
                              * when a trace directory was supplied. With
                              * no trace_dir (NULL/empty) there is nowhere
                              * to persist the suspension record, so the
                              * run still returns EXEC_SUSPENDED yet leaves
                              * *out_suspension_token NULL and is therefore
                              * not resumable. *out_suspension_token is
                              * initialized to NULL on entry, so it is
                              * never left indeterminate. Pass NULL if the
                              * caller doesn't care. */
                             char         **out_suspension_token,
                             DiagStream    *diag);


/* ====================================================================
 * Replay
 *
 * Re-execute a flow against a recorded trace. Each EFFECT_MODEL node
 * either:
 *   - restores the recorded output from `original_dir` (same-model
 *     replay, `new_model_id == NULL`), or
 *   - re-invokes against `new_model_id` with the deterministic
 *     re-computed input (model-versioned replay).
 *
 * Non-model nodes are NOT re-executed — they too are restored from
 * the recorded trace. Pure/deterministic nodes are restored for
 * fidelity; mutation (EFFECT_MUTATION) nodes MUST be restored because
 * re-invoking them would repeat external side effects, which replay
 * must never do. Only EFFECT_MODEL re-invokes, and only under a
 * model-versioned replay. Re-derived inputs that diverge from the
 * recorded ones abort with R157 (see exec.c restore_inputs_match).
 *
 * The new trace at `new_trace_dir` carries replay_of cross-references
 * on every node.
 * ==================================================================== */

exec_status_t flowd_replay_impl(flowd_runtime *rt,
                                size_t         flow_idx,
                                const char    *original_dir,
                                const char    *new_trace_dir,
                                const char    *new_model_id, /* NULL = same */
                                char         **out_json,
                                DiagStream    *diag);


/* ====================================================================
 * Resume after suspension.
 *
 * Resumption is a separate execution. The runtime loads the
 * suspension record at `suspension_token` (the path to
 * suspensions/<id>.json the suspended run wrote), re-executes the
 * flow against the recorded input, restores prior nodes from the
 * original trace via replay, and at the suspended node injects
 * `decision_json` as the output instead of suspending again.
 *
 * The new trace lands at `new_trace_dir`. Every node carries a
 * `replay_of` cross-reference to its counterpart in the original,
 * and the manifest carries `resumed_from` pointing at the original.
 * The original suspended trace is never touched — traces are
 * append-only, so resume always forks a new directory rather than
 * extending the suspended one.
 *
 * A resumed execution can itself suspend at a later suspension
 * point, in which case it writes its own suspensions/ file and the
 * chain extends: on EXEC_SUSPENDED, *out_suspension_token is set to
 * a heap token for that next suspension (NULL otherwise, and NULL if
 * no trace dir was given), exactly as flowd_run_impl does.
 * ==================================================================== */

exec_status_t flowd_resume_impl(flowd_runtime *rt,
                                const char    *suspension_token,
                                const char    *decision_json,
                                const char    *new_trace_dir,
                                char         **out_json,
                                char         **out_suspension_token,
                                DiagStream    *diag);

#endif /* FLOWD_EXEC_H */
