/* src/value.c
 *
 * Implementation of the tagged-union value type.
 *
 * Layout matches the header: constructors first, then structural
 * equality, then canonical serialization (the bulk of the file), then
 * the SHA-256 wrapper.
 *
 * Canonical serialization rules:
 *   - Object keys sorted lexicographically by UTF-8 byte order.
 *   - No inter-token whitespace.
 *   - Integers without decimal or exponent.
 *   - Floats via %.17g under the C locale: the always-round-trip
 *     over-approximation. A shortest-round-trip formatter is deferred;
 *     byte-determinism holds either way.
 *   - Strings per RFC 8259 with the minimum escape set: \", \\,
 *     \b, \f, \n, \r, \t, and \uXXXX for control characters below
 *     U+0020. Non-ASCII bytes pass through verbatim (UTF-8).
 *   - Arrays in source order.
 *   - Booleans as true/false, null as null.
 *   - Variants encoded as {"fields":{...},"variant":"<name>"} — two
 *     keys, already in lex order. Variants are keyed by name only;
 *     no numeric tag is stored or serialized.
 *
 * This file is compiled only into flowd (libflowd.a); the flowc
 * compiler binary never links it. The runtime pins LC_NUMERIC to "C"
 * in load_from_buffer_owned (ir_load.c), before any value is
 * serialized, so %.17g uses '.' rather than a locale-specific decimal
 * separator. That setlocale call is the in-process guarantor for this
 * serializer's determinism. (The compiler's own setlocale in
 * flowc/src/main.c pins LC_NUMERIC for its IR emitter's "%g" and has
 * no bearing on this file.) The test harness additionally exports
 * LC_ALL=C.
 */

#include "value.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>


/* ====================================================================
 * Constructors
 * ==================================================================== */

static value_t *
new_value(Arena *a, value_kind_t kind, type_id_t type)
{
    value_t *v = arena_alloc_zero(a, sizeof *v);
    v->kind = kind;
    v->type = type;
    return v;
}

value_t *value_new_null(Arena *a, type_id_t type)
{
    return new_value(a, VAL_NULL, type);
}

value_t *value_new_bool(Arena *a, type_id_t type, bool b)
{
    value_t *v = new_value(a, VAL_BOOL, type);
    v->u.b = b;
    return v;
}

value_t *value_new_int(Arena *a, type_id_t type, int64_t i)
{
    value_t *v = new_value(a, VAL_INT, type);
    v->u.i = i;
    return v;
}

value_t *value_new_float(Arena *a, type_id_t type, double f)
{
    value_t *v = new_value(a, VAL_FLOAT, type);
    v->u.f = f;
    return v;
}

value_t *value_new_string(Arena *a, type_id_t type, const char *s)
{
    /* Contract (value.h): s is non-NULL. Diagnose a violation here rather
     * than let arena_strdup -> strlen(NULL) fault deep in libc. */
    if (s == NULL) {
        FLOWD_ICE("value_new_string: s must be non-NULL");
    }
    value_t *v = new_value(a, VAL_STRING, type);
    v->u.s = arena_strdup(a, s);
    return v;
}

value_t *value_new_list_take(Arena *a, type_id_t type,
                             value_t **items, size_t n)
{
    value_t *v = new_value(a, VAL_LIST, type);
    v->u.list.items = items;
    v->u.list.n     = n;
    return v;
}

value_t *value_new_record_take(Arena *a, type_id_t type,
                               value_field_t *fields, size_t n)
{
    value_t *v = new_value(a, VAL_RECORD, type);
    v->u.record.fields = fields;
    v->u.record.n      = n;
    return v;
}

value_t *value_new_variant_take(Arena *a, type_id_t type,
                                const char *variant_name,
                                value_field_t *fields, size_t n)
{
    /* Contract (value.h): variant_name is non-NULL. Diagnose a violation
     * here rather than let arena_strdup -> strlen(NULL) fault in libc. */
    if (variant_name == NULL) {
        FLOWD_ICE("value_new_variant_take: variant_name must be non-NULL");
    }
    value_t *v = new_value(a, VAL_VARIANT, type);
    v->u.variant.variant_name = arena_strdup(a, variant_name);
    v->u.variant.fields       = fields;
    v->u.variant.n            = n;
    return v;
}


