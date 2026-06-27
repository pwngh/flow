/* src/ir_load.c
 *
 * IR loader. Builds an in-memory representation of a v1 IR JSON file
 * suitable for runtime introspection. Scope: parse, validate,
 * resolve types, populate tool and flow tables, build the variant
 * index, store flow bodies as opaque cJSON. No execution.
 *
 * Lifetime: every derived structure is arena-owned. The cJSON tree
 * remains alive in the runtime handle so flow bindings/return
 * pointers stay valid; cJSON_Delete fires at flowd_destroy. cJSON
 * uses its default system-malloc allocator — wiring it to the arena
 * would require per-allocation free-no-ops and is not worth the
 * complexity for v1.
 *
 * Layout:
 *
 *   1. type_registry — the structure plus its helpers (name lookup,
 *      list interning, the variant index, canonical iteration).
 *   2. flowd_runtime — the opaque struct + getters.
 *   3. The loader passes — file slurp, JSON parse, top-level
 *      validation, two-pass type resolution, variant indexing, tool
 *      and flow walks.
 *   4. flowd_canonical_dump_lite — the introspection serializer used
 *      by --load-ir and fixture diffs.
 */

#include "ir_load.h"
#include "runtime_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ====================================================================
 * type_registry
 * ==================================================================== */

#define NAMED_TYPE_ID_START  ((type_id_t)100)
#define LIST_TYPE_ID_START   ((type_id_t)1000)

typedef struct {
    const char *name;
    type_id_t   id;
} name_entry_t;

typedef struct {
    const char *name;
    type_id_t   sum_type;
    uint32_t    variant_idx;
} variant_entry_t;

struct type_registry {
    Arena   *arena;

    /* All types in registration order: primitives, then named types
     * in source order, then interned lists in first-seen order. */
    type_t **all;
    size_t   n_all;
    size_t   cap_all;

    /* name -> id lookup (linear scan; counts are small). Holds entries
     * for primitives, every named user type, and (so list reuse works
     * efficiently) every interned list under its full "[T]" key. */
    name_entry_t *by_name;
    size_t        n_by_name;
    size_t        cap_by_name;

    /* Variant index, sorted by variant name. Sorting happens after
     * all sums are loaded; before that, entries land in insertion
     * order. */
    variant_entry_t *vidx;
    size_t           n_vidx;
    size_t           cap_vidx;
    bool             vidx_sorted;

    type_id_t next_named_id;
    type_id_t next_list_id;
};

/* Grow a generic pointer/struct array by re-allocating via the arena.
 * The old allocation is left in the arena and freed at arena_destroy.
 * For a one-shot load over a v1 IR (typically <30 types, <50 tools)
 * the wasted space is negligible. */
static void
grow_array(Arena *a, void **items, size_t *cap, size_t elem_size,
           size_t at_least)
{
    if (*cap >= at_least) return;
    size_t new_cap = *cap ? (*cap) * 2u : 8u;
    while (new_cap < at_least) new_cap *= 2u;
    void *grown = arena_alloc(a, new_cap * elem_size);
    if (*items != NULL && *cap > 0) {
        memcpy(grown, *items, (*cap) * elem_size);
    }
    *items = grown;
    *cap   = new_cap;
}

static void
register_name(type_registry_t *r, const char *name, type_id_t id)
{
    grow_array(r->arena, (void **)&r->by_name, &r->cap_by_name,
               sizeof *r->by_name, r->n_by_name + 1u);
    r->by_name[r->n_by_name].name = name;
    r->by_name[r->n_by_name].id   = id;
    r->n_by_name++;
}

static void
register_type(type_registry_t *r, type_t *t)
{
    grow_array(r->arena, (void **)&r->all, &r->cap_all,
               sizeof *r->all, r->n_all + 1u);
    r->all[r->n_all++] = t;
}

static type_t *
new_primitive(Arena *a, type_id_t id, const char *name)
{
    type_t *t = arena_alloc_zero(a, sizeof *t);
    t->id   = id;
    t->kind = TYPE_KIND_PRIMITIVE;
    t->name = arena_strdup(a, name);
    return t;
}

type_registry_t *
type_registry_create(Arena *arena)
{
    type_registry_t *r = arena_alloc_zero(arena, sizeof *r);
    r->arena         = arena;
    r->next_named_id = NAMED_TYPE_ID_START;
    r->next_list_id  = LIST_TYPE_ID_START;
    r->vidx_sorted   = true;

    /* Primitives at fixed slots. Names are registered so lookups
     * during type-reference resolution (which walks by_name) hit a
     * single code path. */
    type_t *p_str = new_primitive(arena, TYPE_ID_STRING, "string");
    type_t *p_int = new_primitive(arena, TYPE_ID_INT,    "int");
    type_t *p_flt = new_primitive(arena, TYPE_ID_FLOAT,  "float");
    type_t *p_bln = new_primitive(arena, TYPE_ID_BOOL,   "bool");
    register_type(r, p_str); register_name(r, p_str->name, p_str->id);
    register_type(r, p_int); register_name(r, p_int->name, p_int->id);
    register_type(r, p_flt); register_name(r, p_flt->name, p_flt->id);
    register_type(r, p_bln); register_name(r, p_bln->name, p_bln->id);

    return r;
}

/* Look up a name in the registry's flat by_name table. Returns
 * TYPE_ID_NONE on miss. List type strings ("[T]") are recognized
 * after their first intern; bare names are recognized after their
 * registration. */
static type_id_t
lookup_exact(const type_registry_t *r, const char *name)
{
    for (size_t i = 0; i < r->n_by_name; i++) {
        if (strcmp(r->by_name[i].name, name) == 0) {
            return r->by_name[i].id;
        }
    }
    return TYPE_ID_NONE;
}

/* Intern a list type whose element is `elem`. Stores the canonical
 * "[Element]" form so subsequent lookups by string hit the by_name
 * table directly. The element type must already be registered. */
type_id_t
type_registry_intern_list(type_registry_t *r, type_id_t elem)
{
    /* Already interned? Scan all registered types for a list whose
     * element matches `elem`. (Linear; counts are tiny.) */
    for (size_t i = 0; i < r->n_all; i++) {
        const type_t *t = r->all[i];
        if (t->kind == TYPE_KIND_LIST && t->elem == elem) {
            return t->id;
        }
    }

    const type_t *et = type_registry_get(r, elem);
    if (et == NULL) return TYPE_ID_NONE;

    /* The bracketed key nests: the element's own name may itself be a
     * previously-interned list's "[…]" form, so "[[int]]" and friends
     * round-trip through lookup_by_name without a special case. */
    size_t en = strlen(et->name);
    char  *buf = arena_alloc(r->arena, en + 3u);
    buf[0] = '[';
    memcpy(buf + 1, et->name, en);
    buf[en + 1u] = ']';
    buf[en + 2u] = '\0';

    type_t *t = arena_alloc_zero(r->arena, sizeof *t);
    t->id   = r->next_list_id++;
    t->kind = TYPE_KIND_LIST;
    t->name = buf;
    t->elem = elem;
    register_type(r, t);
    register_name(r, buf, t->id);
    return t->id;
}

/* List nesting cap. Real IR has single- or low-digit nesting; this
 * bounds recursion (and the per-level interning work) so a crafted
 * deeply-nested list string cannot overflow the C stack or hang the
 * loader — past the cap the name is simply treated as unknown. */
#define TYPE_NEST_MAX 64

static type_id_t
lookup_by_name_depth(type_registry_t *r, const char *name, int depth)
{
    if (name == NULL || name[0] == '\0') return TYPE_ID_NONE;
    if (depth > TYPE_NEST_MAX) return TYPE_ID_NONE;

    /* Exact hit covers primitives, named types, and already-interned
     * list forms. */
    type_id_t id = lookup_exact(r, name);
    if (id != TYPE_ID_NONE) return id;

    /* List form "[T]": strip brackets, recurse on T, intern. */
    size_t n = strlen(name);
    if (n >= 2 && name[0] == '[' && name[n - 1u] == ']') {
        char *inner = arena_alloc(r->arena, n - 1u);
        memcpy(inner, name + 1, n - 2u);
        inner[n - 2u] = '\0';
        type_id_t elem = lookup_by_name_depth(r, inner, depth + 1);
        if (elem == TYPE_ID_NONE) return TYPE_ID_NONE;
        return type_registry_intern_list(r, elem);
    }

    return TYPE_ID_NONE;
}

