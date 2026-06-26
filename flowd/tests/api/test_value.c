/* tests/api/test_value.c
 *
 * Value-model unit tests.
 *
 * Exercises the four properties the rest of the runtime depends on:
 *
 *   1. value_canonical_serialize emits canonical bytes.
 *      - Object keys lex-sorted by UTF-8 byte order (NOT declaration
 *        order). The pre-sort layout in memory is preserved; sorting
 *        happens at the serializer boundary.
 *      - Strings escape with the minimum set; UTF-8 multi-byte bytes
 *        pass through verbatim.
 *      - Booleans and null take their literal forms; integers have no
 *        decimal point.
 *      - Variants encode as {"fields":{...},"variant":"Name"}.
 *
 *   2. value_canonical_serialize is byte-deterministic: two calls
 *      against the same logical value produce identical bytes.
 *
 *   3. value_equal is structural and IEEE-754-correct (NaN ≠ NaN).
 *
 *   4. value_hash is stable for equal inputs and divergent for
 *      unequal inputs. The same canonical bytes always produce the
 *      same digest.
 *
 * The test program is self-contained: it prints a TAP-style PASS/FAIL
 * line for each case, exits 0 if every assertion held and non-zero
 * otherwise. The fixture-driven driver under tests/run.sh invokes it
 * as one of the test cases.
 *
 * Memory: a single arena is created at startup, reused across cases,
 * and destroyed at shutdown. ASAN catches any leak that survives.
 */

#include "value.h"
#include "util.h"
#include "sha256_builtin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

static void
assert_str_eq(const char *case_name, const char *got, const char *want)
{
    if (strcmp(got, want) == 0) {
        printf("  ok %s\n", case_name);
        g_pass++;
    } else {
        printf("not ok %s\n  want: %s\n  got:  %s\n",
               case_name, want, got);
        g_fail++;
    }
}

static void
assert_true(const char *case_name, bool cond)
{
    if (cond) {
        printf("  ok %s\n", case_name);
        g_pass++;
    } else {
        printf("not ok %s\n", case_name);
        g_fail++;
    }
}

/* Canonicalize into a malloc'd NUL-terminated buffer for assertion.
 * Caller frees. */
static char *
canon(const value_t *v)
{
    char  *buf = NULL;
    size_t sz  = 0;
    FILE *fp = open_memstream(&buf, &sz);
    if (fp == NULL) return NULL;
    value_canonical_serialize(v, fp);
    fclose(fp);
    return buf;
}

static value_field_t
mkfield(Arena *a, const char *name, value_t *val)
{
    value_field_t f;
    f.name  = arena_strdup(a, name);
    f.value = val;
    return f;
}


/* ====================================================================
 * Canonical-serialization cases
 * ==================================================================== */

static void test_primitives(Arena *a)
{
    char *s;

    s = canon(value_new_null(a, TYPE_ID_NONE));
    assert_str_eq("null", s, "null"); free(s);

    s = canon(value_new_bool(a, TYPE_ID_BOOL, true));
    assert_str_eq("bool/true", s, "true"); free(s);

    s = canon(value_new_bool(a, TYPE_ID_BOOL, false));
    assert_str_eq("bool/false", s, "false"); free(s);

    s = canon(value_new_int(a, TYPE_ID_INT, 0));
    assert_str_eq("int/0", s, "0"); free(s);

    s = canon(value_new_int(a, TYPE_ID_INT, -1));
    assert_str_eq("int/-1", s, "-1"); free(s);

    s = canon(value_new_int(a, TYPE_ID_INT, 9223372036854775807LL));
    assert_str_eq("int/max", s, "9223372036854775807"); free(s);

    s = canon(value_new_float(a, TYPE_ID_FLOAT, 0.5));
    assert_str_eq("float/0.5", s, "0.5"); free(s);

    s = canon(value_new_string(a, TYPE_ID_STRING, "hello"));
    assert_str_eq("string/plain", s, "\"hello\""); free(s);

    /* Empty string. */
    s = canon(value_new_string(a, TYPE_ID_STRING, ""));
    assert_str_eq("string/empty", s, "\"\""); free(s);
}

