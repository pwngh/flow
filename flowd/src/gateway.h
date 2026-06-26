/* src/gateway.h
 *
 * Model gateway.
 *
 * The gateway is the routing layer between the executor and model
 * providers. A host can register one function per model tool
 * (`flowd_register_model`, still supported as a fallback path), or
 * register one adapter per provider with the gateway; each adapter
 * declares which model identifiers it serves, and the gateway
 * dispatches model calls by walking adapters in registration order.
 * When an adapter claims a model the gateway handles it; otherwise
 * the executor falls back to a `flowd_register_model` registration.
 *
 * Implemented here:
 *
 *   - adapter dispatch by model id, in registration order
 *   - in-memory LRU cache keyed by (model_id, request hash); hits
 *     skip the network round-trip but are still accounted and
 *     recorded like live calls
 *   - retry wrapper honoring the IR's retry_policy_t (forever / count /
 *     backoff); `noretry:`-prefixed adapter errors skip retry
 *   - per-call usage metrics (tokens, cost) via invoke_with_metrics
 *   - flow-level budget enforcement: the executor threads the runtime's
 *     budget (set via flowd_set_budget) into every gateway_invoke, which
 *     aborts with R161 when a token/cost/elapsed limit is ALREADY
 *     exceeded. Enforcement is a pre-call check only; there is no
 *     post-call abort, so the budget is a soft cap. A single call whose
 *     usage pushes a running total past its limit still completes and is
 *     fully accounted; R161 is raised on the NEXT invocation. Cache hits
 *     are checked and counted against the budget the same way.
 *
 * Notes / not yet here:
 *
 *   - cost_cents is reported only if an adapter computes it; the
 *     bundled Anthropic adapter computes it from host-supplied
 *     per-token rates, and leaves it 0 when those rates are unset
 *   - run-level cooperative cancellation exists (flowd_cancel, checked
 *     at binding boundaries); the gateway does NOT abort an already
 *     in-flight provider HTTP call mid-request
 *
 * Dispatch is synchronous. The adapter struct itself is
 * `flowd_provider_adapter_t` declared in flowd.h (locked public
 * API surface). The bundled Anthropic adapter (adapters/anthropic.c)
 * is libcurl-backed and makes real HTTPS calls.
 */

#ifndef FLOWD_GATEWAY_H
#define FLOWD_GATEWAY_H

#include <signal.h>    /* sig_atomic_t (cooperative cancel flag) */
#include <stdbool.h>
#include <stddef.h>

#include "flowd.h"
#include "ir_load.h"   /* retry_policy_t */
#include "util.h"


/* ====================================================================
 * Gateway lifecycle (internal — created during flowd_load_ir,
 * destroyed during flowd_destroy)
 * ==================================================================== */

typedef struct gateway gateway_t;

gateway_t *gateway_create(Arena *arena);
void       gateway_destroy(gateway_t *gw);


/* ====================================================================
 * Budget
 *
 * Cumulative per-run limits; R161 on excess. See the gateway_invoke
 * `budget` parameter for enforcement and accounting details. Usage
 * resets per run.
 * ==================================================================== */

typedef struct {
    uint64_t tokens_limit;        /* 0 = no limit */
    double   cost_cents_limit;    /* 0 = no limit */
    /* 0 = no limit. NOTE: this caps the SUM of per-gateway_invoke
     * durations (the wall time each model call / cache lookup takes),
     * not total run wall-clock. Non-model tool work, executor overhead,
     * and idle time between calls are excluded, so a host expecting a
     * run deadline will not get one from this field. */
    uint64_t elapsed_ms_limit;
    /* Running totals; populated by the gateway as invocations
     * complete, used for live limit enforcement. elapsed_ms_used
     * accumulates only the per-call durations described above. The
     * manifest's budget_summary is computed separately, by summing
     * per-node metrics at seal time (tokens_in, tokens_out, cost_cents
     * only), not read from here. */
    uint64_t tokens_used;
    double   cost_cents_used;
    uint64_t elapsed_ms_used;
} budget_t;


/* ====================================================================
 * Adapter registration
 * ==================================================================== */

/* Append an adapter to the gateway's table. The adapter pointer is
 * borrowed; the caller must keep it alive for the runtime's
 * lifetime. Returns 0 on success or an R-code on failure (R150 on
 * malformed adapter struct). */
int gateway_register_adapter(gateway_t *gw,
                             const flowd_provider_adapter_t *adapter);


/* ====================================================================
 * Invocation
 *
 * Find the adapter that supports `model_id`, hand it the request,
 * return the response. On failure, `*err_msg` is set to a heap-
 * allocated string the caller frees. Returns the JSON response
 * (heap-allocated; caller frees) on success or NULL on failure.
 *
 * Adapter probing walks the adapter table in registration order;
 * the first whose `supports_model` returns non-zero handles the
 * call. If no adapter claims the model, returns NULL with
 * `*err_msg` set to a "no adapter" message.
 * ==================================================================== */

typedef struct {
    const char            *provider_name;   /* borrowed from adapter */
    uint32_t               retry_attempts;  /* 0 = first-try success */
    /* Per-call metrics reported by the v2 adapter path.
     * Zero when the adapter is v1 or didn't report counts. */
    uint64_t               tokens_in;
    uint64_t               tokens_out;
    double                 cost_cents;
} gateway_result_meta_t;

/* Invoke a model:
 *
 *   retry   honors the IR's per-tool retry_policy_t. NULL = no
 *           retry (one-shot). On TRANSPORT_ERROR-equivalent
 *           failures the call retries; on APPLICATION_ERROR
 *           failures it doesn't. Adapters on either path (the v1
 *           `invoke` callback and the v2 `invoke_with_metrics`
 *           response struct) indicate non-retryable failures by
 *           setting `err_msg` to a string beginning with the
 *           literal prefix "noretry:"; the gateway applies that
 *           convention uniformly to both.
 *
 *   budget  cumulative consumption limits. NULL = unlimited.
 *           Pre-call check: if any limit is already exceeded,
 *           returns NULL with err_msg = "R161 budget exceeded". The
 *           executor recognizes that prefix and reports a budget
 *           overrun under its own diagnostic code rather than folding
 *           it into a generic tool failure.
 *           Post-call: elapsed_ms, tokens, and cost are all added to
 *           the running totals (tokens/cost from metrics-reporting
 *           adapters, 0 otherwise). A cache hit is accounted the same
 *           way (its elapsed_ms is the lookup cost, typically ~0).
 *
 *   cancel  optional pointer to the runtime's cooperative cancel flag
 *           (&rt->cancel_requested). When non-NULL and set, the retry
 *           loop stops between attempts and interrupts its backoff
 *           sleep, then returns NULL with no err_msg — the executor
 *           recognizes the flag and reports R160 rather than a tool
 *           failure. Pass NULL to disable (single-shot callers).
 *
 *   out_meta receives per-call metadata. NULL = caller doesn't
 *           care.
 */
char *gateway_invoke(gateway_t           *gw,
                     const char          *model_id,
                     const char          *request_json,
                     const retry_policy_t *retry,
                     budget_t            *budget,
                     const volatile sig_atomic_t *cancel,
                     char               **err_msg,
                     gateway_result_meta_t *out_meta);


#endif /* FLOWD_GATEWAY_H */
