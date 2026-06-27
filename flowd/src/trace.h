/* src/trace.h
 *
 * Trace writer + reader.
 *
 * Implements the on-disk trace format. One execution produces one
 * directory:
 *
 *   traces/<flow>/<execution_id>/
 *     manifest.json
 *     nodes/<node_id>.json
 *     values/<sha256>.json     (for values above inline threshold)
 *
 * (audit.log is NOT an execution output: the writer never creates it.
 * It is appended lazily by the reader on open/value-read, so an
 * execution that is never read produces no audit.log.)
 *
 * The writer is opened at flow start, accumulates per-node records in
 * memory as nodes complete, and writes them all out at flow end. The
 * seal step flushes every buffered node record, then writes
 * manifest.json atomically (via .tmp + rename), so readers detect a
 * complete trace by the presence of manifest.json.
 *
 * Scope:
 *   - Node kinds: "input", "tool_call", "model_call",
 *     "subflow_call", "suspension", "output". Lower-level expressions (binops,
 *     paths, constructs, pipelines, conditionals, matches) are
 *     not recorded as separate nodes — they're internal to the
 *     containing tool/flow node's evaluation. This keeps trace
 *     output focused on the boundary between flow logic and the
 *     external world, which is what audit and replay care about.
 *   - Single-invocation tool/model nodes for v1 (per-row
 *     invocations for pipeline-driven calls are a follow-up).
 *   - Audit log appends on every reader open (`trace_read` event).
 *   - The writer is not thread-safe; flowd_run is non-reentrant
 *     per the documented contract.
 */

#ifndef FLOWD_TRACE_H
#define FLOWD_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cjson/cJSON.h"
#include "flowd.h"   /* flowd_redactor_fn typedef */
#include "util.h"
#include "value.h"


/* ====================================================================
 * Writer
 * ==================================================================== */

typedef struct trace_writer trace_writer_t;

typedef enum {
    TRACE_STATUS_COMPLETE,
    TRACE_STATUS_FAILED,
    TRACE_STATUS_SUSPENDED,
    TRACE_STATUS_CANCELLED
} trace_status_t;

/* ====================================================================
 * Redaction hook (Layer 3 of the three-layer pattern)
 *
 * The host supplies a function that transforms canonical-value bytes
 * before they're written to disk. Returns a heap-allocated transformed
 * buffer (caller frees) or NULL to leave the bytes untouched. Fires on
 * every value the writer persists: invocation inputs, invocation
 * outputs, and externalized value-store entries. Use cases: stripping
 * leaked API keys, masking PII, replacing a value with a sentinel.
 *
 * Layers 1 and 2 are host/adapter conventions (secrets never reach tool
 * args — they live in adapter user_ctx; adapters never echo credentials).
 * This hook is the defense-in-depth layer for when those were violated
 * or a tool received a sensitive value as input.
 *
 * The typedef is declared in flowd.h (public surface), since the
 * runtime-level flowd_set_redactor takes the same callback shape.
 * ==================================================================== */

/* Open a writer. Creates traces/<flow>/<execution_id>/ and the
 * subdirectories (nodes/, values/). Returns NULL on failure.
 * Directory-create failures emit a diagnostic (R301) via diag;
 * allocation failures (OOM/strdup/slug) return NULL without one, so
 * a NULL return does not guarantee a diag was emitted.
 *
 *   trace_root        e.g. "traces" — the root directory.
 *   flow_name         the source-level flow name.
 *   ir_json           the IR JSON the runtime loaded (will be
 *                     hashed into manifest.ir_hash).
 *   inline_threshold  values whose canonical bytes ≤ this stay
 *                     inline in node records; bigger ones go to
 *                     values/<hash>.json. 0 → 64 KiB default.
 *   diag              for errors.
 *   exec_tag          optional marker appended to the execution id as
 *                     "_<tag>" (e.g. "resumed"). NULL or "" for a
 *                     plain run. The six-hex suffix is normally a
 *                     fresh CSPRNG value per call, so tagged runs are
 *                     unique. The exception is the FLOWD_EXECUTION_ID_SUFFIX
 *                     env override (used by replay tests for byte-identical
 *                     traces): when set it pins the suffix, so two runs
 *                     sharing the same date and tag produce an identical
 *                     execution_id and reuse the same directory. */
trace_writer_t *trace_writer_open(const char *trace_root,
                                  const char *flow_name,
                                  const char *ir_json,
                                  size_t      inline_threshold,
                                  DiagStream *diag,
                                  const char *exec_tag);

/* Install a redactor on the writer. Replaces any previously set
 * redactor. NULL clears. Returns 0 on success. The redactor and
 * its user_ctx must remain valid for the writer's lifetime. */
int trace_writer_set_redactor(trace_writer_t   *w,
                              flowd_redactor_fn redactor,
                              void             *user_ctx);

/* The full trace directory path (e.g. "traces/onboard/exec_..."). */
const char *trace_writer_dir(const trace_writer_t *w);

/* Node id of the most recently created node (e.g. the output node right
 * after trace_writer_set_output), or NULL if none. */
const char *trace_writer_last_node_id(const trace_writer_t *w);

/* Record the flow's input value. Creates and finalizes the in-memory
 * n0 node accumulator immediately, but does not write nodes/n0.json
 * to disk — like every node record, it is buffered until trace_writer_seal.
 * (An externalized value file may still be written immediately if the
 * input exceeds the inline threshold.) input_hash on the manifest is
 * derived from this value, hashed before any redactor rewrites it. */
int trace_writer_set_input(trace_writer_t *w, const value_t *input);

/* Begin a node record. Allocates a sequential node id and stamps the
 * start time. The returned id is the handle the caller passes back to
 * set_tool / set_model / invocation / end_node, so it stays valid for
 * the writer's lifetime (until trace_writer_close), not just until the
 * node ends. */
