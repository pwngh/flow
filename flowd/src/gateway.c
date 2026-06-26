/* src/gateway.c
 *
 * Model gateway.
 *
 * Beyond the base routing surface, this layer adds:
 *   - In-memory LRU cache keyed by (model_id, request_hash).
 *     Hits skip the adapter entirely; misses populate the cache on
 *     success.
 *   - Retry wrapper honoring the IR's retry_policy_t. Forever,
 *     count, and backoff forms all supported. Backoff delays come
 *     from sleeping the calling thread; tests pin FLOWD_BACKOFF_OFF
 *     to disable real sleeps and make retry-counted tests fast.
 *     Adapters indicate non-retryable failures with a `noretry:`
 *     prefix on err_msg.
 *   - Budget tracking (when a budget_t is passed). Pre-call check
 *     rejects with R161 if a limit is already exceeded; post-call,
 *     elapsed_ms, tokens, and cost are all added to the running totals
 *     (tokens/cost come from metrics-reporting adapters, 0 otherwise).
 *     The executor passes the runtime's budget when flowd_set_budget
 *     was called; cache hits are accounted too.
 *
 * Base routing invariants preserved: adapters borrowed (host owns
 * storage), arena-allocated tables, non-thread-safe (one gateway
 * per non-reentrant flowd_run handle).
 */

#include "gateway.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "value.h"   /* sha256_hex via util */


/* ====================================================================
 * Cache entry
 *
 * Doubly-linked LRU list + flat lookup table. The list is in
 * most-recently-used → least-recently-used order; a hit moves the
 * entry to the head. Eviction pops the tail.
 *
 * The lookup table is a flat array scanned linearly. For the current
 * cache sizes (≤ 1000 entries, default), linear scan is fast
 * enough; a hash table is a future optimization if profiles
 * call for it.
 * ==================================================================== */

typedef struct cache_entry {
    char hash_hex[65];           /* sha256 of (model_id ":" request_json) */
    char *response_json;         /* single-owner heap; list is LRU linkage */
    uint64_t tokens_in;          /* metrics of the cached call, so a hit */
    uint64_t tokens_out;         /*   still counts against a budget and is */
    double   cost_cents;         /*   recorded in the trace like a live call */
    struct cache_entry *prev;
    struct cache_entry *next;
} cache_entry_t;

typedef struct {
    cache_entry_t **slots;    /* arena-owned array of entry pointers */
    size_t          n;         /* live entries */
    size_t          cap;       /* maximum entries before eviction; 0 = off */
    cache_entry_t  *head;      /* MRU */
    cache_entry_t  *tail;      /* LRU */
} cache_t;


/* ====================================================================
 * Gateway struct
 * ==================================================================== */

#define GATEWAY_INITIAL_CAP    4u
#define CACHE_DEFAULT_ENTRIES  1000u

struct gateway {
    Arena                            *arena;
    const flowd_provider_adapter_t  **adapters;
    size_t                            n;
    size_t                            cap;

    cache_t                           cache;
};


/* ====================================================================
 * Cache implementation
 * ==================================================================== */

static void
cache_init(cache_t *c, Arena *arena, size_t cap)
{
    c->cap   = cap;
    c->n     = 0;
    c->head  = NULL;
    c->tail  = NULL;
    c->slots = cap > 0
        ? arena_alloc_zero(arena, cap * sizeof *c->slots)
        : NULL;
}

static void
cache_unlink(cache_t *c, cache_entry_t *e)
{
    if (e->prev) e->prev->next = e->next;
    else         c->head       = e->next;
    if (e->next) e->next->prev = e->prev;
    else         c->tail       = e->prev;
    e->prev = e->next = NULL;
}

static void
cache_link_front(cache_t *c, cache_entry_t *e)
{
    e->prev = NULL;
    e->next = c->head;
    if (c->head) c->head->prev = e;
    else         c->tail       = e;
    c->head = e;
}

/* A hit promotes the entry to MRU — so this read mutates LRU order,
 * and the returned pointer aliases a live list node, not a copy. */
static const cache_entry_t *
cache_lookup(cache_t *c, const char *hash_hex)
{
    if (c->cap == 0) return NULL;
    for (size_t i = 0; i < c->n; i++) {
        if (memcmp(c->slots[i]->hash_hex, hash_hex, 64) == 0) {
            cache_entry_t *e = c->slots[i];
            cache_unlink(c, e);
            cache_link_front(c, e);
            return e;
        }
    }
    return NULL;
}