type_id_t
type_registry_lookup_by_name(type_registry_t *r, const char *name)
{
    return lookup_by_name_depth(r, name, 0);
}

const type_t *
type_registry_get(const type_registry_t *r, type_id_t id)
{
    if (id == TYPE_ID_NONE) return NULL;
    for (size_t i = 0; i < r->n_all; i++) {
        if (r->all[i]->id == id) return r->all[i];
    }
    return NULL;
}

size_t
type_registry_count(const type_registry_t *r) { return r->n_all; }

const type_t *
type_registry_at(const type_registry_t *r, size_t idx)
{
    return idx < r->n_all ? r->all[idx] : NULL;
}

/* qsort comparator for the variant index: sort by name, ties broken
 * by sum_type and then variant_idx so the order is total and stable. */
static int
cmp_variant(const void *a, const void *b)
{
    const variant_entry_t *va = a;
    const variant_entry_t *vb = b;
    int c = strcmp(va->name, vb->name);
    if (c != 0) return c;
    if (va->sum_type != vb->sum_type)
        return va->sum_type < vb->sum_type ? -1 : 1;
    if (va->variant_idx != vb->variant_idx)
        return va->variant_idx < vb->variant_idx ? -1 : 1;
    return 0;
}

static void
variant_index_sort(type_registry_t *r)
{
    if (r->vidx_sorted) return;
    if (r->n_vidx > 1) {
        qsort(r->vidx, r->n_vidx, sizeof *r->vidx, cmp_variant);
    }
    r->vidx_sorted = true;
}

static void
variant_index_add(type_registry_t *r, const char *name,
                  type_id_t sum_type, uint32_t variant_idx)
{
    grow_array(r->arena, (void **)&r->vidx, &r->cap_vidx,
               sizeof *r->vidx, r->n_vidx + 1u);
    r->vidx[r->n_vidx].name        = name;
    r->vidx[r->n_vidx].sum_type    = sum_type;
    r->vidx[r->n_vidx].variant_idx = variant_idx;
    r->n_vidx++;
    r->vidx_sorted = false;
}

size_t
type_registry_variant_index_count(const type_registry_t *r)
{
    return r->n_vidx;
}

const char *
type_registry_variant_index_at(const type_registry_t *r, size_t idx,
                               type_id_t *out_sum, uint32_t *out_variant_idx)
{
    type_registry_t *mut = (type_registry_t *)r;
    variant_index_sort(mut);

    if (idx >= r->n_vidx) return NULL;
    if (out_sum)         *out_sum         = r->vidx[idx].sum_type;
    if (out_variant_idx) *out_variant_idx = r->vidx[idx].variant_idx;
    return r->vidx[idx].name;
}


/* ====================================================================
 * flowd_runtime
 * ==================================================================== */

/* flowd_runtime struct lives in runtime_internal.h so exec.c and
 * flowd.c can access fields directly. */

const type_registry_t *flowd_types(const flowd_runtime *rt)
{
    return rt ? rt->types : NULL;
}
size_t flowd_tool_count(const flowd_runtime *rt)
{
    return rt ? rt->n_tools : 0;
}
const tool_t *flowd_tool_at(const flowd_runtime *rt, size_t i)
{
    if (!rt || i >= rt->n_tools) return NULL;
    return &rt->tools[i];
}
const tool_t *flowd_tool_by_name(const flowd_runtime *rt, const char *name)
{
    if (!rt) return NULL;
    for (size_t i = 0; i < rt->n_tools; i++) {
        if (strcmp(rt->tools[i].name, name) == 0) return &rt->tools[i];
    }
    return NULL;
}
size_t flowd_flow_count(const flowd_runtime *rt)
{
    return rt ? rt->n_flows : 0;
}
const flow_t *flowd_flow_at(const flowd_runtime *rt, size_t i)
{
    if (!rt || i >= rt->n_flows) return NULL;
    return &rt->flows[i];
}
const flow_t *flowd_flow_by_name(const flowd_runtime *rt, const char *name)
{
    if (!rt) return NULL;
    for (size_t i = 0; i < rt->n_flows; i++) {
        if (strcmp(rt->flows[i].name, name) == 0) return &rt->flows[i];
    }
    return NULL;
}


/* ====================================================================
 * Loader passes
 * ==================================================================== */

typedef struct {
    flowd_runtime *rt;
    DiagStream    *diag;
    SrcLoc         loc;            /* file path; line/column unused for JSON */
} load_ctx_t;

/* Emit an error diagnostic referencing a JSON path like
 * "tools[3].input[1].type". The diag stream records and (when
 * attached) writes to stderr. */
static void
emit_err(load_ctx_t *c, const char *id, const char *path, const char *fmt, ...)
{
    char  buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (path && path[0]) {
        diag_emit(c->diag, c->loc, DIAG_ERROR, id,
                  "%s: %s", path, buf);
    } else {
        diag_emit(c->diag, c->loc, DIAG_ERROR, id, "%s", buf);
    }
}

/* Thin cJSON wrappers: NULL-tolerant type probes that return
 * NULL/false on a miss (the cJSON_Is* predicates accept NULL), so
 * callers can chain lookups without per-step NULL guards. Diagnostics
 * are emitted by the callers, not here. Object access is case-sensitive
 * so IR keys never collide on case. */
static const cJSON *
json_obj_get(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

static const char *
json_string(const cJSON *v)
{
    return cJSON_IsString(v) ? cJSON_GetStringValue(v) : NULL;
}

static bool
json_is_array(const cJSON *v) { return cJSON_IsArray(v); }

static bool
json_is_object(const cJSON *v) { return cJSON_IsObject(v); }

static int
json_array_size(const cJSON *v) { return cJSON_GetArraySize(v); }


/* ---- Slurp file ---- */
static char *
slurp_file(const char *path, size_t *out_len, DiagStream *diag)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        diag_emit(diag, (SrcLoc){ path, 0, 0 }, DIAG_ERROR, "R150",
                  "cannot open file: %s", strerror(errno));
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        diag_emit(diag, (SrcLoc){ path, 0, 0 }, DIAG_ERROR, "R150",
                  "fseek failed");
        return NULL;
    }
    long sz = ftell(fp);
    rewind(fp);
    if (sz < 0) {
        fclose(fp);
        diag_emit(diag, (SrcLoc){ path, 0, 0 }, DIAG_ERROR, "R150",
                  "ftell failed");
        return NULL;
    }
    char *buf = malloc((size_t)sz + 1u);
    if (!buf) {
        fclose(fp);
        FLOWD_ICE("slurp_file: out of memory (%ld bytes)", sz);
    }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) {
        free(buf);
        diag_emit(diag, (SrcLoc){ path, 0, 0 }, DIAG_ERROR, "R150",
                  "short read");
        return NULL;
    }
    buf[sz] = '\0';
    *out_len = (size_t)sz;
    return buf;
}


/* ---- Type resolution ---- */

/* Resolve a "type" string at a particular JSON path; emit on miss. */
static type_id_t
resolve_type_ref(load_ctx_t *c, const cJSON *jval, const char *json_path)
{
    if (!cJSON_IsString(jval)) {
        emit_err(c, "R150", json_path,
                 "expected a JSON string for type reference");
        return TYPE_ID_NONE;
    }
    const char *name = cJSON_GetStringValue(jval);
    type_id_t id = type_registry_lookup_by_name(c->rt->types, name);
    if (id == TYPE_ID_NONE) {
        emit_err(c, "R150", json_path,
                 "unknown type reference: %s", name);
    }
    return id;
}

/* Pass 3: assign each record/sum its final id and register its name,
 * leaving fields/variants empty. Splitting registration from body
 * resolution (pass 4) lets types reference each other — and themselves
 * — in any order, since every name already exists by the time pass 4
 * resolves a body. */