/* ====================================================================
 * Structural equality
 *
 * Type ids must match exactly. For records and variants we compare
 * fields in their stored declaration order; the canonical serializer
 * does the sorting for hash-key purposes, but the equality predicate
 * itself does not need to (two records with the same fields in the
 * same nominal type necessarily have the same declaration order
 * because the type's declaration is the source of truth).
 *
 * NaN ≠ NaN per IEEE 754. The C99 `==` operator implements this.
 * ==================================================================== */

bool
value_equal(const value_t *a, const value_t *b)
{
    if (a == b) return true;
    if (a == NULL || b == NULL) return false;
    if (a->kind != b->kind) return false;
    if (a->type != b->type) return false;

    switch (a->kind) {
        case VAL_NULL:   return true;
        case VAL_BOOL:   return a->u.b == b->u.b;
        case VAL_INT:    return a->u.i == b->u.i;
        case VAL_FLOAT:  return a->u.f == b->u.f;
        case VAL_STRING: return strcmp(a->u.s, b->u.s) == 0;

        case VAL_LIST: {
            if (a->u.list.n != b->u.list.n) return false;
            for (size_t i = 0; i < a->u.list.n; i++) {
                if (!value_equal(a->u.list.items[i], b->u.list.items[i])) {
                    return false;
                }
            }
            return true;
        }

        case VAL_RECORD: {
            if (a->u.record.n != b->u.record.n) return false;
            for (size_t i = 0; i < a->u.record.n; i++) {
                if (strcmp(a->u.record.fields[i].name,
                           b->u.record.fields[i].name) != 0) {
                    return false;
                }
                if (!value_equal(a->u.record.fields[i].value,
                                 b->u.record.fields[i].value)) {
                    return false;
                }
            }
            return true;
        }

        case VAL_VARIANT: {
            if (strcmp(a->u.variant.variant_name,
                       b->u.variant.variant_name) != 0) {
                return false;
            }
            if (a->u.variant.n != b->u.variant.n) return false;
            for (size_t i = 0; i < a->u.variant.n; i++) {
                if (strcmp(a->u.variant.fields[i].name,
                           b->u.variant.fields[i].name) != 0) {
                    return false;
                }
                if (!value_equal(a->u.variant.fields[i].value,
                                 b->u.variant.fields[i].value)) {
                    return false;
                }
            }
            return true;
        }
    }
    return false;  /* unreachable */
}


/* ====================================================================
 * Canonical serialization
 * ==================================================================== */

void
value_emit_json_string(FILE *out, const char *s)
{
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\b': fputs("\\b",  out); break;
            case '\f': fputs("\\f",  out); break;
            case '\n': fputs("\\n",  out); break;
            case '\r': fputs("\\r",  out); break;
            case '\t': fputs("\\t",  out); break;
            default:
                if (*p < 0x20) {
                    fprintf(out, "\\u%04x", (unsigned)*p);
                } else {
                    /* Bytes >= 0x80 (UTF-8 continuation/lead bytes) are
                     * emitted raw, not \u-escaped: the canonical form keeps
                     * valid UTF-8 verbatim so the byte stream — and thus the
                     * hash — matches the source string exactly. */
                    fputc((int)*p, out);
                }
        }
    }
    fputc('"', out);
}

static void
emit_int(FILE *out, int64_t i)
{
    fprintf(out, "%" PRId64, i);
}

static void
emit_float(FILE *out, double f)
{
    /* Non-finite doubles have no JSON representation and %g renders
     * their sign/spelling differently across libc implementations
     * (glibc "-nan" vs BSD "nan"), which would break cross-machine
     * byte-determinism. Force a single deterministic token for each. */
    if (f != f)            { fputs("nan",  out); return; }  /* NaN */
    if (f ==  (1.0/0.0))   { fputs("inf",  out); return; }  /* +Inf */
    if (f == -(1.0/0.0))   { fputs("-inf", out); return; }  /* -Inf */
    /* Normalize negative zero to "0" so that -0.0 and 0.0 — which
     * compare equal under IEEE `==` (and so under value_equal) —
     * serialize, and therefore hash, identically. */
    if (f == 0.0) { fputs("0", out); return; }

    char buf[64];
    int n = snprintf(buf, sizeof buf, "%.17g", f);
    if (n < 0 || (size_t)n >= sizeof buf) {
        fputs("null", out);
        return;
    }
    fputs(buf, out);
}

/* qsort comparator: sorts value_field_t * by name in UTF-8 byte order.
 * The pointers come from a transient array we allocate per-call;
 * sorting in place is fine because the source record's fields remain
 * untouched. */