static void test_string_escapes(Arena *a)
{
    char *s;

    s = canon(value_new_string(a, TYPE_ID_STRING, "a\"b"));
    assert_str_eq("string/escape-quote", s, "\"a\\\"b\""); free(s);

    s = canon(value_new_string(a, TYPE_ID_STRING, "a\\b"));
    assert_str_eq("string/escape-backslash", s, "\"a\\\\b\""); free(s);

    s = canon(value_new_string(a, TYPE_ID_STRING, "\b\f\n\r\t"));
    assert_str_eq("string/escape-controls",
                  s, "\"\\b\\f\\n\\r\\t\""); free(s);

    /* Control char below U+0020 that doesn't have a named escape. */
    s = canon(value_new_string(a, TYPE_ID_STRING, "x\x01y"));
    assert_str_eq("string/escape-u0001",
                  s, "\"x\\u0001y\""); free(s);

    /* UTF-8 multi-byte: U+00E9 (é) = 0xC3 0xA9. Non-ASCII
     * passes through as its UTF-8 bytes — no \u escaping required. */
    s = canon(value_new_string(a, TYPE_ID_STRING, "caf\xC3\xA9"));
    assert_str_eq("string/utf8-passthrough",
                  s, "\"caf\xC3\xA9\""); free(s);
}

static void test_record_key_order(Arena *a)
{
    /* Declaration order: zeta, alpha, mike. Canonical lex order:
     * alpha, mike, zeta. The serializer must reorder. */
    value_field_t *fields = arena_alloc(a, 3 * sizeof *fields);
    fields[0] = mkfield(a, "zeta",  value_new_int(a, TYPE_ID_INT, 3));
    fields[1] = mkfield(a, "alpha", value_new_int(a, TYPE_ID_INT, 1));
    fields[2] = mkfield(a, "mike",  value_new_int(a, TYPE_ID_INT, 2));

    value_t *r = value_new_record_take(a, 100u, fields, 3);
    char *s = canon(r);
    assert_str_eq("record/lex-sorted-keys",
                  s,
                  "{\"alpha\":1,\"mike\":2,\"zeta\":3}");
    free(s);
}

static void test_record_empty(Arena *a)
{
    value_t *r = value_new_record_take(a, 100u, NULL, 0);
    char *s = canon(r);
    assert_str_eq("record/empty", s, "{}"); free(s);
}

static void test_list_source_order(Arena *a)
{
    /* List preserves source order, in contrast to records. */
    value_t **items = arena_alloc(a, 3 * sizeof *items);
    items[0] = value_new_int(a, TYPE_ID_INT, 30);
    items[1] = value_new_int(a, TYPE_ID_INT, 10);
    items[2] = value_new_int(a, TYPE_ID_INT, 20);

    value_t *l = value_new_list_take(a, 1000u, items, 3);
    char *s = canon(l);
    assert_str_eq("list/source-order", s, "[30,10,20]"); free(s);

    /* Empty list. */
    value_t *empty = value_new_list_take(a, 1000u, NULL, 0);
    s = canon(empty);
    assert_str_eq("list/empty", s, "[]"); free(s);
}

static void test_variant_canonical(Arena *a)
{
    /* Variant: name "Approved" with two payload fields {amount: 100,
     * note: "ok"}. Canonical: {"fields":{...},"variant":"Approved"}
     * with payload keys also lex-sorted. */
    value_field_t *fields = arena_alloc(a, 2 * sizeof *fields);
    fields[0] = mkfield(a, "note",   value_new_string(a, TYPE_ID_STRING, "ok"));
    fields[1] = mkfield(a, "amount", value_new_int(a, TYPE_ID_INT, 100));

    value_t *v = value_new_variant_take(a, 101u, "Approved", fields, 2);
    char *s = canon(v);
    assert_str_eq("variant/canonical",
                  s,
                  "{\"fields\":{\"amount\":100,\"note\":\"ok\"},"
                  "\"variant\":\"Approved\"}");
    free(s);
}