static bool
pass_types_register(load_ctx_t *c, const cJSON *types_arr)
{
    int n = json_array_size(types_arr);
    for (int i = 0; i < n; i++) {
        cJSON *t = cJSON_GetArrayItem(types_arr, i);
        if (!json_is_object(t)) {
            char p[64]; snprintf(p, sizeof p, "types[%d]", i);
            emit_err(c, "R150", p, "expected an object");
            return false;
        }
        const char *name = json_string(json_obj_get(t, "name"));
        const char *kind = json_string(json_obj_get(t, "kind"));
        if (!name || !kind) {
            char p[64]; snprintf(p, sizeof p, "types[%d]", i);
            emit_err(c, "R150", p, "missing required field 'name' or 'kind'");
            return false;
        }
        if (strcmp(kind, "record") != 0 && strcmp(kind, "sum") != 0) {
            char p[64]; snprintf(p, sizeof p, "types[%d].kind", i);
            emit_err(c, "R150", p, "expected 'record' or 'sum', got '%s'",
                     kind);
            return false;
        }
        if (lookup_exact(c->rt->types, name) != TYPE_ID_NONE) {
            char p[64]; snprintf(p, sizeof p, "types[%d].name", i);
            emit_err(c, "R150", p, "duplicate type name '%s'", name);
            return false;
        }

        /* Named-type ids occupy [NAMED_TYPE_ID_START, LIST_TYPE_ID_START);
         * past that they would collide with the interned-list id range and
         * break the "one comparison classifies any id" invariant. Reject
         * an IR with more named types than the range holds rather than
         * silently overlap. */
        if (c->rt->types->next_named_id >= LIST_TYPE_ID_START) {
            char p[64]; snprintf(p, sizeof p, "types[%d].name", i);
            emit_err(c, "R150", p, "too many named types (max %u)",
                     (unsigned)(LIST_TYPE_ID_START - NAMED_TYPE_ID_START));
            return false;
        }
        type_t *tt = arena_alloc_zero(c->rt->arena, sizeof *tt);
        tt->id   = c->rt->types->next_named_id++;
        tt->name = arena_strdup(c->rt->arena, name);
        tt->kind = strcmp(kind, "record") == 0
                   ? TYPE_KIND_RECORD : TYPE_KIND_SUM;
        register_type(c->rt->types, tt);
        register_name(c->rt->types, tt->name, tt->id);
    }
    return true;
}

/* Pass 4: resolve record fields and variant payload types. Forward
 * references inside lists (`[Self]` inside a sum variant) work
 * because every name was registered in pass 3. */
static bool
pass_types_resolve(load_ctx_t *c, const cJSON *types_arr)
{
    int n = json_array_size(types_arr);
    for (int i = 0; i < n; i++) {
        cJSON *t = cJSON_GetArrayItem(types_arr, i);
        const char *name = json_string(json_obj_get(t, "name"));
        type_id_t   id   = lookup_exact(c->rt->types, name);
        type_t     *tt   = NULL;
        /* Re-find the type_t we registered in pass 3. */
        for (size_t k = 0; k < c->rt->types->n_all; k++) {
            if (c->rt->types->all[k]->id == id) {
                tt = c->rt->types->all[k];
                break;
            }
        }
        if (!tt) {
            char p[64]; snprintf(p, sizeof p, "types[%d]", i);
            emit_err(c, "R150", p, "internal: pass-3 registration lost");
            return false;
        }

        if (tt->kind == TYPE_KIND_RECORD) {
            const cJSON *fields = json_obj_get(t, "fields");
            if (!json_is_array(fields)) {
                char p[64]; snprintf(p, sizeof p, "types[%d].fields", i);
                emit_err(c, "R150", p, "expected an array");
                return false;
            }
            int nf = json_array_size(fields);
            tt->fields   = arena_alloc_zero(c->rt->arena,
                                            (size_t)nf * sizeof *tt->fields);
            tt->n_fields = (size_t)nf;
            for (int j = 0; j < nf; j++) {
                cJSON *f = cJSON_GetArrayItem(fields, j);
                const char *fname = json_string(json_obj_get(f, "name"));
                const cJSON *fty  = json_obj_get(f, "type");
                if (!fname || !fty) {
                    char p[96];
                    snprintf(p, sizeof p, "types[%d].fields[%d]", i, j);
                    emit_err(c, "R150", p, "missing 'name' or 'type'");
                    return false;
                }
                for (int k = 0; k < j; k++) {
                    if (strcmp(tt->fields[k].name, fname) == 0) {
                        char p[96];
                        snprintf(p, sizeof p, "types[%d].fields[%d]", i, j);
                        emit_err(c, "R150", p,
                                 "duplicate field name '%s'", fname);
                        return false;
                    }
                }
                tt->fields[j].name = arena_strdup(c->rt->arena, fname);
                char p[96];
                snprintf(p, sizeof p, "types[%d].fields[%d].type", i, j);
                tt->fields[j].type = resolve_type_ref(c, fty, p);
                if (tt->fields[j].type == TYPE_ID_NONE) return false;
            }
        } else /* TYPE_KIND_SUM */ {
            const cJSON *variants = json_obj_get(t, "variants");
            if (!json_is_array(variants)) {
                char p[64]; snprintf(p, sizeof p, "types[%d].variants", i);
                emit_err(c, "R150", p, "expected an array");
                return false;
            }
            int nv = json_array_size(variants);
            tt->variants   = arena_alloc_zero(c->rt->arena,
                                              (size_t)nv * sizeof *tt->variants);
            tt->n_variants = (size_t)nv;
            for (int j = 0; j < nv; j++) {
                cJSON *v = cJSON_GetArrayItem(variants, j);
                const char  *vname = json_string(json_obj_get(v, "name"));
                const cJSON *vfields = json_obj_get(v, "fields");
                if (!vname || !json_is_array(vfields)) {
                    char p[96];
                    snprintf(p, sizeof p, "types[%d].variants[%d]", i, j);
                    emit_err(c, "R150", p,
                             "missing 'name' or 'fields' array");
                    return false;
                }
                for (int k = 0; k < j; k++) {
                    if (strcmp(tt->variants[k].name, vname) == 0) {
                        char p[96];
                        snprintf(p, sizeof p, "types[%d].variants[%d]", i, j);
                        emit_err(c, "R150", p,
                                 "duplicate variant name '%s'", vname);
                        return false;
                    }
                }
                tt->variants[j].name = arena_strdup(c->rt->arena, vname);
                int nvf = json_array_size(vfields);
                tt->variants[j].fields = arena_alloc_zero(c->rt->arena,
                    (size_t)nvf * sizeof *tt->variants[j].fields);
                tt->variants[j].n_fields = (size_t)nvf;
                for (int k = 0; k < nvf; k++) {
                    cJSON *vf = cJSON_GetArrayItem(vfields, k);
                    const char  *fname = json_string(json_obj_get(vf, "name"));
                    const cJSON *fty   = json_obj_get(vf, "type");
                    if (!fname || !fty) {
                        char p[128];
                        snprintf(p, sizeof p,
                                 "types[%d].variants[%d].fields[%d]",
                                 i, j, k);
                        emit_err(c, "R150", p, "missing 'name' or 'type'");
                        return false;
                    }
                    for (int m = 0; m < k; m++) {
                        if (strcmp(tt->variants[j].fields[m].name, fname) == 0) {
                            char pp[128];
                            snprintf(pp, sizeof pp,
                                     "types[%d].variants[%d].fields[%d]",
                                     i, j, k);
                            emit_err(c, "R150", pp,
                                     "duplicate field name '%s'", fname);
                            return false;
                        }
                    }
                    tt->variants[j].fields[k].name =
                        arena_strdup(c->rt->arena, fname);
                    char p[128];
                    snprintf(p, sizeof p,
                             "types[%d].variants[%d].fields[%d].type",
                             i, j, k);
                    tt->variants[j].fields[k].type =
                        resolve_type_ref(c, fty, p);
                    if (tt->variants[j].fields[k].type == TYPE_ID_NONE)
                        return false;
                }
            }
        }
    }
    return true;
}

/* Pass 5: flatten every sum's variants into the by-name index. Must
 * follow pass 4 — it reads the n_variants/variant names pass 4 fills
 * in — and sorts once at the end so the canonical dumps can walk it in
 * a deterministic order. */
static void
pass_build_variant_index(load_ctx_t *c)
{
    for (size_t i = 0; i < c->rt->types->n_all; i++) {
        const type_t *t = c->rt->types->all[i];
        if (t->kind != TYPE_KIND_SUM) continue;
        for (size_t j = 0; j < t->n_variants; j++) {
            variant_index_add(c->rt->types, t->variants[j].name,
                              t->id, (uint32_t)j);
        }
    }
    variant_index_sort(c->rt->types);
}


/* ---- Tools ---- */

static bool
parse_effect_level(load_ctx_t *c, const cJSON *level_v, const char *json_path,
                   effect_level_t *out)
{
    const char *s = json_string(level_v);
    if (!s) {
        emit_err(c, "R150", json_path, "missing or non-string 'level'");
        return false;
    }
    if      (strcmp(s, "pure")          == 0) *out = EFFECT_PURE;
    else if (strcmp(s, "deterministic") == 0) *out = EFFECT_DETERMINISTIC;
    else if (strcmp(s, "model")         == 0) *out = EFFECT_MODEL;
    else if (strcmp(s, "mutation")      == 0) *out = EFFECT_MUTATION;
    else {
        emit_err(c, "R150", json_path, "unknown effect level: %s", s);
        return false;
    }
    return true;
}