static int
cmp_field_name(const void *a, const void *b)
{
    const value_field_t *fa = *(const value_field_t *const *)a;
    const value_field_t *fb = *(const value_field_t *const *)b;
    int c = strcmp(fa->name, fb->name);
    if (c != 0) return c;
    /* Names are unique by construction — value records are built from
     * type-validated IR, and the loader rejects duplicate field names
     * (R150) — so equal names are normally unreachable. We still
     * tie-break on original position (the idx pointers all point into
     * one source array) so the canonical form, and thus the content
     * hash, stays deterministic even for a value built outside that
     * path; qsort alone is not a stable sort and would leave the order
     * of equal-named fields unspecified. */
    return (fa < fb) ? -1 : (fa > fb) ? 1 : 0;
}

static void emit_value(FILE *out, const value_t *v);

/* Emit an object whose keys are the names of the supplied field array,
 * sorted lex by UTF-8 byte order, values recursively serialized.
 * Records and variant payloads share this helper. */
static void
emit_object(FILE *out, const value_field_t *fields, size_t n)
{
    if (n == 0) {
        fputs("{}", out);
        return;
    }
    /* Index array for sorting. We sort pointers into the source array
     * rather than copying fields so the source layout is untouched. */
    const value_field_t **idx = malloc(n * sizeof *idx);
    if (idx == NULL) {
        /* Out-of-memory in a canonicalization path is fatal — the
         * downstream hash cannot be computed without it. */
        FLOWD_ICE("emit_object: idx allocation failed");
    }
    for (size_t i = 0; i < n; i++) {
        idx[i] = &fields[i];
    }
    qsort(idx, n, sizeof *idx, cmp_field_name);

    fputc('{', out);
    for (size_t i = 0; i < n; i++) {
        if (i > 0) fputc(',', out);
        value_emit_json_string(out, idx[i]->name);
        fputc(':', out);
        emit_value(out, idx[i]->value);
    }
    fputc('}', out);

    free(idx);
}

static void
emit_list(FILE *out, value_t *const *items, size_t n)
{
    fputc('[', out);
    for (size_t i = 0; i < n; i++) {
        if (i > 0) fputc(',', out);
        emit_value(out, items[i]);
    }
    fputc(']', out);
}

static void
emit_variant(FILE *out, const value_t *v)
{
    /* {"fields":{...},"variant":"Name"} — keys already in lex order
     * (f < v). A variant is serialized by name only; no numeric tag
     * is stored. */
    fputs("{\"fields\":", out);
    emit_object(out, v->u.variant.fields, v->u.variant.n);
    fputs(",\"variant\":", out);
    value_emit_json_string(out, v->u.variant.variant_name);
    fputc('}', out);
}

static void
emit_value(FILE *out, const value_t *v)
{
    if (v == NULL) {
        fputs("null", out);
        return;
    }
    switch (v->kind) {
        case VAL_NULL:    fputs("null", out);                       break;
        case VAL_BOOL:    fputs(v->u.b ? "true" : "false", out);    break;
        case VAL_INT:     emit_int   (out, v->u.i);                 break;
        case VAL_FLOAT:   emit_float (out, v->u.f);                 break;
        case VAL_STRING:  value_emit_json_string(out, v->u.s);      break;
        case VAL_LIST:    emit_list  (out, v->u.list.items,
                                           v->u.list.n);            break;
        case VAL_RECORD:  emit_object(out, v->u.record.fields,
                                           v->u.record.n);          break;
        case VAL_VARIANT: emit_variant(out, v);                     break;
    }
}

void
value_canonical_serialize(const value_t *v, FILE *out)
{
    emit_value(out, v);
}


/* ====================================================================
 * Hashing
 *
 * Canonicalize into an open_memstream-backed buffer, then run
 * sha256_hex over the resulting bytes. The buffer is freed before
 * return; only the hex digest survives.
 * ==================================================================== */

void
value_hash(const value_t *v, char out_hex[65])
{
    char  *buf = NULL;
    size_t sz  = 0;
    FILE *fp = open_memstream(&buf, &sz);
    if (fp == NULL) {
        FLOWD_ICE("value_hash: open_memstream failed");
    }
    value_canonical_serialize(v, fp);
    if (fclose(fp) != 0) {
        free(buf);
        FLOWD_ICE("value_hash: fclose on memstream failed");
    }
    sha256_hex(buf, sz, out_hex);
    free(buf);
}
