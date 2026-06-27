/* src/ir_load.h
 *
 * IR loader and the in-memory shape it produces.
 *
 * Three concerns live here because they are populated by the same
 * loader pass and queried by the same callers:
 *
 *   1. type_registry_t — primitives at fixed IDs, named record/sum
 *      types, interned list types, and the derived global variant
 *      index.
 *
 *   2. tool_t / flow_t — the parsed declaration tables. Flow bodies
 *      (bindings and return expression) are stored as opaque cJSON
 *      pointers; the executor walks them.
 *
 *   3. flowd_load_ir_file / flowd_load_ir_buffer — the loader entry
 *      points the test harness uses. The public flowd_load_ir
 *      (declared in flowd.h) is a thin wrapper that builds an
 *      internal DiagStream and delegates here.
 *
 * The IR shape this module parses is the JSON IR that flowc emits;
 * the loader and the compiler must agree on it.
 */

#ifndef FLOWD_IR_LOAD_H
#define FLOWD_IR_LOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "value.h"   /* type_id_t and TYPE_ID_* macros */
#include "util.h"
#include "flowd.h"   /* flowd_runtime typedef */

#include "cjson/cJSON.h"


/* ====================================================================
 * Type registry
 *
 * Primitives occupy IDs 1..4 (see value.h). Named record and sum
 * types start at 100 and grow by one as the loader registers them.
 * Interned list types start at 1000 and grow by one per unique
 * element type seen. The three ID ranges are disjoint so a single
 * uint32_t comparison classifies any ID:
 *
 *   id == 0                  : TYPE_ID_NONE (unresolved/uninitialized)
 *   1 <= id <= 4             : primitive
 *   100 <= id < 1000         : named record or sum
 *   1000 <= id               : interned list
 *
 * The "<1000" upper bound is enforced, not merely assumed: the
 * range [100, 1000) holds at most 900 named record/sum types, and the
 * loader rejects an IR whose named-type count would push the next id
 * to 1000 (R150 "too many named types"). That keeps the named and
 * interned-list ranges disjoint so the single-comparison
 * classification above stays valid. 900 is far above anything a v1 IR
 * produces, so the limit is unreachable in practice.
 *
 * `type_registry_lookup_by_name` accepts a type-reference string in
 * the exact form: a primitive name, a named type's
 * identifier, or a bracketed list `[T]` (nested lists `[[T]]` work).
 * On unknown names it returns TYPE_ID_NONE without emitting a diag —
 * the caller decides whether the absence is an error.
 * ==================================================================== */

typedef enum {
    TYPE_KIND_PRIMITIVE,
    TYPE_KIND_LIST,
    TYPE_KIND_RECORD,
    TYPE_KIND_SUM
} type_kind_t;

typedef struct {
    const char *name;   /* arena-owned */
    type_id_t   type;
} type_field_t;

typedef struct {
    const char   *name;       /* arena-owned */
    type_field_t *fields;     /* arena-owned, n_fields long */
    size_t        n_fields;
} type_variant_t;

typedef struct {
    type_id_t       id;
    type_kind_t     kind;
    const char     *name;     /* arena-owned, always set: primitives carry
                               * their primitive name, lists their
                               * bracketed "[T]" form (used as the by_name
                               * key so list strings round-trip), records
                               * and sums their declared name. */

    /* TYPE_KIND_LIST: element type id. */
    type_id_t       elem;

    /* TYPE_KIND_RECORD: fields in declaration order. */
    type_field_t   *fields;
    size_t          n_fields;

    /* TYPE_KIND_SUM: variants in declaration order. */
    type_variant_t *variants;
    size_t          n_variants;
} type_t;

typedef struct type_registry type_registry_t;

/* Lifetime: registries are owned by a flowd_runtime via its arena.
 * Direct construction is for unit tests; production code obtains a
 * registry from flowd_types() on a successfully loaded runtime. */
type_registry_t *type_registry_create(Arena *arena);

/* Resolve a type-reference string to a type ID. Returns
 * TYPE_ID_NONE on unknown name. List types are interned on first
 * lookup; subsequent lookups for the same element type return the
 * cached ID. Primitive names resolve through the same by_name table
 * as everything else: they are pre-registered at create time so
 * resolution never needs a primitive special case. */
type_id_t type_registry_lookup_by_name(type_registry_t *r, const char *name);

/* Fetch a registered type by ID. Returns NULL for TYPE_ID_NONE or any
 * unallocated ID. Pointer remains valid for the registry's lifetime. */