static bool
parse_retry(load_ctx_t *c, const cJSON *retry_v, const char *json_path,
            retry_policy_t *out)
{
    out->kind = RETRY_DEFAULT;
    if (retry_v == NULL) return true;
    if (!json_is_object(retry_v)) {
        emit_err(c, "R150", json_path, "retry must be an object");
        return false;
    }
    const char *kind = json_string(json_obj_get(retry_v, "kind"));
    if (!kind) {
        emit_err(c, "R150", json_path, "retry.kind missing");
        return false;
    }
    if (strcmp(kind, "forever") == 0) {
        out->kind = RETRY_FOREVER;
    } else if (strcmp(kind, "count") == 0) {
        const cJSON *v = json_obj_get(retry_v, "value");
        /* Reject NaN and out-of-range magnitudes before the cast:
         * converting a double outside [0, UINT32_MAX] to uint32_t is
         * undefined behavior (C11 6.3.1.4p1) and would yield a
         * platform-dependent value, breaking byte-determinism. The
         * negated comparison also rejects NaN. */
        if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0.0)
                               || v->valuedouble > (double)UINT32_MAX) {
            emit_err(c, "R150", json_path,
                     "retry.value must be a number in [0, UINT32_MAX]");
            return false;
        }
        out->kind  = RETRY_COUNT;
        out->count = (uint32_t)v->valuedouble;
    } else if (strcmp(kind, "backoff") == 0) {
        const cJSON *ji = json_obj_get(retry_v, "initial");
        const cJSON *jm = json_obj_get(retry_v, "max");
        const cJSON *jf = json_obj_get(retry_v, "factor");
        if (!cJSON_IsNumber(ji) || !cJSON_IsNumber(jm)
                                        || !cJSON_IsNumber(jf)) {
            emit_err(c, "R150", json_path,
                     "retry.backoff requires numeric initial/max/factor");
            return false;
        }
        /* Bound each field to [0, UINT32_MAX] before the cast: a double
         * outside that range (or NaN) is undefined behavior to convert
         * to uint32_t (C11 6.3.1.4p1) and would yield a platform-
         * dependent value, breaking byte-determinism. The negated
         * lower-bound comparison also rejects NaN. */
        if (!(ji->valuedouble >= 0.0) || ji->valuedouble > (double)UINT32_MAX
            || !(jm->valuedouble >= 0.0) || jm->valuedouble > (double)UINT32_MAX
            || !(jf->valuedouble >= 0.0) || jf->valuedouble > (double)UINT32_MAX) {
            emit_err(c, "R150", json_path,
                     "retry.backoff initial/max/factor must each be a "
                     "number in [0, UINT32_MAX]");
            return false;
        }
        out->kind            = RETRY_BACKOFF;
        out->backoff_initial = (uint32_t)ji->valuedouble;
        out->backoff_max     = (uint32_t)jm->valuedouble;
        out->backoff_factor  = (uint32_t)jf->valuedouble;
    } else {
        emit_err(c, "R150", json_path,
                 "unknown retry kind: %s", kind);
        return false;
    }
    return true;
}

static bool
pass_tools(load_ctx_t *c, const cJSON *tools_arr)
{
    int n = json_array_size(tools_arr);
    c->rt->tools   = n > 0 ? arena_alloc_zero(c->rt->arena,
                                              (size_t)n * sizeof *c->rt->tools)
                           : NULL;
    c->rt->n_tools = (size_t)n;

    for (int i = 0; i < n; i++) {
        cJSON *t = cJSON_GetArrayItem(tools_arr, i);
        if (!json_is_object(t)) {
            char p[64]; snprintf(p, sizeof p, "tools[%d]", i);
            emit_err(c, "R150", p, "expected an object");
            return false;
        }
        tool_t *out = &c->rt->tools[i];

        const char  *name   = json_string(json_obj_get(t, "name"));
        const cJSON *input  = json_obj_get(t, "input");
        const cJSON *output = json_obj_get(t, "output");
        const cJSON *effect = json_obj_get(t, "effect");
        if (!name || !input || !output || !effect) {
            char p[64]; snprintf(p, sizeof p, "tools[%d]", i);
            emit_err(c, "R150", p,
                     "missing one of name/input/output/effect");
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(c->rt->tools[j].name, name) == 0) {
                char p[64]; snprintf(p, sizeof p, "tools[%d]", i);
                emit_err(c, "R150", p, "duplicate tool name '%s'", name);
                return false;
            }
        }
        out->name = arena_strdup(c->rt->arena, name);

        if (!json_is_array(input)) {
            char p[64]; snprintf(p, sizeof p, "tools[%d].input", i);
            emit_err(c, "R150", p, "expected an array");
            return false;
        }
        int np = json_array_size(input);
        out->params   = np > 0 ? arena_alloc_zero(c->rt->arena,
                                                  (size_t)np * sizeof *out->params)
                               : NULL;
        out->n_params = (size_t)np;
        for (int j = 0; j < np; j++) {
            cJSON *pj = cJSON_GetArrayItem(input, j);
            const char  *pname = json_string(json_obj_get(pj, "name"));
            const cJSON *pty   = json_obj_get(pj, "type");
            if (!pname || !pty) {
                char p[96]; snprintf(p, sizeof p, "tools[%d].input[%d]", i, j);
                emit_err(c, "R150", p, "missing 'name' or 'type'");
                return false;
            }
            out->params[j].name = arena_strdup(c->rt->arena, pname);
            char p[96];
            snprintf(p, sizeof p, "tools[%d].input[%d].type", i, j);
            out->params[j].type = resolve_type_ref(c, pty, p);
            if (out->params[j].type == TYPE_ID_NONE) return false;
        }

        {
            char p[64]; snprintf(p, sizeof p, "tools[%d].output", i);
            out->output = resolve_type_ref(c, output, p);
            if (out->output == TYPE_ID_NONE) return false;
        }

        {
            char p[64]; snprintf(p, sizeof p, "tools[%d].effect.level", i);
            if (!parse_effect_level(c, json_obj_get(effect, "level"), p,
                                    &out->level))
                return false;
        }
        if (out->level == EFFECT_MODEL) {
            const char *mid = json_string(json_obj_get(effect, "model"));
            if (!mid) {
                char p[64];
                snprintf(p, sizeof p, "tools[%d].effect.model", i);
                emit_err(c, "R150", p,
                         "effect 'model' requires a 'model' identifier");
                return false;
            }
            out->model_id = arena_strdup(c->rt->arena, mid);
        }

        {
            char p[64]; snprintf(p, sizeof p, "tools[%d].effect.retry", i);
            if (!parse_retry(c, json_obj_get(effect, "retry"), p, &out->retry))
                return false;
        }
    }
    return true;
}


/* ---- Flows ---- */