const char *trace_writer_begin_node(trace_writer_t *w,
                                    const char     *kind,
                                    const char     *effect_level);

/* For tool_call / model_call only: record the tool name + version.
 * Must be called between begin and end. */
int trace_writer_set_tool(trace_writer_t *w, const char *node_id,
                          const char *tool_name,
                          const char *tool_version);

/* For model_call only: record provider, model id, version. */
int trace_writer_set_model(trace_writer_t *w, const char *node_id,
                           const char *provider,
                           const char *model,
                           const char *version);

/* For model_call only: record provider-reported usage stats (token
 * counts and cost in cents). Values are 0 when the provider did not
 * report them. Accumulated into the manifest's budget_summary. */
int trace_writer_set_model_metrics(trace_writer_t *w, const char *node_id,
                                   uint64_t tokens_in,
                                   uint64_t tokens_out,
                                   double   cost_cents,
                                   uint32_t retry_attempts);

/* For subflow_call: record the callee flow name. */
int trace_writer_set_subflow(trace_writer_t *w, const char *node_id,
                             const char *callee_flow);

/* Record the `replay_of` cross-reference.
 * Set on every node emitted by a replay run so a reader can find
 * the corresponding node in the original trace.
 *
 *   trace_path   path to the original trace directory
 *   orig_node    the same node_id in the original (replay preserves
 *                node IDs by deterministic execution order)
 *   mode         "restored_from_trace", "re_invoked", or
 *                "substituted". */
int trace_writer_set_replay_of(trace_writer_t *w, const char *node_id,
                               const char *trace_path,
                               const char *orig_node,
                               const char *mode);

/* Add the single invocation of the node (the writer records one
 * invocation per node; per-row invocations come later). Only `output`
 * is a value_t: it is canonicalized, SHA-256'd, and either inlined or
 * externalized per the threshold. `inputs_json` is treated differently
 * — it is stored verbatim into the node record as canonical JSON,
 * never canonicalized again here and never wrapped in the inline/hash
 * envelope outputs use.
 *
 *   inputs_json   JSON-string of the inputs object the host saw
 *                 (e.g. "{\"x\":7}") — already canonical. Pass
 *                 NULL for the "input" and "output" node kinds. */
int trace_writer_invocation(trace_writer_t *w, const char *node_id,
                            const char    *inputs_json,
                            const value_t *output);

/* Stamp the node's end time (this is what fixes its elapsed_ms). A
 * non-NULL error_msg records the node as failed; NULL means success. */
int trace_writer_end_node(trace_writer_t *w, const char *node_id,
                          const char *error_msg);

/* Record the flow's output value (the final return). Writes the
 * synthetic "output" node. */
int trace_writer_set_output(trace_writer_t *w, const value_t *output);

/* Link this trace to the original it resumed from. Emits a
 * `resumed_from` field in the manifest. NULL clears it. */
int trace_writer_set_resumed_from(trace_writer_t *w, const char *original_dir);

/* Atomically write manifest.json and finalize the trace. */
int trace_writer_seal(trace_writer_t *w, trace_status_t status);

void trace_writer_close(trace_writer_t *w);


/* ====================================================================
 * Reader
 *
 * Loads an existing trace directory. The reader appends a
 * `trace_read` audit log entry on open, satisfying the access-
 * record requirement for the entry-event class.
 * ==================================================================== */

typedef struct trace_reader trace_reader_t;

trace_reader_t *trace_reader_open(const char *trace_dir,
                                  const char *caller_id,
                                  DiagStream *diag);

/* The parsed manifest. Pointer valid until trace_reader_close. */
const cJSON *trace_reader_manifest(const trace_reader_t *r);

/* Load a node record by ID. Returns a freshly-parsed cJSON tree;
 * caller frees with cJSON_Delete. */
cJSON *trace_reader_node(const trace_reader_t *r, const char *node_id);

/* Fetch a value from the content-addressed store by its full
 * `sha256:HEX` hash. Appends a `value_read` audit entry.
 * Returns a freshly-parsed cJSON tree; caller frees with
 * cJSON_Delete. Returns NULL if the value file is missing. */
cJSON *trace_reader_value(const trace_reader_t *r, const char *hash);

/* Extract the recorded output of a specific invocation as canonical
 * JSON. The on-disk encoding is either {"inline": <value>} or
 * {"hash": "sha256:..."}; this helper resolves the hash form by
 * fetching from the value store. Returns a heap-allocated NUL-
 * terminated string (caller frees) or NULL on error.
 *
 * Used by replay and resume to restore a node's recorded output
 * instead of re-running it. Every restored node reads through here:
 * pure (0), deterministic (1), and mutation (3) are always restored
 * (mutation must never repeat its side effect), and model (2) nodes
 * are restored too on a same-model replay — only a model-versioned
 * replay re-invokes them. See the replay branch in exec.c. */
char *trace_reader_invocation_output(const trace_reader_t *r,
                                     const char *node_id,
                                     size_t invocation_idx);

/* Same shape, for the recorded inputs of an invocation. Returns
 * the canonical JSON of the inputs object (e.g. `{"x":21}`) or
 * NULL if the node didn't record inputs (synthetic input/output
 * nodes don't). */
char *trace_reader_invocation_inputs(const trace_reader_t *r,
                                     const char *node_id,
                                     size_t invocation_idx);

/* Iterate node IDs in execution order (the manifest's node_count
 * gives the bound; ids are n0, n1, n2, ...). */
size_t trace_reader_node_count(const trace_reader_t *r);

void trace_reader_close(trace_reader_t *r);

#endif /* FLOWD_TRACE_H */