static void
cache_put(cache_t *c, const char *hash_hex, const char *response,
          uint64_t tokens_in, uint64_t tokens_out, double cost_cents,
          Arena *arena)
{
    if (c->cap == 0) return;
    /* Re-cache of an existing key: refresh in place rather than add a
     * second entry, freeing the old single-owner response_json first.
     * Copy first so an OOM leaves the cache without an entry rather than
     * one holding a NULL response_json (which a later hit would strdup). */
    for (size_t i = 0; i < c->n; i++) {
        if (memcmp(c->slots[i]->hash_hex, hash_hex, 64) == 0) {
            char *copy = strdup(response);
            if (!copy) {
                /* Caching is best-effort: drop the stale entry entirely
                 * rather than keep one with a NULL response_json. */
                free(c->slots[i]->response_json);
                cache_unlink(c, c->slots[i]);
                c->slots[i] = c->slots[c->n - 1u];
                c->n--;
                return;
            }
            free(c->slots[i]->response_json);
            c->slots[i]->response_json = copy;
            c->slots[i]->tokens_in  = tokens_in;
            c->slots[i]->tokens_out = tokens_out;
            c->slots[i]->cost_cents = cost_cents;
            cache_unlink(c, c->slots[i]);
            cache_link_front(c, c->slots[i]);
            return;
        }
    }
    if (c->n == c->cap) {
        cache_entry_t *victim = c->tail;
        cache_unlink(c, victim);
        free(victim->response_json);
        /* Swap-remove (last slot fills the victim's): slots[] order is
         * irrelevant since LRU order lives in the linked list, so this
         * avoids shifting the tail. */
        for (size_t i = 0; i < c->n; i++) {
            if (c->slots[i] == victim) {
                c->slots[i] = c->slots[c->n - 1u];
                c->n--;
                break;
            }
        }
        /* The struct stays in the arena (freed only at arena teardown);
         * only its heap response_json was freed above. */
    }
    /* Copy the response before publishing the entry. On OOM, skip caching
     * entirely (best-effort): the arena slab for `e` is reclaimed only at
     * teardown, but it is never linked or counted, so no NULL-response
     * entry becomes visible to cache_lookup. */
    char *copy = strdup(response);
    if (!copy) return;
    cache_entry_t *e = arena_alloc_zero(arena, sizeof *e);
    memcpy(e->hash_hex, hash_hex, 64);
    e->hash_hex[64] = '\0';
    e->response_json = copy;
    e->tokens_in  = tokens_in;
    e->tokens_out = tokens_out;
    e->cost_cents = cost_cents;
    cache_link_front(c, e);
    c->slots[c->n++] = e;
}

static void
cache_clear(cache_t *c)
{
    /* Free every heap'd response_json. Arena-allocated entry structs
     * remain in the arena. */
    for (size_t i = 0; i < c->n; i++) {
        free(c->slots[i]->response_json);
        c->slots[i]->response_json = NULL;
    }
    c->n = 0;
    c->head = c->tail = NULL;
}


/* ====================================================================
 * Gateway lifecycle
 * ==================================================================== */

gateway_t *
gateway_create(Arena *arena)
{
    gateway_t *gw = arena_alloc_zero(arena, sizeof *gw);
    gw->arena    = arena;
    gw->adapters = arena_alloc_zero(arena,
                                    GATEWAY_INITIAL_CAP * sizeof *gw->adapters);
    gw->n        = 0;
    gw->cap      = GATEWAY_INITIAL_CAP;
    cache_init(&gw->cache, arena, CACHE_DEFAULT_ENTRIES);
    return gw;
}

void
gateway_destroy(gateway_t *gw)
{
    if (!gw) return;
    cache_clear(&gw->cache);
}


/* ====================================================================
 * Adapter registration
 * ==================================================================== */

int
gateway_register_adapter(gateway_t *gw,
                         const flowd_provider_adapter_t *adapter)
{
    if (!gw || !adapter) return 150;
    if (!adapter->provider_name || !adapter->supports_model) return 150;
    /* At least one of invoke / invoke_with_metrics must be set.
     * Adapters built before the v2 path only set `invoke`;
     * adapters that emit token/cost metrics set the v2 entry. */
    if (!adapter->invoke && !adapter->invoke_with_metrics) return 150;
    if (gw->n == gw->cap) {
        size_t ncap = gw->cap * 2u;
        const flowd_provider_adapter_t **grown = arena_alloc_zero(
            gw->arena, ncap * sizeof *grown);
        memcpy(grown, gw->adapters, gw->n * sizeof *grown);
        gw->adapters = grown;
        gw->cap      = ncap;
    }
    gw->adapters[gw->n++] = adapter;
    return 0;
}