static bool
pass_flows(load_ctx_t *c, const cJSON *flows_arr)
{
    int n = json_array_size(flows_arr);
    c->rt->flows   = n > 0 ? arena_alloc_zero(c->rt->arena,
                                              (size_t)n * sizeof *c->rt->flows)
                           : NULL;
    c->rt->n_flows = (size_t)n;

    for (int i = 0; i < n; i++) {
        cJSON *f = cJSON_GetArrayItem(flows_arr, i);
        if (!json_is_object(f)) {
            char p[64]; snprintf(p, sizeof p, "flows[%d]", i);
            emit_err(c, "R150", p, "expected an object");
            return false;
        }
        flow_t *out = &c->rt->flows[i];

        const char  *name    = json_string(json_obj_get(f, "name"));
        const cJSON *params  = json_obj_get(f, "params");
        const cJSON *output  = json_obj_get(f, "output");
        const cJSON *binds   = json_obj_get(f, "bindings");
        const cJSON *ret     = json_obj_get(f, "return");

        if (!name || !params || !output || !binds || !ret) {
            char p[64]; snprintf(p, sizeof p, "flows[%d]", i);
            emit_err(c, "R150", p,
                     "missing one of name/params/output/bindings/return");
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(c->rt->flows[j].name, name) == 0) {
                char p[64]; snprintf(p, sizeof p, "flows[%d]", i);
                emit_err(c, "R150", p, "duplicate flow name '%s'", name);
                return false;
            }
        }
        out->name = arena_strdup(c->rt->arena, name);

        if (!json_is_array(params)) {
            char p[64]; snprintf(p, sizeof p, "flows[%d].params", i);
            emit_err(c, "R150", p, "expected an array");
            return false;
        }
        int np = json_array_size(params);
        out->params   = np > 0 ? arena_alloc_zero(c->rt->arena,
                                                  (size_t)np * sizeof *out->params)
                               : NULL;
        out->n_params = (size_t)np;
        for (int j = 0; j < np; j++) {
            cJSON *pj = cJSON_GetArrayItem(params, j);
            const char  *pname = json_string(json_obj_get(pj, "name"));
            const cJSON *pty   = json_obj_get(pj, "type");
            const cJSON *pimpl = json_obj_get(pj, "implicit");
            if (!pname || !pty || !cJSON_IsBool(pimpl)) {
                char p[96]; snprintf(p, sizeof p, "flows[%d].params[%d]", i, j);
                emit_err(c, "R150", p,
                         "missing name/type or non-boolean implicit");
                return false;
            }
            out->params[j].name     = arena_strdup(c->rt->arena, pname);
            out->params[j].implicit = cJSON_IsTrue(pimpl);
            char p[96];
            snprintf(p, sizeof p, "flows[%d].params[%d].type", i, j);
            out->params[j].type = resolve_type_ref(c, pty, p);
            if (out->params[j].type == TYPE_ID_NONE) return false;
        }

        {
            char p[64]; snprintf(p, sizeof p, "flows[%d].output", i);
            out->output = resolve_type_ref(c, output, p);
            if (out->output == TYPE_ID_NONE) return false;
        }

        if (!json_is_array(binds)) {
            char p[64]; snprintf(p, sizeof p, "flows[%d].bindings", i);
            emit_err(c, "R150", p, "expected an array");
            return false;
        }
        if (!json_is_object(ret)) {
            char p[64]; snprintf(p, sizeof p, "flows[%d].return", i);
            emit_err(c, "R150", p, "expected an object");
            return false;
        }

        out->bindings    = binds;
        out->return_expr = ret;
    }
    return true;
}


/* ---- Top-level orchestration ---- */

static flowd_runtime *
load_from_buffer_owned(char *json_buf, size_t len,
                       const char *name, DiagStream *diag)
{
    /* Canonical value serialization formats floats with %.17g (see
     * value.c emit_float), whose decimal separator follows LC_NUMERIC.
     * Pin it to "C" so traces are byte-identical regardless of the
     * host's locale. */
    setlocale(LC_NUMERIC, "C");

    /* Stable source name for diagnostics. rt->source_path is allocated
     * in the arena, but the error paths below destroy the arena before
     * the caller reads the recorded diagnostic (a diagnostic's loc.file
     * is borrowed, not copied), so diagnostics must reference a string
     * that outlives the arena. The caller's `name` — or the "<buffer>"
     * literal — does; rt->source_path does not. */
    const char *src_name = name ? name : "<buffer>";

    Arena *arena = arena_create(0);
    if (!arena) {
        diag_emit(diag, (SrcLoc){ src_name, 0, 0 }, DIAG_ERROR, "R150",
                  "arena allocation failed");
        free(json_buf);
        return NULL;
    }

    flowd_runtime *rt = arena_alloc_zero(arena, sizeof *rt);
    rt->arena       = arena;
    rt->source_path = arena_strdup(arena, src_name);
    rt->types       = type_registry_create(arena);
    rt->gateway     = gateway_create(arena);

    /* cJSON owns the parsed tree via system malloc. The error pointer
     * from cJSON_GetErrorPtr() points into json_buf, so we must capture
     * any diagnostic snippet before freeing the buffer (else use-after-
     * free on malformed/empty input). */
    rt->ir_root = cJSON_ParseWithLength(json_buf, len);
    if (!rt->ir_root) {
        const char *errptr = cJSON_GetErrorPtr();
        char near[41] = {0};
        if (errptr && errptr >= json_buf
            && (size_t)(errptr - json_buf) <= len) {
            size_t off   = (size_t)(errptr - json_buf);
            size_t avail = len - off;
            size_t take  = avail < 40u ? avail : 40u;
            memcpy(near, errptr, take);
            for (size_t i = 0; i < take; i++) {
                if ((unsigned char)near[i] < 0x20u) near[i] = ' ';
            }
        }
        free(json_buf);
        diag_emit(diag, (SrcLoc){ src_name, 0, 0 }, DIAG_ERROR, "R150",
                  "JSON parse failed near: %s",
                  near[0] ? near : "(start of input)");
        arena_destroy(arena);
        return NULL;
    }
    free(json_buf);
    if (!json_is_object(rt->ir_root)) {
        diag_emit(diag, (SrcLoc){ src_name, 0, 0 }, DIAG_ERROR, "R150",
                  "top-level JSON must be an object");
        cJSON_Delete(rt->ir_root);
        arena_destroy(arena);
        return NULL;
    }

    /* Top-level validation. */
    const cJSON *ver_v = json_obj_get(rt->ir_root, "ir_version");
    const char  *ver   = json_string(ver_v);
    if (!ver) {
        diag_emit(diag, (SrcLoc){ src_name, 0, 0 }, DIAG_ERROR, "R150",
                  "missing or non-string 'ir_version'");
        cJSON_Delete(rt->ir_root);
        arena_destroy(arena);
        return NULL;
    }
    if (strcmp(ver, "1.0") != 0) {
        diag_emit(diag, (SrcLoc){ src_name, 0, 0 }, DIAG_ERROR, "R150",
                  "unsupported IR version '%s' (expected '1.0')", ver);
        cJSON_Delete(rt->ir_root);
        arena_destroy(arena);
        return NULL;
    }

    const cJSON *types = json_obj_get(rt->ir_root, "types");
    const cJSON *tools = json_obj_get(rt->ir_root, "tools");
    const cJSON *flows = json_obj_get(rt->ir_root, "flows");
    if (!json_is_array(types) || !json_is_array(tools)
                              || !json_is_array(flows)) {
        diag_emit(diag, (SrcLoc){ src_name, 0, 0 }, DIAG_ERROR, "R150",
                  "top-level 'types', 'tools', and 'flows' must all be arrays");
        cJSON_Delete(rt->ir_root);
        arena_destroy(arena);
        return NULL;
    }

    load_ctx_t c;
    c.rt   = rt;
    c.diag = diag;
    c.loc  = (SrcLoc){ src_name, 0, 0 };

    if (!pass_types_register(&c, types) ||
        !pass_types_resolve (&c, types)) {
        cJSON_Delete(rt->ir_root);
        arena_destroy(arena);
        return NULL;
    }
    pass_build_variant_index(&c);

    if (!pass_tools(&c, tools) ||
        !pass_flows(&c, flows)) {
        cJSON_Delete(rt->ir_root);
        arena_destroy(arena);
        return NULL;
    }
    return rt;
}

flowd_runtime *
flowd_load_ir_buffer(const char *src, size_t len, const char *name,
                     DiagStream *diag)
{
    if (!src) {
        diag_emit(diag, SRCLOC_NONE, DIAG_ERROR, "R150",
                  "flowd_load_ir_buffer: null source");
        return NULL;
    }
    char *buf = malloc(len + 1u);
    if (!buf) {
        FLOWD_ICE("flowd_load_ir_buffer: out of memory");
    }
    memcpy(buf, src, len);
    buf[len] = '\0';
    return load_from_buffer_owned(buf, len, name, diag);
}

flowd_runtime *
flowd_load_ir_file(const char *path, DiagStream *diag)
{
    size_t len = 0;
    char *buf = slurp_file(path, &len, diag);
    if (!buf) return NULL;
    return load_from_buffer_owned(buf, len, path, diag);
}


/* ====================================================================
 * Destruction
 *
 * flowd_destroy (declared in flowd.h) calls this. We free the cJSON
 * tree (system malloc), then the arena (which sweeps everything we
 * built).
 * ==================================================================== */

void flowd_runtime_destroy(flowd_runtime *rt);  /* fwd, called from flowd.c */

void
flowd_runtime_destroy(flowd_runtime *rt)
{
    if (!rt) return;
    if (rt->ir_root) cJSON_Delete(rt->ir_root);
    if (rt->gateway) gateway_destroy(rt->gateway);  /* free heap'd cache responses before the arena goes */
    Arena *a = rt->arena;
    arena_destroy(a);
}