const type_t *type_registry_get(const type_registry_t *r, type_id_t id);

/* Intern a list type with the given element type. Returns the
 * stable id of the list type. Subsequent calls with the same element
 * return the same id. */
type_id_t     type_registry_intern_list(type_registry_t *r, type_id_t elem);

/* Total count of registered types (primitives + named + interned
 * lists). Useful for canonical-dump traversal. */
size_t type_registry_count(const type_registry_t *r);

/* Iterate registered types by index. The order is the order of
 * registration: primitives first (IDs 1..4), then named types in
 * declaration order, then interned lists in first-seen order. */
const type_t *type_registry_at(const type_registry_t *r, size_t idx);


/* ====================================================================
 * Variant index
 *
 * Derived from the type registry. Answers "which sum types
 * contain a variant named X, and at what position within each?" The
 * structure is computed once after the second type-resolution pass
 * and is read-only thereafter.
 * ==================================================================== */

/* Iterate the variant index in lex-sorted order for canonical output.
 * Returns NULL after the last entry. `out_sum` and `out_idx` receive
 * the position information. */
const char *type_registry_variant_index_at(
    const type_registry_t *r,
    size_t                 idx,
    type_id_t             *out_sum,
    uint32_t              *out_variant_idx);

size_t type_registry_variant_index_count(const type_registry_t *r);


/* ====================================================================
 * Tool and flow tables
 *
 * Tool IR uses the JSON key "input" for the parameter array;
 * flow IR uses "params" with a per-parameter "implicit" flag.
 * The internal C struct uses `params` in both cases for
 * uniformity; the loader bridges the JSON key difference.
 *
 * RETRY_DEFAULT is the sentinel for "the tool declaration carried no
 * `retry` field" (see parse_retry); it is not a policy. The gateway
 * treats it as no retry — a single attempt (see retry_schedule).
 * ==================================================================== */

typedef enum {
    EFFECT_PURE,
    EFFECT_DETERMINISTIC,
    EFFECT_MODEL,
    EFFECT_MUTATION
} effect_level_t;

typedef enum {
    RETRY_DEFAULT,
    RETRY_FOREVER,
    RETRY_COUNT,
    RETRY_BACKOFF
} retry_kind_t;

typedef struct {
    retry_kind_t kind;
    uint32_t     count;            /* RETRY_COUNT */
    uint32_t     backoff_initial;  /* RETRY_BACKOFF, milliseconds */
    uint32_t     backoff_max;
    uint32_t     backoff_factor;
} retry_policy_t;

typedef struct {
    const char   *name;             /* arena-owned */
    type_id_t     type;
} tool_param_t;

typedef struct {
    const char     *name;           /* arena-owned */
    effect_level_t  level;
    const char     *model_id;       /* arena-owned; NULL unless level==EFFECT_MODEL */
    tool_param_t   *params;         /* parsed from JSON "input" key */
    size_t          n_params;
    type_id_t       output;
    retry_policy_t  retry;
} tool_t;

typedef struct {
    const char *name;               /* arena-owned */
    type_id_t   type;
    bool        implicit;           /* "it"-shaped flow input */
} flow_param_t;

typedef struct {
    const char    *name;            /* arena-owned */
    flow_param_t  *params;
    size_t         n_params;
    type_id_t      output;

    /* Stored as opaque cJSON pointers; the executor walks
     * them. Lifetime is tied to the runtime handle. */
    const cJSON   *bindings;        /* array of {name, expr} objects */
    const cJSON   *return_expr;     /* single expression object */
} flow_t;


/* ====================================================================
 * Loader entry points (internal — used by --load-ir and direct tests)
 *
 * The public flowd_load_ir(const char *ir_json) wraps these; it
 * constructs a DiagStream internally and routes failures into the
 * process-global error slot consumed by flowd_last_error_json. That
 * slot (g_last_load_error_json in flowd.c) is a single static pointer
 * shared across the whole process: it is not per-runtime, and
 * flowd_last_error_json ignores its runtime argument and reads/clears
 * the one global. The loader uses no locking, so concurrent loads —
 * even on distinct runtimes — race on that slot (free/strdup without
 * mutual exclusion); callers must serialize load and last-error calls
 * across threads.
 *
 * Loader passes:
 *
 *   1. Parse the IR JSON with cJSON.
 *   2. Validate top-level shape (ir_version, types, tools, flows).
 *      Emit R150 if ir_version != "1.0".
 *   3. First pass over types[]: register names with placeholder IDs.
 *   4. Second pass over types[]: resolve field/variant types. Forward
 *      references are admitted.
 *   5. Build the variant index.
 *   6. Walk tools[]: read JSON "input", resolve types, parse effect
 *      and retry.
 *   7. Walk flows[]: read JSON "params" with "implicit", resolve
 *      types, store bindings/return as opaque cJSON.
 *
 * Any failure releases the in-progress arena and returns NULL. The
 * caller's DiagStream contains the recorded cause.
 *
 * The `name` argument to flowd_load_ir_buffer is the synthetic path
 * embedded in diagnostic locations; pass "<stdin>" or "<buffer>" when
 * there is no real path.
 * ==================================================================== */