static void test_nested(Arena *a)
{
    /* {a: [{x:1}, {x:2}], b: true} → keys sorted, inner records too. */
    value_field_t *inner1 = arena_alloc(a, sizeof *inner1);
    inner1[0] = mkfield(a, "x", value_new_int(a, TYPE_ID_INT, 1));
    value_t *r1 = value_new_record_take(a, 100u, inner1, 1);

    value_field_t *inner2 = arena_alloc(a, sizeof *inner2);
    inner2[0] = mkfield(a, "x", value_new_int(a, TYPE_ID_INT, 2));
    value_t *r2 = value_new_record_take(a, 100u, inner2, 1);

    value_t **items = arena_alloc(a, 2 * sizeof *items);
    items[0] = r1;
    items[1] = r2;
    value_t *list = value_new_list_take(a, 1000u, items, 2);

    value_field_t *outer = arena_alloc(a, 2 * sizeof *outer);
    outer[0] = mkfield(a, "b", value_new_bool(a, TYPE_ID_BOOL, true));
    outer[1] = mkfield(a, "a", list);
    value_t *r = value_new_record_take(a, 100u, outer, 2);

    char *s = canon(r);
    assert_str_eq("nested/sorted-and-recursive",
                  s,
                  "{\"a\":[{\"x\":1},{\"x\":2}],\"b\":true}");
    free(s);
}


/* ====================================================================
 * Determinism: the same value canonicalizes to the same bytes twice
 * ==================================================================== */

static void test_determinism(Arena *a)
{
    value_field_t *fields = arena_alloc(a, 3 * sizeof *fields);
    fields[0] = mkfield(a, "z", value_new_int(a, TYPE_ID_INT, 26));
    fields[1] = mkfield(a, "a", value_new_int(a, TYPE_ID_INT, 1));
    fields[2] = mkfield(a, "m", value_new_int(a, TYPE_ID_INT, 13));
    value_t *r = value_new_record_take(a, 100u, fields, 3);

    char *s1 = canon(r);
    char *s2 = canon(r);
    assert_true("determinism/same-bytes-twice", strcmp(s1, s2) == 0);
    free(s1); free(s2);
}


/* ====================================================================
 * Structural equality
 * ==================================================================== */

static void test_equality(Arena *a)
{
    /* Same kind, same value, same type id → equal. */
    value_t *i1 = value_new_int(a, TYPE_ID_INT, 42);
    value_t *i2 = value_new_int(a, TYPE_ID_INT, 42);
    assert_true("equal/int-same", value_equal(i1, i2));

    /* Different value → not equal. */
    value_t *i3 = value_new_int(a, TYPE_ID_INT, 43);
    assert_true("equal/int-diff-value", !value_equal(i1, i3));

    /* Same value, different type id → not equal (nominal typing). */
    value_t *i4 = value_new_int(a, 999u, 42);
    assert_true("equal/int-diff-type", !value_equal(i1, i4));

    /* NaN ≠ NaN per IEEE 754. */
    value_t *n1 = value_new_float(a, TYPE_ID_FLOAT, NAN);
    value_t *n2 = value_new_float(a, TYPE_ID_FLOAT, NAN);
    assert_true("equal/nan-not-nan", !value_equal(n1, n2));

    /* Records with same fields in same order — equal. */
    value_field_t *fa = arena_alloc(a, 2 * sizeof *fa);
    fa[0] = mkfield(a, "k", value_new_int(a, TYPE_ID_INT, 1));
    fa[1] = mkfield(a, "j", value_new_int(a, TYPE_ID_INT, 2));
    value_t *ra = value_new_record_take(a, 100u, fa, 2);

    value_field_t *fb = arena_alloc(a, 2 * sizeof *fb);
    fb[0] = mkfield(a, "k", value_new_int(a, TYPE_ID_INT, 1));
    fb[1] = mkfield(a, "j", value_new_int(a, TYPE_ID_INT, 2));
    value_t *rb = value_new_record_take(a, 100u, fb, 2);

    assert_true("equal/record-same", value_equal(ra, rb));

    /* List equality (admitted at runtime). */
    value_t **xs = arena_alloc(a, 2 * sizeof *xs);
    xs[0] = value_new_int(a, TYPE_ID_INT, 1);
    xs[1] = value_new_int(a, TYPE_ID_INT, 2);
    value_t *la = value_new_list_take(a, 1000u, xs, 2);

    value_t **ys = arena_alloc(a, 2 * sizeof *ys);
    ys[0] = value_new_int(a, TYPE_ID_INT, 1);
    ys[1] = value_new_int(a, TYPE_ID_INT, 2);
    value_t *lb = value_new_list_take(a, 1000u, ys, 2);

    assert_true("equal/list-same", value_equal(la, lb));
}


