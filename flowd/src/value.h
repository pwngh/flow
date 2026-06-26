/* src/value.h
 *
 * Tagged-union value type for the Flow runtime.
 *
 * Every runtime value — flow input, tool input, tool output, binding
 * RHS, flow output — is a `value_t`. The kind discriminator names the
 * top-level shape; the nominal `type_id_t` carries the declared Flow
 * type, which is what governs equality, dispatch, and serialization
 * (a record value with type id A is distinct from a structurally
 * identical record with type id B even though their bytes match).
 *
 * Values are immutable once constructed. There is no `value_set_*`
 * API and there never will be. Mutation comes from constructing new
 * values; the arena absorbs the cost. String contents are arena-owned
 * and NUL-terminated.
 *
 * Lifetime: every value pointer is valid for as long as the arena it
 * was allocated against is alive. flowd_runtime owns its arena;
 * flowd_destroy reclaims everything.
 *
 * Canonical serialization (`value_canonical_serialize`) produces a
 * stable on-the-wire form. The function is byte-deterministic:
 * the same logical value always produces the same bytes, on the same
 * machine and across machines, regardless of system locale. Object
 * keys are sorted lexicographically by UTF-8 byte order, not
 * declaration order. The compiler and gateway both depend on this
 * stability for content addressing.
 */

#ifndef FLOWD_VALUE_H
#define FLOWD_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "util.h"


/* ====================================================================
 * Type IDs
 *
 * Primitives occupy fixed slots so hot-path code skips name lookups.
 * User-defined record and sum types are assigned IDs by the type
 * registry as the IR loads. Interned list types live in their own
 * range so list/non-list discrimination is trivial.
 *
 * The exact layout (primitives 1..4, named types 100+, lists 1000+)
 * is enforced by the type_registry implementation; this header just
 * names the primitive slots so values constructed without a registry
 * (e.g. test fixtures) can still produce the right type stamps.
 * ==================================================================== */

typedef uint32_t type_id_t;

#define TYPE_ID_NONE    ((type_id_t)0)
#define TYPE_ID_STRING  ((type_id_t)1)
#define TYPE_ID_INT     ((type_id_t)2)
#define TYPE_ID_FLOAT   ((type_id_t)3)
#define TYPE_ID_BOOL    ((type_id_t)4)


/* ====================================================================
 * Value kinds
 *
 * A variant's payload fields are kept in declaration order, the same
 * invariant records carry (see "Field cells" below). List elements
 * share a uniform element type — a Flow type-system guarantee the
 * runtime relies on but the bare items/n pair below cannot express.
 *
 * `VAL_NULL` exists for two reasons: it is the only value of the
 * unit-shaped null primitive (used by tools whose declared output is
 * pseudo-empty in the v1 surface), and it lets constructors return a
 * sentinel for "not yet computed" without exposing a NULL pointer to
 * callers.
 * ==================================================================== */

typedef enum {
    VAL_NULL,
    VAL_BOOL,
    VAL_INT,
    VAL_FLOAT,
    VAL_STRING,
    VAL_LIST,
    VAL_RECORD,
    VAL_VARIANT
} value_kind_t;


/* ====================================================================
 * Field cells
 *
 * Record fields and variant payload fields carry their name alongside
 * their value. Records are kept in *declaration order* in memory
 * — canonical serialization sorts on output, but the in-memory shape
 * preserves source order so executor traversal can rely on it.
 * ==================================================================== */

typedef struct value value_t;

typedef struct {
    const char *name;   /* arena-owned, NUL-terminated, never NULL */
    value_t    *value;  /* arena-owned */
} value_field_t;


/* ====================================================================
 * The value union itself
 *
 * Two discriminators, not one: `kind` selects the active union arm,
 * while `type` is the nominal Flow type id that drives equality (two
 * values of the same `kind` but different `type` are unequal; see the
 * file header). Reading the union without checking `kind` first is
 * undefined behaviour.
 * ==================================================================== */

