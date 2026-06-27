/* src/runtime_internal.h
 *
 * Internal struct layout for flowd_runtime. Shared by ir_load.c
 * (which builds it), exec.c (which runs flows against it), and
 * flowd.c (which manages the lifecycle through the locked-contract
 * public API).
 *
 * Public callers see flowd_runtime as opaque through flowd.h. This
 * header is internal — never installed, never exposed by include/.
 *
 * The executor adds tool/model implementation registration to the
 * runtime: the IR knows tool *names* and *signatures*; the host
 * supplies their *implementations* via flowd_register_tool /
 * flowd_register_model.
 */

#ifndef FLOWD_RUNTIME_INTERNAL_H
#define FLOWD_RUNTIME_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <signal.h>

#include "flowd.h"
#include "gateway.h"
#include "ir_load.h"
#include "util.h"

#include "cjson/cJSON.h"


/* ====================================================================
 * Tool / model implementation slots
 *
 * One slot per registered impl. The runtime indexes by tool name on
 * each invocation (linear scan; tool counts are tiny in practice).
 * The host's declared effect level must match the IR's level for the
 * tool exactly, checked at registration (R151) rather than at call
 * time so a mismatch fails fast; a name absent from the IR is rejected
 * (R152).
 * ==================================================================== */

typedef struct {
    const char         *name;          /* arena-owned */
    flowd_effect_level  level;
    /* A slot is one or the other: exactly one of these is non-NULL,
     * and find_impl uses which one to discriminate tool vs. model
     * lookups for the same name. */
    flowd_tool_fn       fn;            /* NULL if this is a model slot */
    flowd_model_fn      model_fn;      /* NULL if this is a tool slot */
    const char         *impl_version;  /* arena-owned */
    void               *user_ctx;
} tool_impl_t;

struct flowd_runtime {
    Arena            *arena;
    cJSON            *ir_root;          /* heap-owned via cJSON malloc */
    const char       *source_path;      /* arena-owned */

    type_registry_t  *types;

    tool_t           *tools;
    size_t            n_tools;
    flow_t           *flows;
    size_t            n_flows;

    /* Host implementations. Capacity grows by 2× as
     * registrations come in; for tiny tool counts this is wasteful
     * but linear-scan lookup is fast enough that we don't need a
     * hash table. */
    tool_impl_t      *impls;
    size_t            n_impls;
    size_t            cap_impls;

    /* Model gateway. Owned by the runtime arena; lifetime
     * matches the runtime handle. Tries adapters in registration
     * order for every EFFECT_MODEL tool call before falling back to
     * the per-model impl table above. */
    gateway_t        *gateway;

    /* Host-installed redactor + ctx, propagated into
     * every trace_writer the runtime opens. NULL = no redaction.
     * The redactor and ctx are borrowed; the host owns lifetime. */
    flowd_redactor_fn redactor;
    void             *redactor_ctx;

    /* Flow-level budget. When has_budget, the executor passes &budget
     * to every gateway_invoke so token/cost/elapsed limits are enforced
     * (R161 on excess) and usage accumulates. Reset at the start of
     * each run so limits are per-run. */
    budget_t          budget;
    bool              has_budget;

    /* Cooperative cancellation. A host (or a tool callback, or a signal
     * handler) sets this via flowd_cancel; run_flow polls it once per
     * let-binding (before evaluating each binding's expression) and once
     * more before the return expression, then aborts the run with R160.
     * The model gateway's retry loop also polls it — a pointer to this
     * field is threaded into gateway_invoke — so a retrying model call
     * (RETRY_FOREVER in particular) is interrupted mid-loop rather than
     * spinning until it succeeds. Other work is not re-checked: a
     * one-shot non-model tool call, or a single binding whose expression
     * nests several calls, runs to the next binding boundary before the
     * flag is observed again. It is a sig_atomic_t so it can be set from a
     * signal handler; it is not protected by any lock, so a setter racing
     * with the executor only guarantees the request is observed at the
     * next poll point, not immediately. */
    volatile sig_atomic_t cancel_requested;
};

#endif /* FLOWD_RUNTIME_INTERNAL_H */