/* ====================================================================
 * Retry helper
 *
 * Walks the policy, calling the adapter with backoff between
 * attempts. Returns the successful response or NULL after exhaustion.
 *
 * "Retryable" = err_msg does not begin with "noretry:". This is the
 * v1 convention adapters use to signal application-level failures
 * (APPLICATION_ERROR semantics).
 *
 * Sleeps are real wall-clock waits via nanosleep(). The env var
 * FLOWD_BACKOFF_OFF=1 disables sleeps so tests run fast.
 *
 * Cancellation: the loop consults the runtime's cooperative cancel flag
 * through the `cancel` pointer (&rt->cancel_requested, threaded in by the
 * executor). It is checked at the top of every attempt and in short
 * slices inside the backoff sleep, so a RETRY_FOREVER policy against a
 * persistently failing adapter aborts within ~20ms of flowd_cancel
 * rather than spinning indefinitely. On cancel the loop returns NULL
 * with no err_msg; the executor maps the flag to R160 (cancelled),
 * matching its binding-boundary checks. A NULL `cancel` disables this
 * (single-shot callers that pass no retry policy).
 * ==================================================================== */

static bool
is_retryable_err(const char *err_msg)
{
    return err_msg == NULL || strncmp(err_msg, "noretry:", 8) != 0;
}

static void
backoff_sleep(uint32_t ms, const volatile sig_atomic_t *cancel)
{
    /* Any non-empty value disables real sleeping (so FLOWD_BACKOFF_OFF=0
     * disables too — unset the variable to enable backoff). Tests set it
     * to keep retry-count cases fast. */
    const char *off = getenv("FLOWD_BACKOFF_OFF");
    if (off && *off) return;
    /* Sleep in short slices so a cooperative cancel is observed within one
     * slice rather than after the full (possibly multi-second) backoff. */
    const uint32_t slice = 20u;
    for (uint32_t slept = 0; slept < ms; slept += slice) {
        if (cancel && *cancel) return;
        uint32_t chunk = (ms - slept < slice) ? (ms - slept) : slice;
        struct timespec ts;
        ts.tv_sec  = chunk / 1000u;
        ts.tv_nsec = (long)(chunk % 1000u) * 1000000L;
        nanosleep(&ts, NULL);
    }
}

/* Returns 0 if this attempt index should proceed, non-zero if the
 * policy has exhausted. Also computes the pre-attempt sleep
 * duration. */
static int
retry_schedule(const retry_policy_t *p, uint32_t attempt_idx,
               uint32_t *out_sleep_ms)
{
    *out_sleep_ms = 0;
    if (!p || p->kind == RETRY_DEFAULT) {
        return attempt_idx > 0;       /* default: no retry */
    }
    switch (p->kind) {
        case RETRY_FOREVER:
            /* No bound. Use a fixed 100ms wait between attempts. */
            if (attempt_idx > 0) *out_sleep_ms = 100;
            return 0;
        case RETRY_COUNT:
            /* count is the number of retries, so attempt 0 (the initial
             * try) plus count retries are allowed; index > count exhausts. */
            if (attempt_idx > p->count) return 1;
            if (attempt_idx > 0) *out_sleep_ms = 100;
            return 0;
        case RETRY_BACKOFF: {
            if (attempt_idx == 0) return 0;
            uint64_t delay = p->backoff_initial;
            for (uint32_t k = 1; k < attempt_idx; k++) {
                delay *= p->backoff_factor;
                if (delay > p->backoff_max) {
                    delay = p->backoff_max;
                    break;
                }
            }
            /* Loop runs attempt_idx-1 times, so the first retry
             * (attempt_idx==1) sleeps backoff_initial untouched -- hence this
             * re-clamp, the only thing that bounds the case where
             * backoff_initial alone already exceeds backoff_max. */
            if (delay > p->backoff_max) delay = p->backoff_max;
            *out_sleep_ms = (uint32_t)delay;
            /* The backoff policy bounds only delay, not attempt count, so
             * hard-cap at 10 to keep a failing call from retrying forever. */
            return attempt_idx > 10;
        }
        case RETRY_DEFAULT: break;
    }
    return 1;
}