struct value {
    value_kind_t kind;
    type_id_t    type;
    union {
        bool         b;
        int64_t      i;
        double       f;
        const char  *s;
        struct { value_t       **items;  size_t n; } list;
        struct { value_field_t  *fields; size_t n; } record;
        struct {
            const char    *variant_name;  /* arena-owned; canonical key */
            value_field_t *fields;
            size_t         n;
        } variant;
    } u;
};


/* ====================================================================
 * Constructors
 *
 * Each constructor allocates a `value_t` (and any backing arrays) from
 * the supplied arena and returns a pointer the caller may treat as
 * immutable. The `type` argument is the nominal type id stamped onto
 * the value; for primitives, use the matching TYPE_ID_* macro.
 *
 * `value_new_list_take` and `value_new_record_take` adopt
 * caller-supplied arrays of pointers/fields. The caller is responsible
 * for those arrays being arena-allocated against the same arena.
 *
 * Contract on string/name arguments: `value_new_string`'s `s` and
 * `value_new_variant_take`'s `variant_name` must be non-NULL,
 * NUL-terminated strings. The constructors copy them via arena_strdup,
 * which calls strlen() with no NULL guard, so passing NULL is a caller
 * contract violation that crashes inside libc rather than failing
 * cleanly — callers must guarantee a valid string at the boundary.
 * ==================================================================== */

value_t *value_new_null   (Arena *a, type_id_t type);
value_t *value_new_bool   (Arena *a, type_id_t type, bool        b);
value_t *value_new_int    (Arena *a, type_id_t type, int64_t     i);
value_t *value_new_float  (Arena *a, type_id_t type, double      f);
value_t *value_new_string (Arena *a, type_id_t type, const char *s);

value_t *value_new_list_take (Arena *a, type_id_t type,
                              value_t **items, size_t n);

value_t *value_new_record_take(Arena *a, type_id_t type,
                               value_field_t *fields, size_t n);

value_t *value_new_variant_take(Arena *a, type_id_t type,
                                const char *variant_name,
                                value_field_t *fields, size_t n);


/* ====================================================================
 * Operations
 *
 * value_equal — structural equality. Type ids must match. NaN ≠ NaN
 * per IEEE 754; this is intentional. The documented "unequal
 * values hash differently" property does not hold for NaN: all NaN
 * doubles serialize to the single token "nan", so two unequal NaN
 * values hash identically. An accepted exception — flows are
 * not expected to produce non-finite floats. Negative zero is handled:
 * -0.0 and 0.0 are equal and serialize identically ("0"), so they hash
 * the same.
 *
 * value_canonical_serialize — emits canonical bytes. Object keys
 * are sorted lexicographically by UTF-8 byte order. Numbers use the C
 * locale: the shared loader core (load_from_buffer_owned in
 * ir_load.c) installs setlocale(LC_NUMERIC, "C") before any value is
 * serialized, so the decimal separator is locale-independent. This
 * pin sits on the loader core that every load path funnels through —
 * flowd_load_ir_file (CLI) and flowd_load_ir_buffer (the public
 * flowd_load_ir wrapper) alike — rather than on flowd_load_ir itself,
 * which the CLI never calls. Non-finite floats are forced to the fixed
 * tokens nan / inf / -inf (not valid JSON, but deterministic across
 * platforms).
 *
 * value_hash — convenience wrapper that canonicalizes to an
 * open_memstream buffer and runs sha256_hex over the result. The
 * output is 64 lower-case hex chars plus a NUL.
 * ==================================================================== */

bool value_equal(const value_t *a, const value_t *b);

void value_canonical_serialize(const value_t *v, FILE *out);

/* Write `s` as a JSON string literal (including the surrounding double
 * quotes) to `out`, using the canonical RFC 8259 escape set: \", \\,
 * \b, \f, \n, \r, \t, and \uXXXX for other control characters below
 * 0x20. UTF-8 multi-byte sequences pass through verbatim. */
void value_emit_json_string(FILE *out, const char *s);

void value_hash(const value_t *v, char out_hex[65]);


#endif /* FLOWD_VALUE_H */