/* ====================================================================
 * Hashing
 * ==================================================================== */

static void test_hashing(Arena *a)
{
    /* Same logical value → same hash, twice. */
    value_t *v1 = value_new_int(a, TYPE_ID_INT, 42);
    value_t *v2 = value_new_int(a, TYPE_ID_INT, 42);
    char h1[65], h2[65];
    value_hash(v1, h1);
    value_hash(v2, h2);
    assert_true("hash/same-value-same-digest", strcmp(h1, h2) == 0);

    /* The digest of "42" canonical bytes is well-known. */
    char expected[65];
    sha256_hex_string("42", expected);
    assert_str_eq("hash/int-42-matches-known", h1, expected);

    /* Different values → different digests. */
    value_t *v3 = value_new_int(a, TYPE_ID_INT, 43);
    char h3[65];
    value_hash(v3, h3);
    assert_true("hash/diff-value-diff-digest", strcmp(h1, h3) != 0);

    /* Records that differ only in declaration order produce the SAME
     * hash, because canonical sorts the keys. This is the load-bearing
     * property for content addressing: a host that re-emits the same
     * logical record with shuffled fields gets the same hash. */
    value_field_t *fa = arena_alloc(a, 2 * sizeof *fa);
    fa[0] = mkfield(a, "k", value_new_int(a, TYPE_ID_INT, 1));
    fa[1] = mkfield(a, "j", value_new_int(a, TYPE_ID_INT, 2));
    value_t *ra = value_new_record_take(a, 100u, fa, 2);

    value_field_t *fb = arena_alloc(a, 2 * sizeof *fb);
    fb[0] = mkfield(a, "j", value_new_int(a, TYPE_ID_INT, 2));
    fb[1] = mkfield(a, "k", value_new_int(a, TYPE_ID_INT, 1));
    value_t *rb = value_new_record_take(a, 100u, fb, 2);

    char hra[65], hrb[65];
    value_hash(ra, hra);
    value_hash(rb, hrb);
    assert_true("hash/declaration-order-irrelevant",
                strcmp(hra, hrb) == 0);

    /* The vendored SHA-256 (no-libcrypto / WASM builds) must agree with the
     * linked one byte for byte, or the content-addressed value store would
     * split between builds. */
    unsigned char dg[32];
    char hbuilt[65], hlinked[65];
    flowd_sha256_builtin("the quick brown fox", 19u, dg);
    for (int i = 0; i < 32; i++)
        sprintf(hbuilt + 2 * i, "%02x", dg[i]);
    sha256_hex("the quick brown fox", 19u, hlinked);
    assert_str_eq("hash/builtin-matches-linked", hbuilt, hlinked);

    /* A 120-byte message has a tail >= 56, exercising the two-block padding
     * path (the length trailer spills past the first block). */
    char big[120];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (char)(i * 3 + 1);
    flowd_sha256_builtin(big, sizeof big, dg);
    for (int i = 0; i < 32; i++)
        sprintf(hbuilt + 2 * i, "%02x", dg[i]);
    sha256_hex(big, sizeof big, hlinked);
    assert_str_eq("hash/builtin-matches-linked-long", hbuilt, hlinked);
}


/* ====================================================================
 * Entry point
 * ==================================================================== */

int main(void)
{
    Arena *a = arena_create(0);
    if (a == NULL) {
        fputs("arena_create failed\n", stderr);
        return 4;
    }

    test_primitives(a);
    test_string_escapes(a);
    test_record_key_order(a);
    test_record_empty(a);
    test_list_source_order(a);
    test_variant_canonical(a);
    test_nested(a);
    test_determinism(a);
    test_equality(a);
    test_hashing(a);

    arena_destroy(a);

    printf("\nPASS %d  FAIL %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