/* ====================================================================
 * Invocation
 * ==================================================================== */

static uint64_t
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static bool
budget_exceeded(const budget_t *b, const char **why)
{
    if (!b) return false;
    if (b->tokens_limit > 0 && b->tokens_used > b->tokens_limit) {
        if (why) *why = "tokens";
        return true;
    }
    if (b->cost_cents_limit > 0 && b->cost_cents_used > b->cost_cents_limit) {
        if (why) *why = "cost_cents";
        return true;
    }
    if (b->elapsed_ms_limit > 0 && b->elapsed_ms_used > b->elapsed_ms_limit) {
        if (why) *why = "elapsed_ms";
        return true;
    }
    return false;
}

char *
gateway_invoke(gateway_t *gw, const char *model_id,
               const char *request_json,
               const retry_policy_t *retry,
               budget_t *budget,
               const volatile sig_atomic_t *cancel,
               char **err_msg,
               gateway_result_meta_t *out_meta)
{
    if (err_msg) *err_msg = NULL;
    if (out_meta) {
        out_meta->provider_name   = NULL;
        out_meta->retry_attempts  = 0;
        out_meta->tokens_in       = 0;
        out_meta->tokens_out      = 0;
        out_meta->cost_cents      = 0.0;
    }
    if (!gw || !model_id) {
        if (err_msg) *err_msg = strdup("gateway_invoke: null arguments");
        return NULL;
    }

    /* Budget pre-check. The "R161" prefix is a contract with the
     * executor: it recognizes a budget overrun and reports it under
     * its own diagnostic code rather than folding it into a generic
     * tool-failure (R101). */
    const char *why = NULL;
    if (budget_exceeded(budget, &why)) {
        char buf[128];
        snprintf(buf, sizeof buf,
                 "R161 budget exceeded (%s)", why ? why : "?");
        if (err_msg) *err_msg = strdup(buf);
        return NULL;
    }

    /* First adapter (in registration order) that claims the model wins,
     * so registration order resolves overlapping supports_model claims. */
    const flowd_provider_adapter_t *adapter = NULL;
    for (size_t i = 0; i < gw->n; i++) {
        if (gw->adapters[i]->supports_model(model_id,
                                            gw->adapters[i]->user_ctx)) {
            adapter = gw->adapters[i];
            break;
        }
    }
    if (!adapter) {
        size_t mlen = strlen(model_id);
        char *msg = malloc(mlen + 64u);
        if (msg) snprintf(msg, mlen + 64u,
                          "no provider adapter supports model '%s'",
                          model_id);
        if (err_msg) *err_msg = msg;
        return NULL;
    }
    if (out_meta) out_meta->provider_name = adapter->provider_name();

    /* Cache key: sha256(model_id || ":" || request_json).
     * Unkeyed concatenation: model_id and request_json are joined by a single
     * ':' with no length prefix, so this stays collision-free only because
     * model_ids never contain ':'. Don't relax that without length-prefixing,
     * or two distinct (model,request) pairs could share a cache slot. */
    const char *req = request_json ? request_json : "{}";
    size_t mlen = strlen(model_id);
    size_t rlen = strlen(req);
    char  *keybuf = malloc(mlen + 1u + rlen);
    if (!keybuf) { if (err_msg) *err_msg = strdup("OOM"); return NULL; }
    memcpy(keybuf, model_id, mlen);
    keybuf[mlen] = ':';
    memcpy(keybuf + mlen + 1u, req, rlen);
    char hash_hex[65];
    sha256_hex(keybuf, mlen + 1u + rlen, hash_hex);
    free(keybuf);

    /* Timer covers the whole call, including the cache lookup, so a
     * hit and a miss are accounted on the same basis. */
    uint64_t start = now_ms();

    /* Cache hit? A hit skips the adapter but still counts against the
     * budget and reports the cached call's metrics — elapsed_ms (the
     * lookup cost, typically ~0), tokens, and cost — so a cached model
     * call is accounted and traced exactly like a fresh one. */
    if (gw->cache.cap > 0) {
        const cache_entry_t *hit = cache_lookup(&gw->cache, hash_hex);
        if (hit) {
            /* Copy the cached response before touching out_meta/budget so an
             * OOM here is surfaced as an explicit error rather than a silent
             * NULL return (which the executor would misreport as a generic
             * tool failure). cache_put never stores a NULL response_json, so
             * the source is always a valid string. */
            char *copy = strdup(hit->response_json);
            if (!copy) {
                if (err_msg) *err_msg = strdup("OOM");
                return NULL;
            }
            uint64_t elapsed = now_ms() - start;
            if (out_meta) {
                out_meta->tokens_in    = hit->tokens_in;
                out_meta->tokens_out   = hit->tokens_out;
                out_meta->cost_cents   = hit->cost_cents;
            }
            if (budget) {
                budget->elapsed_ms_used += elapsed;
                budget->tokens_used     += hit->tokens_in + hit->tokens_out;
                budget->cost_cents_used += hit->cost_cents;
            }
            return copy;
        }
    }

    char *result = NULL;
    uint32_t attempt = 0;
    char *attempt_err = NULL;
    uint64_t tokens_in = 0, tokens_out = 0;
    double   cost_cents = 0.0;
    for (;;) {
        /* Cooperative cancellation: bail before starting another attempt.
         * Leave *err_msg NULL — this is not a gateway failure; the executor
         * sees rt->cancel_requested and reports R160 (cancelled). result
         * stays NULL. */
        if (cancel && *cancel) {
            free(attempt_err);
            attempt_err = NULL;
            break;
        }
        uint32_t sleep_ms = 0;
        int exhausted = retry_schedule(retry, attempt, &sleep_ms);
        if (exhausted) {
            char buf[256];
            snprintf(buf, sizeof buf,
                     "R130 tool call failed after %u attempt(s): %s",
                     attempt,
                     attempt_err ? attempt_err : "(no message)");
            if (err_msg) *err_msg = strdup(buf);
            free(attempt_err);
            break;
        }
        if (sleep_ms > 0) backoff_sleep(sleep_ms, cancel);

        free(attempt_err);
        attempt_err = NULL;
        char *resp = NULL;
        if (adapter->invoke_with_metrics) {
            /* v2 path: the adapter returns token/cost metrics in the
             * response struct. r.err_msg is adapter-allocated and adopted
             * into attempt_err (freed like the v1 out-param). */
            flowd_adapter_response_t r = {0};
            adapter->invoke_with_metrics(model_id, req, &r,
                                         adapter->user_ctx);
            resp        = r.response_json;
            attempt_err = r.err_msg;
            /* Only trust the metrics on success; on failure the counts
             * are unspecified and must not pollute the budget. */
            if (resp != NULL) {
                tokens_in  = r.tokens_in;
                tokens_out = r.tokens_out;
                cost_cents = r.cost_cents;
            }
        } else {
            resp = adapter->invoke(model_id, req, &attempt_err,
                                   adapter->user_ctx);
        }
        if (resp != NULL) {
            result = resp;
            /* A v2 adapter may report a non-fatal err_msg alongside a
             * successful response; free it so success never leaks it. */
            free(attempt_err);
            attempt_err = NULL;
            break;
        }
        if (!is_retryable_err(attempt_err)) {
            /* Non-retryable (noretry:) application failure: surface R101
             * now, distinct from the R130 emitted only on retry exhaustion. */
            char buf[512];
            snprintf(buf, sizeof buf, "R101 %s",
                     attempt_err ? attempt_err : "tool failed");
            if (err_msg) *err_msg = strdup(buf);
            free(attempt_err);
            break;
        }
        attempt++;
    }

    uint64_t elapsed = now_ms() - start;
    /* out_meta is meaningful only on a non-NULL return; a cancelled call
     * (R160) returns NULL but may leave a stale, nonzero retry_attempts here. */
    if (out_meta) {
        out_meta->retry_attempts = attempt;
        out_meta->tokens_in      = tokens_in;
        out_meta->tokens_out     = tokens_out;
        out_meta->cost_cents     = cost_cents;
    }
    if (budget) {
        budget->elapsed_ms_used += elapsed;
        budget->tokens_used     += tokens_in + tokens_out;
        budget->cost_cents_used += cost_cents;
    }

    if (result && gw->cache.cap > 0) {
        cache_put(&gw->cache, hash_hex, result,
                  tokens_in, tokens_out, cost_cents, gw->arena);
    }
    return result;
}