/* ====================================================================
 * Canonical-dump-lite
 * ==================================================================== */

static const char *
effect_word(effect_level_t lvl)
{
    switch (lvl) {
        case EFFECT_PURE:          return "pure";
        case EFFECT_DETERMINISTIC: return "deterministic";
        case EFFECT_MODEL:         return "model";
        case EFFECT_MUTATION:      return "mutation";
    }
    return "?";
}

static int cmp_tool_by_name(const void *a, const void *b)
{
    const tool_t *const *pa = a;
    const tool_t *const *pb = b;
    return strcmp((*pa)->name, (*pb)->name);
}
static int cmp_flow_by_name(const void *a, const void *b)
{
    const flow_t *const *pa = a;
    const flow_t *const *pb = b;
    return strcmp((*pa)->name, (*pb)->name);
}
static int cmp_field_by_name(const void *a, const void *b)
{
    const type_field_t *fa = a;
    const type_field_t *fb = b;
    return strcmp(fa->name, fb->name);
}
static int cmp_tool_param_by_name(const void *a, const void *b)
{
    const tool_param_t *pa = a;
    const tool_param_t *pb = b;
    return strcmp(pa->name, pb->name);
}
static int cmp_flow_param_by_name(const void *a, const void *b)
{
    const flow_param_t *pa = a;
    const flow_param_t *pb = b;
    return strcmp(pa->name, pb->name);
}

static const char *
type_name_of(const type_registry_t *r, type_id_t id)
{
    const type_t *t = type_registry_get(r, id);
    return t ? t->name : "?";
}

static void
dump_type(FILE *out, const type_registry_t *r, const type_t *t)
{
    switch (t->kind) {
        case TYPE_KIND_PRIMITIVE:
            fprintf(out, "  [%" PRIu32 "] %s (primitive)\n", t->id, t->name);
            return;
        case TYPE_KIND_LIST:
            fprintf(out, "  [%" PRIu32 "] %s (list of %s)\n",
                    t->id, t->name, type_name_of(r, t->elem));
            return;
        case TYPE_KIND_RECORD: {
            fprintf(out, "  [%" PRIu32 "] %s (record)\n", t->id, t->name);
            if (t->n_fields == 0) return;
            /* Lex-sort fields for canonical output. */
            type_field_t *sorted = malloc(t->n_fields * sizeof *sorted);
            memcpy(sorted, t->fields, t->n_fields * sizeof *sorted);
            qsort(sorted, t->n_fields, sizeof *sorted, cmp_field_by_name);
            for (size_t i = 0; i < t->n_fields; i++) {
                fprintf(out, "    %s: %s\n",
                        sorted[i].name,
                        type_name_of(r, sorted[i].type));
            }
            free(sorted);
            return;
        }
        case TYPE_KIND_SUM: {
            fprintf(out, "  [%" PRIu32 "] %s (sum)\n", t->id, t->name);
            /* Variants in declaration order so the variant_idx is
             * visible. Payload fields lex-sorted per-variant. */
            for (size_t i = 0; i < t->n_variants; i++) {
                const type_variant_t *v = &t->variants[i];
                fprintf(out, "    #%zu %s", i, v->name);
                if (v->n_fields == 0) {
                    fputc('\n', out);
                    continue;
                }
                type_field_t *sorted = malloc(v->n_fields * sizeof *sorted);
                memcpy(sorted, v->fields, v->n_fields * sizeof *sorted);
                qsort(sorted, v->n_fields, sizeof *sorted, cmp_field_by_name);
                fputc(' ', out); fputc('{', out);
                for (size_t k = 0; k < v->n_fields; k++) {
                    if (k > 0) fputs(", ", out);
                    fprintf(out, "%s: %s",
                            sorted[k].name,
                            type_name_of(r, sorted[k].type));
                }
                fputs("}\n", out);
                free(sorted);
            }
            return;
        }
    }
}

void
flowd_canonical_dump_lite(const flowd_runtime *rt, FILE *out)
{
    if (!rt) {
        fputs("ir_version: (not loaded)\n", out);
        return;
    }
    fputs("ir_version: 1.0\n", out);

    /* Types in registration order (primitives 1..4, then named in
     * source order, then interned lists in first-seen order). The
     * registry's iteration order is already canonical for a given
     * IR — flowc emits types in a stable declaration order. */
    fputs("types:\n", out);
    for (size_t i = 0; i < type_registry_count(rt->types); i++) {
        dump_type(out, rt->types, type_registry_at(rt->types, i));
    }

    /* Tools by name, lex-sorted for diff stability. */
    fputs("tools:\n", out);
    if (rt->n_tools > 0) {
        const tool_t **sorted = malloc(rt->n_tools * sizeof *sorted);
        for (size_t i = 0; i < rt->n_tools; i++) sorted[i] = &rt->tools[i];
        qsort(sorted, rt->n_tools, sizeof *sorted, cmp_tool_by_name);
        for (size_t i = 0; i < rt->n_tools; i++) {
            const tool_t *t = sorted[i];
            fprintf(out, "  %s\n", t->name);
            fprintf(out, "    effect: %s", effect_word(t->level));
            if (t->level == EFFECT_MODEL && t->model_id) {
                fprintf(out, " (model=%s)", t->model_id);
            }
            fputc('\n', out);
            switch (t->retry.kind) {
                case RETRY_DEFAULT: break;
                case RETRY_FOREVER:
                    fputs("    retry: forever\n", out); break;
                case RETRY_COUNT:
                    fprintf(out, "    retry: count=%" PRIu32 "\n",
                            t->retry.count); break;
                case RETRY_BACKOFF:
                    fprintf(out,
                            "    retry: backoff initial=%" PRIu32
                            " max=%" PRIu32 " factor=%" PRIu32 "\n",
                            t->retry.backoff_initial,
                            t->retry.backoff_max,
                            t->retry.backoff_factor);
                    break;
            }
            /* Sort params by name so the dump is diff-stable; the IR's
             * declaration order carries no meaning for tool inputs. */
            if (t->n_params > 0) {
                tool_param_t *sp = malloc(t->n_params * sizeof *sp);
                memcpy(sp, t->params, t->n_params * sizeof *sp);
                qsort(sp, t->n_params, sizeof *sp, cmp_tool_param_by_name);
                fputs("    input:", out);
                for (size_t p = 0; p < t->n_params; p++) {
                    fprintf(out, " %s:%s",
                            sp[p].name,
                            type_name_of(rt->types, sp[p].type));
                }
                fputc('\n', out);
                free(sp);
            } else {
                fputs("    input: (none)\n", out);
            }
            fprintf(out, "    output: %s\n",
                    type_name_of(rt->types, t->output));
        }
        free(sorted);
    }

    /* Flows by name, lex-sorted for diff stability. */
    fputs("flows:\n", out);
    if (rt->n_flows > 0) {
        const flow_t **sorted = malloc(rt->n_flows * sizeof *sorted);
        for (size_t i = 0; i < rt->n_flows; i++) sorted[i] = &rt->flows[i];
        qsort(sorted, rt->n_flows, sizeof *sorted, cmp_flow_by_name);
        for (size_t i = 0; i < rt->n_flows; i++) {
            const flow_t *f = sorted[i];
            fprintf(out, "  %s\n", f->name);
            if (f->n_params > 0) {
                /* Lex-sort params for canonical output. */
                flow_param_t *sp = malloc(f->n_params * sizeof *sp);
                memcpy(sp, f->params, f->n_params * sizeof *sp);
                qsort(sp, f->n_params, sizeof *sp, cmp_flow_param_by_name);
                fputs("    params:", out);
                for (size_t p = 0; p < f->n_params; p++) {
                    fprintf(out, " %s:%s%s",
                            sp[p].name,
                            type_name_of(rt->types, sp[p].type),
                            sp[p].implicit ? " (implicit)" : "");
                }
                fputc('\n', out);
                free(sp);
            } else {
                fputs("    params: (none)\n", out);
            }
            fprintf(out, "    output: %s\n",
                    type_name_of(rt->types, f->output));
        }
        free(sorted);
    }

    /* Variant index — already lex-sorted by name in the registry. */
    fputs("variant_index:\n", out);
    size_t nv = type_registry_variant_index_count(rt->types);
    for (size_t i = 0; i < nv; i++) {
        type_id_t sum;
        uint32_t  idx;
        const char *name = type_registry_variant_index_at(rt->types, i,
                                                          &sum, &idx);
        fprintf(out, "  %s -> %s#%" PRIu32 "\n",
                name, type_name_of(rt->types, sum), idx);
    }
}


