/* src/adapters/anthropic.h
 *
 * Anthropic provider adapter.
 *
 * Implements `flowd_provider_adapter_t` against Anthropic's
 * Messages API (`POST https://api.anthropic.com/v1/messages`).
 * Supports any model identifier whose first segment is "claude-",
 * which covers every Anthropic model the API serves at the time
 * of this writing and admits future Claude-family releases without
 * an adapter change.
 *
 * The adapter is the first one v1 ships built-in. The interface is
 * the `flowd_provider_adapter_t` struct in flowd.h (also described in
 * libflowd(3)); this file is the reference implementation hosts copy
 * when adding a provider.
 *
 * Three-layer secrets pattern:
 *
 *   Layer 1 (convention). The API key never appears in flow IR
 *   or in the flow's input. It enters the runtime through
 *   anthropic_adapter_config_t.api_key, which is fed to the
 *   adapter via `user_ctx` at registration time.
 *
 *   Layer 2 (adapter). The adapter attaches the key to the
 *   outgoing `x-api-key` request header. It never logs the key,
 *   never echoes it in error messages, never includes it in the
 *   response shape passed back to the runtime.
 *
 *   Layer 3 (defense-in-depth). The host registers a redactor on
 *   the trace writer (`trace_writer_set_redactor`) for any
 *   pattern-matchable secret shapes — backstop for the case where
 *   layers 1 or 2 were violated.
 *
 * Build dependency: libcurl. Configured in `flowd/configure`.
 */

#ifndef FLOWD_ADAPTERS_ANTHROPIC_H
#define FLOWD_ADAPTERS_ANTHROPIC_H

#include <stdint.h>

#include "../flowd.h"

/* Configuration for the adapter. Populated by the host before
 * registration; lifetime must exceed the runtime's. */
typedef struct {
    /* API key. Mandatory. Never logged, never traced. */
    const char *api_key;

    /* Base URL. Defaults to "https://api.anthropic.com" when NULL.
     * Tests point this at a local mock server. */
    const char *base_url;

    /* Maximum tokens to request per call. The Messages API requires
     * max_tokens, so 0 means the adapter substitutes its own default
     * of 1024 (see build_request_body) rather than omitting the field. */
    uint32_t max_tokens;

    /* Per-call timeout in milliseconds. 0 = no timeout. */
    uint32_t timeout_ms;

    /* Pricing in USD per million tokens, used to fill cost_cents from
     * the provider-reported token counts. Host-supplied so the figures
     * track the caller's actual rates rather than a hardcoded table
     * that would drift. Both 0 (the default) means cost is left at 0. */
    double price_in_per_mtok;
    double price_out_per_mtok;
} anthropic_adapter_config_t;

/* Build the adapter struct. Returns the adapter by value; the
 * caller keeps it (and the `cfg` it points at) alive for the
 * runtime's lifetime. Pass `cfg` by pointer to identify which
 * configuration to use.
 *
 * Usage:
 *
 *   anthropic_adapter_config_t cfg = {
 *       .api_key   = getenv("ANTHROPIC_API_KEY"),
 *       .base_url  = NULL,
 *       .max_tokens = 1024,
 *       .timeout_ms = 30000,
 *   };
 *   flowd_provider_adapter_t adapter = anthropic_adapter(&cfg);
 *   flowd_register_provider(rt, &adapter);
 *
 * The returned struct's `user_ctx` points at `cfg`.
 */
flowd_provider_adapter_t
anthropic_adapter(const anthropic_adapter_config_t *cfg);

#endif /* FLOWD_ADAPTERS_ANTHROPIC_H */