/* flowd_runtime typedef comes from flowd.h, included above. */

flowd_runtime *flowd_load_ir_file  (const char *path,
                                    DiagStream *diag);

flowd_runtime *flowd_load_ir_buffer(const char *src, size_t len,
                                    const char *name,
                                    DiagStream *diag);


/* ====================================================================
 * Runtime introspection (internal)
 *
 * The public flowd.h exposes a narrower surface; these are the calls
 * the test driver and the --load-ir summary use. They are stable
 * across phases.
 * ==================================================================== */

const type_registry_t *flowd_types       (const flowd_runtime *rt);
size_t                 flowd_tool_count  (const flowd_runtime *rt);
const tool_t          *flowd_tool_at     (const flowd_runtime *rt, size_t i);
const tool_t          *flowd_tool_by_name(const flowd_runtime *rt, const char *name);
size_t                 flowd_flow_count  (const flowd_runtime *rt);
const flow_t          *flowd_flow_at     (const flowd_runtime *rt, size_t i);
const flow_t          *flowd_flow_by_name(const flowd_runtime *rt, const char *name);


/* ====================================================================
 * Canonical-dump-lite
 *
 * Writes a stable text-format introspection summary of the loaded
 * runtime to `out`. Used by `flowd --load-ir <path>` and diffed in
 * fixture tests. The output is byte-deterministic across runs and
 * machines.
 *
 * Format (one entry per line, lex-sorted at every collection):
 *
 *     ir_version: 1.0
 *     types:
 *       [1] string (primitive)
 *       [2] int (primitive)
 *       ...
 *       [100] User (record)
 *         email: string
 *       [101] Tier (sum)
 *         #0 Free
 *         #1 Pro {months: int}
 *       [1000] [Tier] (list of Tier)
 *     tools:
 *       create_user
 *         effect: deterministic
 *         input: email:string
 *         output: User
 *     flows:
 *       signup
 *         params: it:SignupRequest (implicit)
 *         output: User
 *     variant_index:
 *       Free -> Tier#0
 *       Pro -> Tier#1
 *
 * Human-readable inspector counterpart to the JSON canonical-dump
 * (flowd_canonical_dump_json), which serves machine diffs.
 * ==================================================================== */

void flowd_canonical_dump_lite(const flowd_runtime *rt, FILE *out);


/* ====================================================================
 * Canonical-dump JSON (differential harness backbone)
 *
 * Writes the loaded runtime as pretty-printed JSON with two-space
 * indentation, byte-deterministic across runs and machines. Distinct
 * from the canonical *value* form in value.c: this is
 * the canonical *runtime-state* form, covering types, tools, flows,
 * and the variant index. Bindings and return expressions are
 * intentionally omitted — they are opaque to the loader and belong to
 * the executor's domain.
 *
 * Top-level shape (keys lex-sorted):
 *
 *   {
 *     "flows":         [ ... ],     // by name
 *     "ir_version":    "1.0",
 *     "tools":         [ ... ],     // by name
 *     "types":         [ ... ],     // by id
 *     "variant_index": [ ... ]      // by name, ties by sum then idx
 *   }
 *
 * Per-element shapes are documented inline in the emitter (see
 * ir_load.c). All object keys, all field-lists within records and
 * variant payloads, and all parameter arrays are emitted in lex
 * order. Variant order within a sum type is preserved as the IR's
 * declaration order so the variant_idx remains a valid reference.
 *
 * The form is what tests/diff/ goldens lock down. flowc emits IR in
 * insertion order; we re-sort everything on emit so the canonical form
 * stays stable across flowc refactoring that perturbs source order.
 * ==================================================================== */

void flowd_canonical_dump_json(const flowd_runtime *rt, FILE *out);

#endif /* FLOWD_IR_LOAD_H */