/* ====================================================================
 * Canonical-dump JSON
 *
 * Pretty-printed JSON, 2-space indent, byte-deterministic. Keys
 * within every object appear in lex (UTF-8 byte) order, hand-rolled
 * by the emit_* functions so the order is explicit and grep-able.
 *
 * Arrays are sorted on emit:
 *   - types       by id (ascending)
 *   - tools       by name (lex)
 *   - flows       by name (lex)
 *   - record/variant payload fields by name (lex)
 *   - tool input params and flow params by name (lex)
 *   - variant_index by (name, sum_type, variant_idx) — already
 *     sorted by the registry; we just walk it.
 *
 * Variants within a sum type stay in declaration order: the JSON
 * encodes each variant's "index" alongside its name, so reordering
 * would break the (sum_type, variant_idx) reference downstream.
 *
 * Strings emit per RFC 8259 with the same minimum escape set the
 * value-model serializer uses. Numbers (only used for type
 * ids and retry parameters) emit without locale dependence.
 * ==================================================================== */

static void
ind(FILE *out, int depth)
{
    for (int i = 0; i < depth; i++) {
        fputs("  ", out);
    }
}

/* Sort helper: copy a type_field_t array and lex-sort by name. The
 * caller frees the copy. */
static type_field_t *
sorted_fields(const type_field_t *src, size_t n)
{
    if (n == 0) return NULL;
    type_field_t *out = malloc(n * sizeof *out);
    memcpy(out, src, n * sizeof *out);
    qsort(out, n, sizeof *out, cmp_field_by_name);
    return out;
}

/* Emit a {"name":..., "type":...} object — used for record fields,
 * variant payload fields, and tool input params. */
static void
emit_named_typed(FILE *out, int depth, const char *name, const char *typ)
{
    ind(out, depth); fputs("{\n", out);
    ind(out, depth + 1); fputs("\"name\": ", out); value_emit_json_string(out, name);
    fputs(",\n", out);
    ind(out, depth + 1); fputs("\"type\": ", out); value_emit_json_string(out, typ);
    fputc('\n', out);
    ind(out, depth); fputc('}', out);
}

/* Comma + newline between array elements, suppressed before first. */
static void
sep(FILE *out, bool *first)
{
    if (*first) { *first = false; fputc('\n', out); }
    else        { fputs(",\n", out); }
}

static void
emit_type_primitive(FILE *out, int depth, const type_t *t)
{
    ind(out, depth); fputs("{\n", out);
    ind(out, depth + 1); fprintf(out, "\"id\": %" PRIu32 ",\n", t->id);
    ind(out, depth + 1); fputs("\"kind\": \"primitive\",\n", out);
    ind(out, depth + 1); fputs("\"name\": ", out); value_emit_json_string(out, t->name);
    fputc('\n', out);
    ind(out, depth); fputc('}', out);
}

static void
emit_type_list(FILE *out, int depth, const type_registry_t *r, const type_t *t)
{
    ind(out, depth); fputs("{\n", out);
    ind(out, depth + 1); fputs("\"elem\": ", out);
    value_emit_json_string(out, type_name_of(r, t->elem)); fputs(",\n", out);
    ind(out, depth + 1); fprintf(out, "\"id\": %" PRIu32 ",\n", t->id);
    ind(out, depth + 1); fputs("\"kind\": \"list\",\n", out);
    ind(out, depth + 1); fputs("\"name\": ", out); value_emit_json_string(out, t->name);
    fputc('\n', out);
    ind(out, depth); fputc('}', out);
}

static void
emit_type_record(FILE *out, int depth, const type_registry_t *r,
                 const type_t *t)
{
    ind(out, depth); fputs("{\n", out);

    /* "fields" first (lex). */
    type_field_t *sf = sorted_fields(t->fields, t->n_fields);
    ind(out, depth + 1); fputs("\"fields\": [", out);
    {
        bool first = true;
        for (size_t i = 0; i < t->n_fields; i++) {
            sep(out, &first);
            emit_named_typed(out, depth + 2, sf[i].name,
                             type_name_of(r, sf[i].type));
        }
        if (t->n_fields > 0) { fputc('\n', out); ind(out, depth + 1); }
    }
    fputs("],\n", out);
    free(sf);

    ind(out, depth + 1); fprintf(out, "\"id\": %" PRIu32 ",\n", t->id);
    ind(out, depth + 1); fputs("\"kind\": \"record\",\n", out);
    ind(out, depth + 1); fputs("\"name\": ", out); value_emit_json_string(out, t->name);
    fputc('\n', out);
    ind(out, depth); fputc('}', out);
}

static void
emit_variant(FILE *out, int depth, const type_registry_t *r,
             const type_variant_t *v, size_t idx)
{
    ind(out, depth); fputs("{\n", out);

    type_field_t *sf = sorted_fields(v->fields, v->n_fields);
    ind(out, depth + 1); fputs("\"fields\": [", out);
    {
        bool first = true;
        for (size_t i = 0; i < v->n_fields; i++) {
            sep(out, &first);
            emit_named_typed(out, depth + 2, sf[i].name,
                             type_name_of(r, sf[i].type));
        }
        if (v->n_fields > 0) { fputc('\n', out); ind(out, depth + 1); }
    }
    fputs("],\n", out);
    free(sf);

    ind(out, depth + 1); fprintf(out, "\"index\": %zu,\n", idx);
    ind(out, depth + 1); fputs("\"name\": ", out); value_emit_json_string(out, v->name);
    fputc('\n', out);
    ind(out, depth); fputc('}', out);
}

static void
emit_type_sum(FILE *out, int depth, const type_registry_t *r, const type_t *t)
{
    ind(out, depth); fputs("{\n", out);
    ind(out, depth + 1); fprintf(out, "\"id\": %" PRIu32 ",\n", t->id);
    ind(out, depth + 1); fputs("\"kind\": \"sum\",\n", out);
    ind(out, depth + 1); fputs("\"name\": ", out); value_emit_json_string(out, t->name);
    fputs(",\n", out);

    /* Variants in declaration order (preserves variant_idx). */
    ind(out, depth + 1); fputs("\"variants\": [", out);
    {
        bool first = true;
        for (size_t i = 0; i < t->n_variants; i++) {
            sep(out, &first);
            emit_variant(out, depth + 2, r, &t->variants[i], i);
        }
        if (t->n_variants > 0) { fputc('\n', out); ind(out, depth + 1); }
    }
    fputs("]\n", out);
    ind(out, depth); fputc('}', out);
}

static int
cmp_type_by_id(const void *a, const void *b)
{
    const type_t *const *pa = a;
    const type_t *const *pb = b;
    if ((*pa)->id < (*pb)->id) return -1;
    if ((*pa)->id > (*pb)->id) return 1;
    return 0;
}

static void
emit_types(FILE *out, int depth, const type_registry_t *r)
{
    size_t n = type_registry_count(r);
    if (n == 0) {
        fputs("[]", out);
        return;
    }
    const type_t **sorted = malloc(n * sizeof *sorted);
    for (size_t i = 0; i < n; i++) sorted[i] = type_registry_at(r, i);
    qsort(sorted, n, sizeof *sorted, cmp_type_by_id);

    fputc('[', out);
    {
        bool first = true;
        for (size_t i = 0; i < n; i++) {
            sep(out, &first);
            switch (sorted[i]->kind) {
                case TYPE_KIND_PRIMITIVE:
                    emit_type_primitive(out, depth + 1, sorted[i]);   break;
                case TYPE_KIND_LIST:
                    emit_type_list     (out, depth + 1, r, sorted[i]); break;
                case TYPE_KIND_RECORD:
                    emit_type_record   (out, depth + 1, r, sorted[i]); break;
                case TYPE_KIND_SUM:
                    emit_type_sum      (out, depth + 1, r, sorted[i]); break;
            }
        }
        fputc('\n', out); ind(out, depth);
    }
    fputc(']', out);
    free(sorted);
}

static void
emit_retry(FILE *out, int depth, const retry_policy_t *r)
{
    /* Compact-but-readable inline form. Keys lex-sorted. */
    switch (r->kind) {
        case RETRY_DEFAULT:
            /* Should never reach this path: caller suppresses the key. */
            fputs("null", out);
            return;
        case RETRY_FOREVER:
            fputs("{\n", out);
            ind(out, depth + 1); fputs("\"kind\": \"forever\"\n", out);
            ind(out, depth); fputc('}', out);
            return;
        case RETRY_COUNT:
            fputs("{\n", out);
            ind(out, depth + 1); fputs("\"kind\": \"count\",\n", out);
            ind(out, depth + 1);
            fprintf(out, "\"value\": %" PRIu32 "\n", r->count);
            ind(out, depth); fputc('}', out);
            return;
        case RETRY_BACKOFF:
            fputs("{\n", out);
            ind(out, depth + 1);
            fprintf(out, "\"factor\": %" PRIu32 ",\n", r->backoff_factor);
            ind(out, depth + 1);
            fprintf(out, "\"initial\": %" PRIu32 ",\n", r->backoff_initial);
            ind(out, depth + 1); fputs("\"kind\": \"backoff\",\n", out);
            ind(out, depth + 1);
            fprintf(out, "\"max\": %" PRIu32 "\n", r->backoff_max);
            ind(out, depth); fputc('}', out);
            return;
    }
}

static void
emit_effect(FILE *out, int depth, const tool_t *t)
{
    /* Object: {"level":..., "model"?:..., "retry"?:...} — keys lex. */
    fputs("{\n", out);
    ind(out, depth + 1); fputs("\"level\": ", out);
    value_emit_json_string(out, effect_word(t->level));
    bool need_model = (t->level == EFFECT_MODEL && t->model_id != NULL);
    bool need_retry = (t->retry.kind != RETRY_DEFAULT);
    if (need_model) {
        fputs(",\n", out);
        ind(out, depth + 1); fputs("\"model\": ", out);
        value_emit_json_string(out, t->model_id);
    }
    if (need_retry) {
        fputs(",\n", out);
        ind(out, depth + 1); fputs("\"retry\": ", out);
        emit_retry(out, depth + 1, &t->retry);
    }
    fputc('\n', out);
    ind(out, depth); fputc('}', out);
}

static void
emit_tool(FILE *out, int depth, const type_registry_t *r, const tool_t *t)
{
    ind(out, depth); fputs("{\n", out);
    /* Keys: effect, input, name, output. */
    ind(out, depth + 1); fputs("\"effect\": ", out);
    emit_effect(out, depth + 1, t);
    fputs(",\n", out);

    /* input — sorted by name. */
    ind(out, depth + 1); fputs("\"input\": [", out);
    if (t->n_params > 0) {
        tool_param_t *sp = malloc(t->n_params * sizeof *sp);
        memcpy(sp, t->params, t->n_params * sizeof *sp);
        qsort(sp, t->n_params, sizeof *sp, cmp_tool_param_by_name);
        bool first = true;
        for (size_t i = 0; i < t->n_params; i++) {
            sep(out, &first);
            emit_named_typed(out, depth + 2, sp[i].name,
                             type_name_of(r, sp[i].type));
        }
        fputc('\n', out); ind(out, depth + 1);
        free(sp);
    }
    fputs("],\n", out);

    ind(out, depth + 1); fputs("\"name\": ", out);
    value_emit_json_string(out, t->name); fputs(",\n", out);

    ind(out, depth + 1); fputs("\"output\": ", out);
    value_emit_json_string(out, type_name_of(r, t->output)); fputc('\n', out);

    ind(out, depth); fputc('}', out);
}

static void
emit_tools(FILE *out, int depth, const flowd_runtime *rt)
{
    if (rt->n_tools == 0) {
        fputs("[]", out);
        return;
    }
    const tool_t **sorted = malloc(rt->n_tools * sizeof *sorted);
    for (size_t i = 0; i < rt->n_tools; i++) sorted[i] = &rt->tools[i];
    qsort(sorted, rt->n_tools, sizeof *sorted, cmp_tool_by_name);

    fputc('[', out);
    {
        bool first = true;
        for (size_t i = 0; i < rt->n_tools; i++) {
            sep(out, &first);
            emit_tool(out, depth + 1, rt->types, sorted[i]);
        }
        fputc('\n', out); ind(out, depth);
    }
    fputc(']', out);
    free(sorted);
}

static void
emit_flow_param(FILE *out, int depth, const type_registry_t *r,
                const flow_param_t *p)
{
    ind(out, depth); fputs("{\n", out);
    ind(out, depth + 1);
    fprintf(out, "\"implicit\": %s,\n", p->implicit ? "true" : "false");
    ind(out, depth + 1); fputs("\"name\": ", out);
    value_emit_json_string(out, p->name); fputs(",\n", out);
    ind(out, depth + 1); fputs("\"type\": ", out);
    value_emit_json_string(out, type_name_of(r, p->type)); fputc('\n', out);
    ind(out, depth); fputc('}', out);
}

static void
emit_flow(FILE *out, int depth, const type_registry_t *r, const flow_t *f)
{
    ind(out, depth); fputs("{\n", out);

    ind(out, depth + 1); fputs("\"name\": ", out);
    value_emit_json_string(out, f->name); fputs(",\n", out);

    ind(out, depth + 1); fputs("\"output\": ", out);
    value_emit_json_string(out, type_name_of(r, f->output)); fputs(",\n", out);

    ind(out, depth + 1); fputs("\"params\": [", out);
    if (f->n_params > 0) {
        flow_param_t *sp = malloc(f->n_params * sizeof *sp);
        memcpy(sp, f->params, f->n_params * sizeof *sp);
        qsort(sp, f->n_params, sizeof *sp, cmp_flow_param_by_name);
        bool first = true;
        for (size_t i = 0; i < f->n_params; i++) {
            sep(out, &first);
            emit_flow_param(out, depth + 2, r, &sp[i]);
        }
        fputc('\n', out); ind(out, depth + 1);
        free(sp);
    }
    fputs("]\n", out);
    ind(out, depth); fputc('}', out);
}

static void
emit_flows(FILE *out, int depth, const flowd_runtime *rt)
{
    if (rt->n_flows == 0) {
        fputs("[]", out);
        return;
    }
    const flow_t **sorted = malloc(rt->n_flows * sizeof *sorted);
    for (size_t i = 0; i < rt->n_flows; i++) sorted[i] = &rt->flows[i];
    qsort(sorted, rt->n_flows, sizeof *sorted, cmp_flow_by_name);

    fputc('[', out);
    {
        bool first = true;
        for (size_t i = 0; i < rt->n_flows; i++) {
            sep(out, &first);
            emit_flow(out, depth + 1, rt->types, sorted[i]);
        }
        fputc('\n', out); ind(out, depth);
    }
    fputc(']', out);
    free(sorted);
}

static void
emit_variant_index(FILE *out, int depth, const flowd_runtime *rt)
{
    size_t n = type_registry_variant_index_count(rt->types);
    if (n == 0) {
        fputs("[]", out);
        return;
    }
    fputc('[', out);
    {
        bool first = true;
        for (size_t i = 0; i < n; i++) {
            type_id_t sum;
            uint32_t  idx;
            const char *name = type_registry_variant_index_at(
                rt->types, i, &sum, &idx);
            sep(out, &first);
            ind(out, depth + 1); fputs("{\n", out);
            ind(out, depth + 2); fputs("\"name\": ", out);
            value_emit_json_string(out, name); fputs(",\n", out);
            ind(out, depth + 2); fputs("\"sum\": ", out);
            value_emit_json_string(out, type_name_of(rt->types, sum));
            fputs(",\n", out);
            ind(out, depth + 2);
            fprintf(out, "\"variant_idx\": %" PRIu32 "\n", idx);
            ind(out, depth + 1); fputc('}', out);
        }
        fputc('\n', out); ind(out, depth);
    }
    fputc(']', out);
}

void
flowd_canonical_dump_json(const flowd_runtime *rt, FILE *out)
{
    if (!rt) {
        fputs("null\n", out);
        return;
    }
    /* Top-level keys lex-sorted: flows, ir_version, tools, types,
     * variant_index. */
    fputs("{\n", out);

    ind(out, 1); fputs("\"flows\": ", out);
    emit_flows(out, 1, rt); fputs(",\n", out);

    ind(out, 1); fputs("\"ir_version\": \"1.0\",\n", out);

    ind(out, 1); fputs("\"tools\": ", out);
    emit_tools(out, 1, rt); fputs(",\n", out);

    ind(out, 1); fputs("\"types\": ", out);
    emit_types(out, 1, rt->types); fputs(",\n", out);

    ind(out, 1); fputs("\"variant_index\": ", out);
    emit_variant_index(out, 1, rt); fputc('\n', out);

    fputs("}\n", out);
}
