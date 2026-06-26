/* src/exec.c
 *
 * Flow executor.
 *
 * What lands here:
 *   - env_t: lexically scoped name → value_t* map.
 *   - cJSON ↔ value_t conversion against the type registry.
 *   - Expression evaluator covering every kind:
 *     literal, path, list_literal, construct, construct_variant,
 *     binop, unop, conditional, try_else, call, subflow_call, match,
 *     pipeline.
 *   - Pipeline driver: where (predicate string), select, dedupe by,
 *     concat, take, rank (sort), terminals (top, pick, count, sum,
 *     max, min, any, all).
 *   - Tiny predicate parser for `where`: flowc emits the predicate
 *     as a string instead of an expression tree, so we parse it here
 *     against the row's record fields.
 *   - flowd_run_impl: parse input, bind params, walk bindings, eval
 *     return, canonicalize the resulting value to JSON.
 *
 * Adding a new expression kind: write a helper, add a case to the
 * switch in eval_expr. Adding a pipeline stage: write a stage_*
 * function, add a case to run_stage. Everything else stays.
 */

#include "exec.h"
#include "gateway.h"
#include "runtime_internal.h"
#include "trace.h"

/* Replay context for a single flow execution. NULL inside ctx
 * means "normal execution"; non-NULL puts the executor into replay
 * mode and rewrites model_call dispatch. */
typedef struct {
    trace_reader_t *original;      /* the trace to restore from */
    const char     *original_dir;  /* arena-owned path, for cross-refs */
    const char     *new_model_id;  /* NULL = same-model; non-NULL =
                                    * model-versioned (swap model id) */
} exec_replay_t;

/* When the executor encounters the built-in await_human_approval
 * tool, it populates this struct and returns EXEC_SUSPENDED. The
 * outer run_or_replay walks the suspended trace_writer, flushes a
 * suspensions/<token>.json file, and seals the manifest. */
typedef struct {
    char *token;             /* heap-owned; freed by caller */
    char *node_id;           /* arena-owned; the suspended node */
    char *condition;         /* arena-owned; "human_approval" in v1 */
    char *args_json;         /* arena-owned canonical JSON of the
                              * suspension call's args (e.g. prompt) */
} exec_suspension_t;

/* Resume context: opposite of suspension. The host has provided a
 * decision; we re-execute the flow from the start and at the
 * suspended node we inject the decision as that node's output
 * instead of suspending again. */
typedef struct {
    trace_reader_t *original;       /* the suspended trace */
    const char     *original_dir;
    const char     *suspended_node; /* node_id to inject at */
    const char     *decision_json;  /* host's decision, raw JSON */
} exec_resume_t;

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cjson/cJSON.h"


/* ====================================================================
 * Environment
 *
 * Frame layout: a flat append-only array of bindings. New scopes
 * (match arms, pipeline row bindings, sub-flow invocations) push a
 * fresh frame; the parent pointer chains them. Lookup walks the
 * chain from `top` outward, returning the first match — newest-wins
 * shadowing.
 * ==================================================================== */

#define ENV_FRAME_INITIAL_CAP 8u

typedef struct {
    const char *name;
    value_t    *value;
} env_binding_t;

struct env_frame {
    env_frame_t   *parent;
    env_binding_t *bindings;
    size_t         n;
    size_t         cap;
};

void
env_init(env_t *e, Arena *a)
{
    e->arena = a;
    e->top   = NULL;
}

env_frame_t *
env_push(env_t *e)
{
    env_frame_t *prior = e->top;
    env_frame_t *f = arena_alloc_zero(e->arena, sizeof *f);
    f->parent   = prior;
    f->bindings = arena_alloc_zero(e->arena,
                                   ENV_FRAME_INITIAL_CAP * sizeof *f->bindings);
    f->n        = 0;
    f->cap      = ENV_FRAME_INITIAL_CAP;
    e->top      = f;
    return prior;
}

void
env_pop(env_t *e, env_frame_t *prior_top)
{
    e->top = prior_top;
}

void
env_bind(env_t *e, const char *name, value_t *v)
{
    env_frame_t *f = e->top;
    if (f == NULL) {
        FLOWD_ICE("env_bind: no active frame");
    }
    if (f->n == f->cap) {
        size_t ncap = f->cap * 2u;
        env_binding_t *grown = arena_alloc(e->arena,
                                           ncap * sizeof *grown);
        memcpy(grown, f->bindings, f->n * sizeof *grown);
        f->bindings = grown;
        f->cap      = ncap;
    }
    f->bindings[f->n].name  = arena_strdup(e->arena, name);
    f->bindings[f->n].value = v;
    f->n++;
}

const value_t *
env_lookup(const env_t *e, const char *name)
{
    for (env_frame_t *f = e->top; f != NULL; f = f->parent) {
        for (size_t i = f->n; i > 0; i--) {
            if (strcmp(f->bindings[i - 1u].name, name) == 0) {
                return f->bindings[i - 1u].value;
            }
        }
    }
    return NULL;
}


/* ====================================================================
 * Execution context
 *
 * Threading every helper with (rt, arena, diag) gets noisy, so we
 * bundle them. ctx is immutable across a flow run; the env is the
 * mutable state that gets pushed/popped as the executor descends.
 * ==================================================================== */

typedef struct {
    flowd_runtime     *rt;
    Arena             *arena;
    DiagStream        *diag;
    trace_writer_t    *trace;       /* NULL = no tracing this run */
    exec_replay_t     *replay;      /* NULL = normal exec */
    exec_resume_t     *resume;      /* NULL = no resumption */
    /* OUT-parameter: when the executor returns EXEC_SUSPENDED, this
     * slot carries the suspension state. NULL pointer in slot before
     * eval; populated by the await_human_approval recognizer. */
    exec_suspension_t *suspension_out;

    /* Current subflow nesting depth, capped to bound the C stack on
     * (possibly mutually) recursive sub-flow composition. */
    int subflow_depth;
} exec_ctx_t;

#define SUBFLOW_DEPTH_MAX 256


/* ====================================================================
 * Diagnostic helpers + result constructors
 * ==================================================================== */

/* R-codes for runtime failures. Callers pick the right code so
 * try/else can discriminate recoverable from unrecoverable failures
 * (only R101, R102, R130, R161 fall through). Programming-bug-shape
 * errors with no dedicated code use R155 as the catch-all. */
static void
exec_err(DiagStream *diag, const char *code, const char *json_path,
         const char *fmt, ...) FLOWD_PRINTF(4, 5);

static void
exec_err(DiagStream *diag, const char *code, const char *json_path,
         const char *fmt, ...)
{
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    if (json_path && json_path[0]) {
        diag_emit(diag, SRCLOC_NONE, DIAG_ERROR, code,
                  "%s: %s", json_path, body);
    } else {
        diag_emit(diag, SRCLOC_NONE, DIAG_ERROR, code, "%s", body);
    }
}

static exec_result_t mk_ok (value_t *v) { exec_result_t r = { EXEC_OK,        v    }; return r; }
static exec_result_t mk_err(void)       { exec_result_t r = { EXEC_ERROR,     NULL }; return r; }
static exec_result_t mk_cancel(void)    { exec_result_t r = { EXEC_CANCELLED, NULL }; return r; }


/* ====================================================================
 * JSON → value_t
 * ==================================================================== */

static int64_t
json_number_to_int(const cJSON *n, bool *ok)
{
    double d = n->valuedouble;
    if (d != (double)(int64_t)d) { *ok = false; return 0; }
    *ok = true;
    return (int64_t)d;
}

value_t *
value_from_json(Arena                 *arena,
                const type_registry_t *types,
                type_id_t              expected,
                const cJSON           *json,
                DiagStream            *diag,
                const char            *json_path)
{
    if (json == NULL) {
        exec_err(diag, "R155", json_path, "missing value");
        return NULL;
    }
    const type_t *t = type_registry_get(types, expected);
    if (t == NULL) {
        exec_err(diag, "R155", json_path, "internal: unknown type id %u",
                 (unsigned)expected);
        return NULL;
    }

    switch (t->kind) {
        case TYPE_KIND_PRIMITIVE:
            switch (expected) {
                case TYPE_ID_STRING:
                    if (!cJSON_IsString(json)) {
                        exec_err(diag, "R155", json_path, "expected string");
                        return NULL;
                    }
                    return value_new_string(arena, TYPE_ID_STRING,
                        cJSON_GetStringValue(json));
                case TYPE_ID_INT: {
                    if (!cJSON_IsNumber(json)) {
                        exec_err(diag, "R155", json_path, "expected int");
                        return NULL;
                    }
                    bool ok; int64_t i = json_number_to_int(json, &ok);
                    if (!ok) {
                        exec_err(diag, "R155", json_path,
                                 "expected int, got non-integer number");
                        return NULL;
                    }
                    return value_new_int(arena, TYPE_ID_INT, i);
                }
                case TYPE_ID_FLOAT:
                    if (!cJSON_IsNumber(json)) {
                        exec_err(diag, "R155", json_path, "expected float");
                        return NULL;
                    }
                    return value_new_float(arena, TYPE_ID_FLOAT,
                                           json->valuedouble);
                case TYPE_ID_BOOL:
                    if (!cJSON_IsBool(json)) {
                        exec_err(diag, "R155", json_path, "expected bool");
                        return NULL;
                    }
                    return value_new_bool(arena, TYPE_ID_BOOL,
                                          cJSON_IsTrue(json));
                default:
                    exec_err(diag, "R155", json_path,
                             "internal: unknown primitive id %u",
                             (unsigned)expected);
                    return NULL;
            }

        case TYPE_KIND_LIST: {
            if (!cJSON_IsArray(json)) {
                exec_err(diag, "R155", json_path, "expected list");
                return NULL;
            }
            int n = cJSON_GetArraySize(json);
            value_t **items = n > 0
                ? arena_alloc_zero(arena, (size_t)n * sizeof *items)
                : NULL;
            for (int i = 0; i < n; i++) {
                cJSON *el = cJSON_GetArrayItem(json, i);
                char child_path[256];
                snprintf(child_path, sizeof child_path,
                         "%s[%d]", json_path ? json_path : "", i);
                items[i] = value_from_json(arena, types, t->elem, el,
                                           diag, child_path);
                if (items[i] == NULL) return NULL;
            }
            return value_new_list_take(arena, expected, items,
                                       (size_t)n);
        }

        case TYPE_KIND_RECORD: {
            if (!cJSON_IsObject(json)) {
                exec_err(diag, "R155", json_path, "expected record object");
                return NULL;
            }
            int got = cJSON_GetArraySize(json);
            if ((size_t)got != t->n_fields) {
                exec_err(diag, "R155", json_path,
                         "record '%s' expects %zu fields, got %d",
                         t->name, t->n_fields, got);
                return NULL;
            }
            value_field_t *fields = t->n_fields > 0
                ? arena_alloc_zero(arena,
                                   t->n_fields * sizeof *fields)
                : NULL;
            for (size_t i = 0; i < t->n_fields; i++) {
                cJSON *fv = cJSON_GetObjectItemCaseSensitive(
                    json, t->fields[i].name);
                if (fv == NULL) {
                    exec_err(diag, "R155", json_path,
                             "record '%s' missing field '%s'",
                             t->name, t->fields[i].name);
                    return NULL;
                }
                char child_path[256];
                snprintf(child_path, sizeof child_path,
                         "%s.%s", json_path ? json_path : "",
                         t->fields[i].name);
                fields[i].name  = arena_strdup(arena, t->fields[i].name);
                fields[i].value = value_from_json(arena, types,
                                                  t->fields[i].type,
                                                  fv, diag, child_path);
                if (fields[i].value == NULL) return NULL;
            }
            return value_new_record_take(arena, expected, fields,
                                         t->n_fields);
        }

        case TYPE_KIND_SUM: {
            if (!cJSON_IsObject(json)) {
                exec_err(diag, "R155", json_path,
                         "expected sum object {\"variant\":..., "
                         "\"fields\":...}");
                return NULL;
            }
            cJSON *vname_v = cJSON_GetObjectItemCaseSensitive(
                json, "variant");
            cJSON *fields_v = cJSON_GetObjectItemCaseSensitive(
                json, "fields");
            if (!cJSON_IsString(vname_v) || !cJSON_IsObject(fields_v)) {
                exec_err(diag, "R155", json_path,
                         "sum requires string 'variant' and object 'fields'");
                return NULL;
            }
            const char *vname = cJSON_GetStringValue(vname_v);
            const type_variant_t *tv = NULL;
            for (size_t i = 0; i < t->n_variants; i++) {
                if (strcmp(t->variants[i].name, vname) == 0) {
                    tv = &t->variants[i];
                    break;
                }
            }
            if (tv == NULL) {
                exec_err(diag, "R155", json_path,
                         "unknown variant '%s' of sum '%s'",
                         vname, t->name);
                return NULL;
            }
            int got = cJSON_GetArraySize(fields_v);
            if ((size_t)got != tv->n_fields) {
                exec_err(diag, "R155", json_path,
                         "variant '%s.%s' expects %zu fields, got %d",
                         t->name, vname, tv->n_fields, got);
                return NULL;
            }
            value_field_t *fields = tv->n_fields > 0
                ? arena_alloc_zero(arena,
                                   tv->n_fields * sizeof *fields)
                : NULL;
            for (size_t i = 0; i < tv->n_fields; i++) {
                cJSON *fv = cJSON_GetObjectItemCaseSensitive(
                    fields_v, tv->fields[i].name);
                if (fv == NULL) {
                    exec_err(diag, "R155", json_path,
                             "variant '%s.%s' missing field '%s'",
                             t->name, vname, tv->fields[i].name);
                    return NULL;
                }
                char child_path[256];
                snprintf(child_path, sizeof child_path,
                         "%s.fields.%s",
                         json_path ? json_path : "",
                         tv->fields[i].name);
                fields[i].name  = arena_strdup(arena,
                                               tv->fields[i].name);
                fields[i].value = value_from_json(arena, types,
                                                  tv->fields[i].type,
                                                  fv, diag, child_path);
                if (fields[i].value == NULL) return NULL;
            }
            return value_new_variant_take(arena, expected, vname,
                                          fields, tv->n_fields);
        }
    }
    exec_err(diag, "R155", json_path, "internal: unhandled type kind");
    return NULL;
}


/* ====================================================================
 * value_t → JSON
 * ==================================================================== */

char *
value_to_json_canonical(const value_t *v)
{
    char  *buf = NULL;
    size_t sz  = 0;
    FILE *fp = open_memstream(&buf, &sz);
    if (fp == NULL) return NULL;
    value_canonical_serialize(v, fp);
    fclose(fp);
    return buf;
}


/* ====================================================================
 * Where-predicate parser
 *
 * flowc emits where predicates as a string ("cost <= 100 and
 * success_rate >= 0.9") rather than as an expression tree. This is
 * inconsistent with other stages whose predicates are trees; until
 * the compiler emits a tree, we parse the string here.
 *
 * Grammar (subset of the v1 surface):
 *
 *   pred    ::= and_expr ( "or" and_expr )*
 *   and_expr ::= unary   ( "and" unary )*
 *   unary   ::= "not" unary | primary
 *   primary ::= cmp_term  cmp_op  cmp_term
 *             | bool_lit
 *             | "(" pred ")"
 *   cmp_op  ::= "<" | "<=" | ">" | ">=" | "==" | "!="
 *   cmp_term ::= identifier | int_lit | float_lit | string_lit
 *
 * Identifiers resolve as field lookups on the row record (implicit-
 * `it` rule). No general expressions; the where-
 * predicate restriction (primary_expr operands, no arithmetic)
 * keeps the parser tiny.
 * ==================================================================== */

typedef struct {
    const char  *src;
    size_t       pos;
    DiagStream  *diag;
    bool         err;
} pp_t;

static void
pp_skip_ws(pp_t *p)
{
    while (p->src[p->pos] && isspace((unsigned char)p->src[p->pos])) p->pos++;
}

static bool
pp_eat_keyword(pp_t *p, const char *kw)
{
    pp_skip_ws(p);
    size_t n = strlen(kw);
    if (strncmp(p->src + p->pos, kw, n) != 0) return false;
    /* Word boundary: the char after kw must not be an ident char. */
    char c = p->src[p->pos + n];
    if (isalnum((unsigned char)c) || c == '_') return false;
    p->pos += n;
    return true;
}

static bool
pp_eat_char(pp_t *p, char ch)
{
    pp_skip_ws(p);
    if (p->src[p->pos] != ch) return false;
    p->pos++;
    return true;
}

/* Result kinds for cmp_term and pred. */
typedef struct {
    enum { TERM_BOOL, TERM_INT, TERM_FLOAT, TERM_STR } kind;
    union { bool b; int64_t i; double f; const char *s; } u;
} pp_term_t;

static bool
pp_parse_ident(pp_t *p, char *out, size_t cap)
{
    pp_skip_ws(p);
    size_t start = p->pos;
    if (!(isalpha((unsigned char)p->src[start]) || p->src[start] == '_')) {
        return false;
    }
    size_t i = 0;
    while (isalnum((unsigned char)p->src[p->pos]) || p->src[p->pos] == '_') {
        if (i + 1u < cap) out[i++] = p->src[p->pos];
        p->pos++;
    }
    out[i] = '\0';
    return i > 0;
}

static bool
pp_parse_number(pp_t *p, pp_term_t *out)
{
    pp_skip_ws(p);
    size_t start = p->pos;
    if (p->src[start] == '-' || p->src[start] == '+') p->pos++;
    bool any = false, isf = false;
    while (isdigit((unsigned char)p->src[p->pos])) { p->pos++; any = true; }
    if (p->src[p->pos] == '.') {
        isf = true; p->pos++;
        while (isdigit((unsigned char)p->src[p->pos])) { p->pos++; any = true; }
    }
    if (!any) { p->pos = start; return false; }
    if (isf) {
        out->kind  = TERM_FLOAT;
        out->u.f   = strtod(p->src + start, NULL);
    } else {
        out->kind  = TERM_INT;
        out->u.i   = strtoll(p->src + start, NULL, 10);
    }
    return true;
}

static bool
pp_parse_string(pp_t *p, pp_term_t *out, Arena *arena)
{
    pp_skip_ws(p);
    if (p->src[p->pos] != '"') return false;
    size_t start = ++p->pos;
    while (p->src[p->pos] && p->src[p->pos] != '"') {
        if (p->src[p->pos] == '\\' && p->src[p->pos + 1]) p->pos += 2;
        else p->pos++;
    }
    if (p->src[p->pos] != '"') return false;
    size_t len = p->pos - start;
    out->kind  = TERM_STR;
    out->u.s   = arena_strndup(arena, p->src + start, len);
    p->pos++;
    return true;
}

/* Evaluate a cmp_term against the current row's record fields. */
static bool
pp_eval_term(pp_t *p, const value_t *row, Arena *arena, pp_term_t *out)
{
    pp_skip_ws(p);
    if (pp_eat_keyword(p, "true"))  { out->kind = TERM_BOOL; out->u.b = true;  return true; }
    if (pp_eat_keyword(p, "false")) { out->kind = TERM_BOOL; out->u.b = false; return true; }
    if (p->src[p->pos] == '"') {
        return pp_parse_string(p, out, arena);
    }
    char c = p->src[p->pos];
    if (isdigit((unsigned char)c) || c == '-' || c == '+' || c == '.') {
        return pp_parse_number(p, out);
    }
    /* Identifier — look up as field on row. */
    char name[128];
    if (!pp_parse_ident(p, name, sizeof name)) {
        exec_err(p->diag, "R155", "where", "expected term at position %zu", p->pos);
        p->err = true;
        return false;
    }
    if (row->kind != VAL_RECORD) {
        exec_err(p->diag, "R155", "where",
                 "field '%s' lookup on non-record row", name);
        p->err = true;
        return false;
    }
    for (size_t i = 0; i < row->u.record.n; i++) {
        if (strcmp(row->u.record.fields[i].name, name) == 0) {
            const value_t *v = row->u.record.fields[i].value;
            switch (v->kind) {
                case VAL_BOOL:   out->kind = TERM_BOOL;  out->u.b = v->u.b; return true;
                case VAL_INT:    out->kind = TERM_INT;   out->u.i = v->u.i; return true;
                case VAL_FLOAT:  out->kind = TERM_FLOAT; out->u.f = v->u.f; return true;
                case VAL_STRING: out->kind = TERM_STR;   out->u.s = v->u.s; return true;
                default:
                    exec_err(p->diag, "R155", "where",
                             "field '%s' has non-scalar kind, "
                             "not supported in where predicates",
                             name);
                    p->err = true;
                    return false;
            }
        }
    }
    exec_err(p->diag, "R155", "where", "unknown field '%s' in where predicate",
             name);
    p->err = true;
    return false;
}

static int
pp_compare(pp_term_t a, pp_term_t b)
{
    /* Coerce int <-> float for cross-numeric comparison. Strings only
     * compare with strings, bools with bools. */
    if ((a.kind == TERM_INT || a.kind == TERM_FLOAT) &&
        (b.kind == TERM_INT || b.kind == TERM_FLOAT)) {
        double da = (a.kind == TERM_INT) ? (double)a.u.i : a.u.f;
        double db = (b.kind == TERM_INT) ? (double)b.u.i : b.u.f;
        if (da < db) return -1;
        if (da > db) return  1;
        return 0;
    }
    if (a.kind == TERM_STR && b.kind == TERM_STR) {
        return strcmp(a.u.s, b.u.s);
    }
    if (a.kind == TERM_BOOL && b.kind == TERM_BOOL) {
        return (int)a.u.b - (int)b.u.b;
    }
    return INT_MIN;   /* incomparable */
}

static bool pp_parse_or(pp_t *p, const value_t *row, Arena *arena);

static bool
pp_parse_primary(pp_t *p, const value_t *row, Arena *arena)
{
    pp_skip_ws(p);
    if (pp_eat_char(p, '(')) {
        bool r = pp_parse_or(p, row, arena);
        if (!pp_eat_char(p, ')')) {
            exec_err(p->diag, "R155", "where", "expected ')'");
            p->err = true;
        }
        return r;
    }
    /* Could be a bare bool literal (true/false). Peek before consuming
     * a full term. */
    {
        size_t save = p->pos;
        if (pp_eat_keyword(p, "true"))  return true;
        if (pp_eat_keyword(p, "false")) return false;
        p->pos = save;
    }
    pp_term_t lhs;
    if (!pp_eval_term(p, row, arena, &lhs)) return false;
    pp_skip_ws(p);

    /* Read at most two operator chars. op[0] may be the terminating
     * '\0' (end of input after a bare-bool term); only then read
     * src[pos+1], which would otherwise be one byte past the NUL. */
    char op[3];
    op[0] = p->src[p->pos];
    op[1] = op[0] ? p->src[p->pos + 1] : '\0';
    op[2] = '\0';
    enum { CMP_LT, CMP_LE, CMP_GT, CMP_GE, CMP_EQ, CMP_NE } cmp;
    if      (op[0] == '<' && op[1] == '=') { cmp = CMP_LE; p->pos += 2; }
    else if (op[0] == '>' && op[1] == '=') { cmp = CMP_GE; p->pos += 2; }
    else if (op[0] == '=' && op[1] == '=') { cmp = CMP_EQ; p->pos += 2; }
    else if (op[0] == '!' && op[1] == '=') { cmp = CMP_NE; p->pos += 2; }
    else if (op[0] == '<')                 { cmp = CMP_LT; p->pos += 1; }
    else if (op[0] == '>')                 { cmp = CMP_GT; p->pos += 1; }
    else {
        /* Bare term — only valid if it's a bool. */
        if (lhs.kind == TERM_BOOL) return lhs.u.b;
        exec_err(p->diag, "R155", "where",
                 "expected comparison operator after term");
        p->err = true;
        return false;
    }
    pp_term_t rhs;
    if (!pp_eval_term(p, row, arena, &rhs)) return false;
    int c = pp_compare(lhs, rhs);
    if (c == INT_MIN) {
        exec_err(p->diag, "R155", "where",
                 "incompatible types in where comparison");
        p->err = true;
        return false;
    }
    switch (cmp) {
        case CMP_LT: return c <  0;
        case CMP_LE: return c <= 0;
        case CMP_GT: return c >  0;
        case CMP_GE: return c >= 0;
        case CMP_EQ: return c == 0;
        case CMP_NE: return c != 0;
    }
    return false;
}

static bool
pp_parse_unary(pp_t *p, const value_t *row, Arena *arena)
{
    pp_skip_ws(p);
    if (pp_eat_keyword(p, "not")) {
        return !pp_parse_unary(p, row, arena);
    }
    return pp_parse_primary(p, row, arena);
}

static bool
pp_parse_and(pp_t *p, const value_t *row, Arena *arena)
{
    bool lhs = pp_parse_unary(p, row, arena);
    for (;;) {
        if (p->err) return false;
        if (!pp_eat_keyword(p, "and")) break;
        bool rhs = pp_parse_unary(p, row, arena);
        lhs = lhs && rhs;
    }
    return lhs;
}

static bool
pp_parse_or(pp_t *p, const value_t *row, Arena *arena)
{
    bool lhs = pp_parse_and(p, row, arena);
    for (;;) {
        if (p->err) return false;
        if (!pp_eat_keyword(p, "or")) break;
        bool rhs = pp_parse_and(p, row, arena);
        lhs = lhs || rhs;
    }
    return lhs;
}

/* Parse + evaluate. Sets *ok to false on a parse error. */
static bool
where_predicate_eval(const char *predicate, const value_t *row,
                     Arena *arena, DiagStream *diag, bool *ok)
{
    pp_t p = { predicate, 0, diag, false };
    bool r = pp_parse_or(&p, row, arena);
    pp_skip_ws(&p);
    if (!p.err && p.src[p.pos] != '\0') {
        exec_err(diag, "R155", "where",
                 "trailing input in where predicate: '%s'",
                 p.src + p.pos);
        p.err = true;
    }
    *ok = !p.err;
    return r;
}


/* ====================================================================
 * Forward declarations — expression dispatch
 * ==================================================================== */

static exec_result_t eval_expr(exec_ctx_t *ctx, const cJSON *expr,
                               env_t *env);


/* ====================================================================
 * eval_literal, eval_path
 * ==================================================================== */

static exec_result_t
eval_literal(exec_ctx_t *ctx, const cJSON *expr)
{
    const cJSON *type_v  = cJSON_GetObjectItemCaseSensitive(
        expr, "type");
    const cJSON *value_v = cJSON_GetObjectItemCaseSensitive(
        expr, "value");
    if (!cJSON_IsString(type_v) || value_v == NULL) {
        exec_err(ctx->diag, "R155", "literal",
                 "missing 'type' or 'value' on literal");
        return mk_err();
    }
    const char *ty = cJSON_GetStringValue(type_v);
    if (strcmp(ty, "string") == 0) {
        if (!cJSON_IsString(value_v)) {
            exec_err(ctx->diag, "R155", "literal",
                     "string literal needs string value");
            return mk_err();
        }
        return mk_ok(value_new_string(ctx->arena, TYPE_ID_STRING,
            cJSON_GetStringValue(value_v)));
    }
    if (strcmp(ty, "int") == 0) {
        if (!cJSON_IsNumber(value_v)) {
            exec_err(ctx->diag, "R155", "literal",
                     "int literal needs number value");
            return mk_err();
        }
        bool ok; int64_t i = json_number_to_int(value_v, &ok);
        if (!ok) {
            exec_err(ctx->diag, "R155", "literal",
                     "int literal value is non-integer");
            return mk_err();
        }
        return mk_ok(value_new_int(ctx->arena, TYPE_ID_INT, i));
    }
    if (strcmp(ty, "float") == 0) {
        if (!cJSON_IsNumber(value_v)) {
            exec_err(ctx->diag, "R155", "literal",
                     "float literal needs number value");
            return mk_err();
        }
        return mk_ok(value_new_float(ctx->arena, TYPE_ID_FLOAT,
                                     value_v->valuedouble));
    }
    if (strcmp(ty, "bool") == 0) {
        if (!cJSON_IsBool(value_v)) {
            exec_err(ctx->diag, "R155", "literal",
                     "bool literal needs bool value");
            return mk_err();
        }
        return mk_ok(value_new_bool(ctx->arena, TYPE_ID_BOOL,
                                    cJSON_IsTrue(value_v)));
    }
    exec_err(ctx->diag, "R155", "literal", "unknown literal type '%s'", ty);
    return mk_err();
}

static exec_result_t
eval_path(exec_ctx_t *ctx, const cJSON *expr, env_t *env)
{
    const cJSON *segs = cJSON_GetObjectItemCaseSensitive(
        expr, "segments");
    if (!cJSON_IsArray(segs)) {
        exec_err(ctx->diag, "R155", "path", "missing 'segments' array");
        return mk_err();
    }
    int n = cJSON_GetArraySize(segs);
    if (n == 0) {
        exec_err(ctx->diag, "R155", "path", "empty path");
        return mk_err();
    }
    cJSON *head = cJSON_GetArrayItem(segs, 0);
    if (!cJSON_IsString(head)) {
        exec_err(ctx->diag, "R155", "path",
                 "first segment must be a name");
        return mk_err();
    }
    const char *root_name = cJSON_GetStringValue(head);
    const value_t *cur = env_lookup(env, root_name);
    if (cur == NULL) {
        exec_err(ctx->diag, "R155", "path",
                 "unbound name '%s'", root_name);
        return mk_err();
    }
    for (int i = 1; i < n; i++) {
        cJSON *seg = cJSON_GetArrayItem(segs, i);
        if (!cJSON_IsString(seg)) {
            exec_err(ctx->diag, "R155", "path",
                     "segment %d is not a string", i);
            return mk_err();
        }
        const char *field = cJSON_GetStringValue(seg);
        if (cur->kind != VAL_RECORD) {
            exec_err(ctx->diag, "R155", "path",
                     "cannot access field '%s' on non-record value",
                     field);
            return mk_err();
        }
        const value_t *next = NULL;
        for (size_t k = 0; k < cur->u.record.n; k++) {
            if (strcmp(cur->u.record.fields[k].name, field) == 0) {
                next = cur->u.record.fields[k].value;
                break;
            }
        }
        if (next == NULL) {
            exec_err(ctx->diag, "R155", "path",
                     "record has no field '%s'", field);
            return mk_err();
        }
        cur = next;
    }
    return mk_ok((value_t *)cur);
}


/* ====================================================================
 * eval_construct / eval_construct_variant / eval_list_literal
 * ==================================================================== */

static const type_t *
lookup_type_by_name(const type_registry_t *r, const char *name)
{
    return type_registry_get(r,
        type_registry_lookup_by_name((type_registry_t *)r, name));
}

static exec_result_t
eval_construct(exec_ctx_t *ctx, const cJSON *expr, env_t *env)
{
    const cJSON *type_v   = cJSON_GetObjectItemCaseSensitive(expr, "type");
    const cJSON *fields_v = cJSON_GetObjectItemCaseSensitive(expr, "fields");
    if (!cJSON_IsString(type_v) || !cJSON_IsArray(fields_v)) {
        exec_err(ctx->diag, "R155", "construct",
                 "missing 'type' or 'fields'");
        return mk_err();
    }
    const char  *tname = cJSON_GetStringValue(type_v);
    const type_t *t    = lookup_type_by_name(flowd_types(ctx->rt), tname);
    if (t == NULL || t->kind != TYPE_KIND_RECORD) {
        exec_err(ctx->diag, "R155", "construct",
                 "type '%s' is not a known record type", tname);
        return mk_err();
    }
    size_t got = (size_t)cJSON_GetArraySize(fields_v);
    if (got != t->n_fields) {
        exec_err(ctx->diag, "R155", "construct",
                 "record '%s' expects %zu fields, got %zu",
                 tname, t->n_fields, got);
        return mk_err();
    }
    value_field_t *fields = t->n_fields > 0
        ? arena_alloc_zero(ctx->arena, t->n_fields * sizeof *fields)
        : NULL;
    /* Each declared field must appear in the IR's fields array. The
     * IR carries them in source order; we re-key by name to be
     * order-independent. */
    for (size_t i = 0; i < t->n_fields; i++) {
        cJSON *fv = NULL;
        for (size_t j = 0; j < got; j++) {
            cJSON *cand = cJSON_GetArrayItem(fields_v, (int)j);
            cJSON *cn = cJSON_GetObjectItemCaseSensitive(cand, "name");
            if (cJSON_IsString(cn) &&
                strcmp(cJSON_GetStringValue(cn),
                       t->fields[i].name) == 0) {
                fv = cJSON_GetObjectItemCaseSensitive(cand, "value");
                break;
            }
        }
        if (fv == NULL) {
            exec_err(ctx->diag, "R155", "construct",
                     "record '%s' missing field '%s' in construction",
                     tname, t->fields[i].name);
            return mk_err();
        }
        exec_result_t r = eval_expr(ctx, fv, env);
        if (r.status != EXEC_OK) return r;
        fields[i].name  = arena_strdup(ctx->arena, t->fields[i].name);
        fields[i].value = r.value;
    }
    return mk_ok(value_new_record_take(ctx->arena, t->id,
                                       fields, t->n_fields));
}

static exec_result_t
eval_construct_variant(exec_ctx_t *ctx, const cJSON *expr, env_t *env)
{
    const cJSON *type_v    = cJSON_GetObjectItemCaseSensitive(
        expr, "type");
    const cJSON *variant_v = cJSON_GetObjectItemCaseSensitive(
        expr, "variant");
    const cJSON *fields_v  = cJSON_GetObjectItemCaseSensitive(
        expr, "fields");
    if (!cJSON_IsString(type_v) || !cJSON_IsString(variant_v)
                                || !cJSON_IsArray(fields_v)) {
        exec_err(ctx->diag, "R155", "construct_variant",
                 "missing 'type', 'variant', or 'fields'");
        return mk_err();
    }
    const char *tname = cJSON_GetStringValue(type_v);
    const char *vname = cJSON_GetStringValue(variant_v);
    const type_t *t = lookup_type_by_name(flowd_types(ctx->rt), tname);
    if (t == NULL || t->kind != TYPE_KIND_SUM) {
        exec_err(ctx->diag, "R155", "construct_variant",
                 "type '%s' is not a known sum type", tname);
        return mk_err();
    }
    const type_variant_t *tv = NULL;
    for (size_t i = 0; i < t->n_variants; i++) {
        if (strcmp(t->variants[i].name, vname) == 0) {
            tv = &t->variants[i];
            break;
        }
    }
    if (tv == NULL) {
        exec_err(ctx->diag, "R155", "construct_variant",
                 "unknown variant '%s' of sum '%s'", vname, tname);
        return mk_err();
    }
    size_t got = (size_t)cJSON_GetArraySize(fields_v);
    if (got != tv->n_fields) {
        exec_err(ctx->diag, "R155", "construct_variant",
                 "variant '%s.%s' expects %zu fields, got %zu",
                 tname, vname, tv->n_fields, got);
        return mk_err();
    }
    value_field_t *fields = tv->n_fields > 0
        ? arena_alloc_zero(ctx->arena, tv->n_fields * sizeof *fields)
        : NULL;
    for (size_t i = 0; i < tv->n_fields; i++) {
        cJSON *fv = NULL;
        for (size_t j = 0; j < got; j++) {
            cJSON *cand = cJSON_GetArrayItem(fields_v, (int)j);
            cJSON *cn = cJSON_GetObjectItemCaseSensitive(cand, "name");
            if (cJSON_IsString(cn) &&
                strcmp(cJSON_GetStringValue(cn),
                       tv->fields[i].name) == 0) {
                fv = cJSON_GetObjectItemCaseSensitive(cand, "value");
                break;
            }
        }
        if (fv == NULL) {
            exec_err(ctx->diag, "R155", "construct_variant",
                     "variant '%s.%s' missing field '%s'",
                     tname, vname, tv->fields[i].name);
            return mk_err();
        }
        exec_result_t r = eval_expr(ctx, fv, env);
        if (r.status != EXEC_OK) return r;
        fields[i].name  = arena_strdup(ctx->arena, tv->fields[i].name);
        fields[i].value = r.value;
    }
    return mk_ok(value_new_variant_take(ctx->arena, t->id, vname,
                                        fields, tv->n_fields));
}

static exec_result_t
eval_list_literal(exec_ctx_t *ctx, const cJSON *expr, env_t *env)
{
    const cJSON *etype_v = cJSON_GetObjectItemCaseSensitive(
        expr, "element_type");
    const cJSON *els_v   = cJSON_GetObjectItemCaseSensitive(
        expr, "elements");
    if (!cJSON_IsString(etype_v) || !cJSON_IsArray(els_v)) {
        exec_err(ctx->diag, "R155", "list_literal",
                 "missing 'element_type' or 'elements'");
        return mk_err();
    }
    type_id_t etype = type_registry_lookup_by_name(
        (type_registry_t *)flowd_types(ctx->rt),
        cJSON_GetStringValue(etype_v));
    if (etype == TYPE_ID_NONE) {
        exec_err(ctx->diag, "R155", "list_literal",
                 "unknown element type '%s'",
                 cJSON_GetStringValue(etype_v));
        return mk_err();
    }
    type_id_t list_type = type_registry_intern_list(
        (type_registry_t *)flowd_types(ctx->rt), etype);
    int n = cJSON_GetArraySize(els_v);
    value_t **items = n > 0
        ? arena_alloc_zero(ctx->arena, (size_t)n * sizeof *items)
        : NULL;
    for (int i = 0; i < n; i++) {
        cJSON *el = cJSON_GetArrayItem(els_v, i);
        exec_result_t r = eval_expr(ctx, el, env);
        if (r.status != EXEC_OK) return r;
        items[i] = r.value;
    }
    return mk_ok(value_new_list_take(ctx->arena, list_type, items,
                                     (size_t)n));
}


/* ====================================================================
 * eval_binop / eval_unop
 * ==================================================================== */

static exec_result_t
eval_binop(exec_ctx_t *ctx, const cJSON *expr, env_t *env)
{
    const cJSON *op_v    = cJSON_GetObjectItemCaseSensitive(expr, "op");
    const cJSON *left_v  = cJSON_GetObjectItemCaseSensitive(expr, "left");
    const cJSON *right_v = cJSON_GetObjectItemCaseSensitive(expr, "right");
    if (!cJSON_IsString(op_v) || !cJSON_IsObject(left_v)
                              || !cJSON_IsObject(right_v)) {
        exec_err(ctx->diag, "R155", "binop",
                 "missing 'op', 'left', or 'right'");
        return mk_err();
    }
    const char *op = cJSON_GetStringValue(op_v);

    /* Short-circuit and/or before evaluating right. */
    if (strcmp(op, "and") == 0 || strcmp(op, "or") == 0) {
        exec_result_t lr = eval_expr(ctx, left_v, env);
        if (lr.status != EXEC_OK) return lr;
        if (lr.value->kind != VAL_BOOL) {
            exec_err(ctx->diag, "R155", "binop",
                     "%s requires bool operands", op);
            return mk_err();
        }
        bool lb = lr.value->u.b;
        if (strcmp(op, "and") == 0 && !lb)
            return mk_ok(value_new_bool(ctx->arena, TYPE_ID_BOOL, false));
        if (strcmp(op, "or") == 0  &&  lb)
            return mk_ok(value_new_bool(ctx->arena, TYPE_ID_BOOL, true));
        exec_result_t rr = eval_expr(ctx, right_v, env);
        if (rr.status != EXEC_OK) return rr;
        if (rr.value->kind != VAL_BOOL) {
            exec_err(ctx->diag, "R155", "binop",
                     "%s requires bool operands", op);
            return mk_err();
        }
        return mk_ok(value_new_bool(ctx->arena, TYPE_ID_BOOL,
                                    rr.value->u.b));
    }

    exec_result_t lr = eval_expr(ctx, left_v, env);
    if (lr.status != EXEC_OK) return lr;
    exec_result_t rr = eval_expr(ctx, right_v, env);
    if (rr.status != EXEC_OK) return rr;
    const value_t *L = lr.value, *R = rr.value;

    /* == and != work on any matching type. */
    if (strcmp(op, "==") == 0) {
        return mk_ok(value_new_bool(ctx->arena, TYPE_ID_BOOL,
                                    value_equal(L, R)));
    }
    if (strcmp(op, "!=") == 0) {
        return mk_ok(value_new_bool(ctx->arena, TYPE_ID_BOOL,
                                    !value_equal(L, R)));
    }

    /* Numeric and ordering ops below. Both sides must be int/int or
     * float/float; the typechecker has guaranteed this when the IR
     * is well-formed, so we treat a mismatch as an exec error.
     * String + string is concatenation. */
    if (L->kind == VAL_STRING && R->kind == VAL_STRING
                              && strcmp(op, "+") == 0) {
        size_t la = strlen(L->u.s), lb = strlen(R->u.s);
        char *buf = arena_alloc(ctx->arena, la + lb + 1u);
        memcpy(buf, L->u.s, la);
        memcpy(buf + la, R->u.s, lb);
        buf[la + lb] = '\0';
        value_t *v = value_new_string(ctx->arena, TYPE_ID_STRING, buf);
        return mk_ok(v);
    }

    if (L->kind == VAL_INT && R->kind == VAL_INT) {
        int64_t a = L->u.i, b = R->u.i, r = 0;
        bool cmp = false; bool cmpv = false;
        /* Signed int64 overflow is UB; the wrap is not "just a big number".
         * Detect it and error instead of letting the compiler miscompile. */
        if      (strcmp(op, "+") == 0) {
            if (__builtin_add_overflow(a, b, &r)) { exec_err(ctx->diag, "R140", "binop", "int arithmetic overflow"); return mk_err(); }
        }
        else if (strcmp(op, "-") == 0) {
            if (__builtin_sub_overflow(a, b, &r)) { exec_err(ctx->diag, "R140", "binop", "int arithmetic overflow"); return mk_err(); }
        }
        else if (strcmp(op, "*") == 0) {
            if (__builtin_mul_overflow(a, b, &r)) { exec_err(ctx->diag, "R140", "binop", "int arithmetic overflow"); return mk_err(); }
        }
        else if (strcmp(op, "/") == 0) {
            if (b == 0) { exec_err(ctx->diag, "R140", "binop", "int division by zero"); return mk_err(); }
            r = a / b;
        }
        else if (strcmp(op, "%") == 0) {
            if (b == 0) { exec_err(ctx->diag, "R140", "binop", "int modulo by zero"); return mk_err(); }
            r = a % b;
        }
        else if (strcmp(op, "<")  == 0) { cmp = true; cmpv = a <  b; }
        else if (strcmp(op, "<=") == 0) { cmp = true; cmpv = a <= b; }
        else if (strcmp(op, ">")  == 0) { cmp = true; cmpv = a >  b; }
        else if (strcmp(op, ">=") == 0) { cmp = true; cmpv = a >= b; }
        else {
            exec_err(ctx->diag, "R155", "binop", "unsupported int binop '%s'", op);
            return mk_err();
        }
        return cmp
            ? mk_ok(value_new_bool(ctx->arena, TYPE_ID_BOOL, cmpv))
            : mk_ok(value_new_int (ctx->arena, TYPE_ID_INT,  r));
    }
    if (L->kind == VAL_FLOAT && R->kind == VAL_FLOAT) {
        double a = L->u.f, b = R->u.f, r = 0;
        bool cmp = false, cmpv = false;
        if      (strcmp(op, "+") == 0) r = a + b;
        else if (strcmp(op, "-") == 0) r = a - b;
        else if (strcmp(op, "*") == 0) r = a * b;
        else if (strcmp(op, "/") == 0) r = a / b;
        else if (strcmp(op, "<")  == 0) { cmp = true; cmpv = a <  b; }
        else if (strcmp(op, "<=") == 0) { cmp = true; cmpv = a <= b; }
        else if (strcmp(op, ">")  == 0) { cmp = true; cmpv = a >  b; }
        else if (strcmp(op, ">=") == 0) { cmp = true; cmpv = a >= b; }
        else {
            exec_err(ctx->diag, "R155", "binop", "unsupported float binop '%s'", op);
            return mk_err();
        }
        return cmp
            ? mk_ok(value_new_bool (ctx->arena, TYPE_ID_BOOL,  cmpv))
            : mk_ok(value_new_float(ctx->arena, TYPE_ID_FLOAT, r));
    }
    exec_err(ctx->diag, "R155", "binop",
             "incompatible operand kinds for '%s'", op);
    return mk_err();
}

static exec_result_t
eval_unop(exec_ctx_t *ctx, const cJSON *expr, env_t *env)
{
    const cJSON *op_v       = cJSON_GetObjectItemCaseSensitive(
        expr, "op");
    const cJSON *operand_v  = cJSON_GetObjectItemCaseSensitive(
        expr, "operand");
    if (!cJSON_IsString(op_v) || !cJSON_IsObject(operand_v)) {
        exec_err(ctx->diag, "R155", "unop", "missing 'op' or 'operand'");
        return mk_err();
    }
    const char *op = cJSON_GetStringValue(op_v);
    exec_result_t r = eval_expr(ctx, operand_v, env);
    if (r.status != EXEC_OK) return r;
    if (strcmp(op, "-") == 0) {
        if (r.value->kind == VAL_INT) {
            /* -INT64_MIN overflows int64 (its magnitude has no positive
             * counterpart) and negating it is UB; reject it as overflow. */
            if (r.value->u.i == INT64_MIN) { exec_err(ctx->diag, "R140", "unop", "int arithmetic overflow"); return mk_err(); }
            return mk_ok(value_new_int  (ctx->arena, TYPE_ID_INT,   -r.value->u.i));
        }
        if (r.value->kind == VAL_FLOAT)
            return mk_ok(value_new_float(ctx->arena, TYPE_ID_FLOAT, -r.value->u.f));
        exec_err(ctx->diag, "R155", "unop", "unary - requires numeric");
        return mk_err();
    }
    if (strcmp(op, "not") == 0) {
        if (r.value->kind == VAL_BOOL)
            return mk_ok(value_new_bool(ctx->arena, TYPE_ID_BOOL,
                                        !r.value->u.b));
        exec_err(ctx->diag, "R155", "unop", "not requires bool");
        return mk_err();
    }
    exec_err(ctx->diag, "R155", "unop", "unknown unary op '%s'", op);
    return mk_err();
}


/* ====================================================================
 * eval_conditional / eval_try_else
 * ==================================================================== */

static exec_result_t
eval_conditional(exec_ctx_t *ctx, const cJSON *expr, env_t *env)
{
    const cJSON *branches = cJSON_GetObjectItemCaseSensitive(
        expr, "branches");
    const cJSON *else_v   = cJSON_GetObjectItemCaseSensitive(
        expr, "else");
    if (!cJSON_IsArray(branches) || !cJSON_IsObject(else_v)) {
        exec_err(ctx->diag, "R155", "conditional",
                 "missing 'branches' array or 'else' expression");
        return mk_err();
    }
    int n = cJSON_GetArraySize(branches);
    for (int i = 0; i < n; i++) {
        cJSON *b = cJSON_GetArrayItem(branches, i);
        cJSON *cond = cJSON_GetObjectItemCaseSensitive(b, "cond");
        cJSON *body = cJSON_GetObjectItemCaseSensitive(b, "consequent");
        if (!cJSON_IsObject(cond) || !cJSON_IsObject(body)) {
            exec_err(ctx->diag, "R155", "conditional",
                     "branch %d malformed", i);
            return mk_err();
        }
        exec_result_t cr = eval_expr(ctx, cond, env);
        if (cr.status != EXEC_OK) return cr;
        if (cr.value->kind != VAL_BOOL) {
            exec_err(ctx->diag, "R155", "conditional",
                     "branch %d condition is not bool", i);
            return mk_err();
        }
        if (cr.value->u.b) return eval_expr(ctx, body, env);
    }
    return eval_expr(ctx, else_v, env);
}

static exec_result_t
eval_try_else(exec_ctx_t *ctx, const cJSON *expr, env_t *env)
{
    const cJSON *try_v  = cJSON_GetObjectItemCaseSensitive(
        expr, "try");
    const cJSON *else_v = cJSON_GetObjectItemCaseSensitive(
        expr, "else");
    if (!cJSON_IsObject(try_v) || !cJSON_IsObject(else_v)) {
        exec_err(ctx->diag, "R155", "try_else",
                 "missing 'try' or 'else' expression");
        return mk_err();
    }
    /* try/else catches only the recoverable
     * per-invocation runtime codes — R101 (tool call failed),
     * R102 (timeout, reserved — not yet emitted; kept here for
     * forward-compatibility), R130 (retried-then-failed), and R161
     * (budget exceeded). Programming-bug-shape codes propagate through
     * try unchanged: this includes R155 (the structural/decode
     * catch-all), R110, R140, R141, R150–R157, etc.
     *
     * We snapshot the diag-record index before evaluating the try
     * arm, then walk only the new entries to find their R-codes. If
     * every new error is in the recoverable set, swallow them (the
     * else arm is taking over) and evaluate else. Otherwise the
     * error is unrecoverable; propagate the failure. */
    size_t recorded_before = diag_count(ctx->diag);
    exec_result_t r = eval_expr(ctx, try_v, env);
    if (r.status == EXEC_OK) return r;

    size_t recorded_after = diag_count(ctx->diag);
    bool all_recoverable = recorded_after > recorded_before;
    for (size_t i = recorded_before; i < recorded_after; i++) {
        const Diagnostic *d = diag_at(ctx->diag, i);
        if (d == NULL || d->severity != DIAG_ERROR) continue;
        const char *id = d->id ? d->id : "";
        bool is_recoverable =
            strcmp(id, "R101") == 0 ||   /* tool call failed            */
            strcmp(id, "R102") == 0 ||   /* timeout (reserved; see note)*/
            strcmp(id, "R130") == 0 ||   /* retried, then failed        */
            strcmp(id, "R161") == 0;     /* budget exceeded             */
        if (!is_recoverable) { all_recoverable = false; break; }
    }
    if (!all_recoverable) {
        /* Let the unrecoverable error propagate; leave diags in place. */
        return r;
    }
    /* Recoverable: emit a note pointing at the fallback, then run else. */
    diag_note(ctx->diag, SRCLOC_NONE,
              "try/else: caught %zu recoverable error(s); "
              "falling through to else arm",
              recorded_after - recorded_before);
    return eval_expr(ctx, else_v, env);
}


/* ====================================================================
 * eval_call (tool dispatch)
 *
 * Look up the tool in the IR, find the host-registered implementation,
 * build a JSON args object, invoke, parse the returned JSON against
 * the tool's declared output type, return the resulting value_t.
 *
 * If the tool has no registered impl, that's R152.
 * ==================================================================== */

static const tool_impl_t *
find_impl(const flowd_runtime *rt, const char *name, bool want_model)
{
    for (size_t i = 0; i < rt->n_impls; i++) {
        if (strcmp(rt->impls[i].name, name) != 0) continue;
        if ((want_model ? rt->impls[i].model_fn : rt->impls[i].fn) != NULL) {
            return &rt->impls[i];
        }
    }
    return NULL;
}

/* Build a JSON object string for a tool's args. Each arg in the IR is
 * {"field":"name","value":<expr>}; we evaluate the value, canonicalize
 * to its JSON form, and assemble {name1: val1, name2: val2, ...}. */
static char *
build_tool_args_json(exec_ctx_t *ctx, const cJSON *args_arr, env_t *env)
{
    int n = cJSON_GetArraySize(args_arr);
    char  *buf = NULL;
    size_t sz  = 0;
    FILE *fp = open_memstream(&buf, &sz);
    if (fp == NULL) {
        exec_err(ctx->diag, "R155", "call",
                 "open_memstream failed");
        return NULL;
    }
    fputc('{', fp);
    for (int i = 0; i < n; i++) {
        cJSON *a = cJSON_GetArrayItem(args_arr, i);
        cJSON *f = cJSON_GetObjectItemCaseSensitive(a, "field");
        cJSON *v = cJSON_GetObjectItemCaseSensitive(a, "value");
        if (!cJSON_IsString(f) || !cJSON_IsObject(v)) {
            fclose(fp); free(buf);
            exec_err(ctx->diag, "R155", "call",
                     "arg %d malformed (missing field/value)", i);
            return NULL;
        }
        const char *fname = cJSON_GetStringValue(f);
        exec_result_t r = eval_expr(ctx, v, env);
        if (r.status != EXEC_OK) {
            fclose(fp); free(buf);
            return NULL;
        }
        if (i > 0) fputc(',', fp);
        value_emit_json_string(fp, fname);
        fputc(':', fp);
        value_canonical_serialize(r.value, fp);
    }
    fputc('}', fp);
    fclose(fp);
    return buf;
}

/* Replay/resume restore a recorded node's output by node id. Before
 * trusting that output, confirm the input we just re-derived matches
 * the input the original recorded for the same node. If an IR or
 * upstream change made this node receive a different input, restoring
 * the stale output would silently corrupt the run — callers fail with
 * R157 instead. Comparison is on JSON normalized through cJSON on both
 * sides, so writer/reader serializer differences don't read as
 * divergence. A node with no recorded inputs (older trace) cannot be
 * checked and is allowed, preserving backward compatibility. */
static bool
restore_inputs_match(trace_reader_t *orig, const char *node_id,
                     const char *args_json)
{
    char *rec = trace_reader_invocation_inputs(orig, node_id, 0u);
    if (rec == NULL) return true;
    bool same = false;
    cJSON *a = args_json ? cJSON_Parse(args_json) : NULL;
    if (a) {
        char *a_norm = cJSON_PrintUnformatted(a);
        if (a_norm) { same = (strcmp(a_norm, rec) == 0); free(a_norm); }
        cJSON_Delete(a);
    }
    free(rec);
    return same;
}

static exec_result_t
eval_call(exec_ctx_t *ctx, const cJSON *expr, env_t *env)
{
    const cJSON *tool_v = cJSON_GetObjectItemCaseSensitive(
        expr, "tool");
    const cJSON *args_v = cJSON_GetObjectItemCaseSensitive(
        expr, "args");
    if (!cJSON_IsString(tool_v) || !cJSON_IsArray(args_v)) {
        exec_err(ctx->diag, "R155", "call", "missing 'tool' or 'args'");
        return mk_err();
    }
    const char *name = cJSON_GetStringValue(tool_v);
    const tool_t *t = flowd_tool_by_name(ctx->rt, name);
    if (t == NULL) {
        exec_err(ctx->diag, "R155", "call",
                 "tool '%s' not declared in IR", name);
        return mk_err();
    }

    char *args_json = build_tool_args_json(ctx, args_v, env);
    if (args_json == NULL) return mk_err();

    /* Open a node record if tracing. node_kind is one of "suspension"
     * (only await_human_approval), "model_call", or "tool_call". */
    const char *node_id = NULL;
    if (ctx->trace) {
        const char *level_s =
            t->level == EFFECT_PURE          ? "pure"          :
            t->level == EFFECT_DETERMINISTIC ? "deterministic" :
            t->level == EFFECT_MODEL         ? "model"         :
                                               "mutation";
        const char *kind_s =
            (strcmp(name, "await_human_approval") == 0) ? "suspension"
          : (t->level == EFFECT_MODEL)                  ? "model_call"
          :                                               "tool_call";
        node_id = trace_writer_begin_node(ctx->trace, kind_s, level_s);
        /* Fast discriminator: of the three kind_s spellings ("suspension",
         * "model_call", "tool_call") only "model_call" begins with 'm'.  Add a
         * node kind starting with 'm' and this shortcut misroutes it -- compare
         * the full string instead. */
        if (kind_s[0] == 'm') {
            /* For model_call, the "tool" name is still useful, and
             * model identity is the model string. v1 doesn't yet
             * know a provider, so it's omitted. */
            trace_writer_set_model(ctx->trace, node_id, NULL,
                                   t->model_id ? t->model_id : name,
                                   NULL);
        } else {
            trace_writer_set_tool(ctx->trace, node_id, name, NULL);
        }
    }

    /* await_human_approval — the only suspending tool v1
     * ships built-in. On normal execution, this triggers EXEC_SUSPENDED;
     * on resume, the host has provided a decision_json that we inject
     * as this node's output (instead of suspending again). */
    if (strcmp(name, "await_human_approval") == 0) {
        if (ctx->resume && ctx->resume->suspended_node != NULL
            && node_id != NULL
            && strcmp(node_id, ctx->resume->suspended_node) == 0) {
            /* Resume path: inject the host's decision as the output. */
            cJSON *decision = cJSON_Parse(ctx->resume->decision_json);
            if (decision == NULL) {
                free(args_json);
                exec_err(ctx->diag, "R155", "call",
                         "resume: decision_json is not valid JSON");
                if (node_id) {
                    trace_writer_end_node(ctx->trace, node_id,
                                          "invalid decision JSON");
                }
                return mk_err();
            }
            value_t *v = value_from_json(ctx->arena, flowd_types(ctx->rt),
                                         t->output, decision, ctx->diag,
                                         "resume.decision");
            cJSON_Delete(decision);
            if (v == NULL) {
                free(args_json);
                if (node_id) {
                    trace_writer_end_node(ctx->trace, node_id,
                                          "decision failed type check");
                }
                return mk_err();
            }
            if (node_id) {
                trace_writer_invocation(ctx->trace, node_id, args_json, v);
                trace_writer_set_replay_of(ctx->trace, node_id,
                                           ctx->resume->original_dir,
                                           node_id, "re_invoked");
                trace_writer_end_node(ctx->trace, node_id, NULL);
            }
            free(args_json);
            return mk_ok(v);
        }
        /* Normal execution: trigger suspension. */
        if (ctx->suspension_out == NULL) {
            /* Can't suspend — no out-slot allocated by the caller. */
            free(args_json);
            exec_err(ctx->diag, "R155", "call",
                     "await_human_approval invoked without a "
                     "suspension slot (executor misconfiguration)");
            if (node_id) {
                trace_writer_end_node(ctx->trace, node_id,
                                      "suspension slot missing");
            }
            return mk_err();
        }
        /* Token is 12 hex chars + NUL, hence malloc(13) and [12]='\0'
         * below. FLOWD_SUSPENSION_TOKEN pins it for deterministic
         * tests; otherwise it's drawn from /dev/urandom, falling back
         * to a fixed "aaaa..." only if that read fails. */
        ctx->suspension_out->token = malloc(13u);
        if (!ctx->suspension_out->token) {
            free(args_json);
            FLOWD_ICE("suspension token alloc failed");
        }
        static const char hex[] = "0123456789abcdef";
        const char *pin = getenv("FLOWD_SUSPENSION_TOKEN");
        if (pin && strlen(pin) >= 12u) {
            memcpy(ctx->suspension_out->token, pin, 12);
        } else {
            unsigned char b[6];
            FILE *fp = fopen("/dev/urandom", "rb");
            if (fp && fread(b, 1, 6, fp) == 6) {
                fclose(fp);
                for (size_t i = 0; i < 6; i++) {
                    ctx->suspension_out->token[2u*i+0u] = hex[(b[i]>>4)&0xF];
                    ctx->suspension_out->token[2u*i+1u] = hex[b[i]&0xF];
                }
            } else {
                if (fp) fclose(fp);
                memcpy(ctx->suspension_out->token,
                       "aaaaaaaaaaaa", 12);
            }
        }
        ctx->suspension_out->token[12] = '\0';
        ctx->suspension_out->node_id   = arena_strdup(ctx->arena,
                                                      node_id ? node_id : "");
        ctx->suspension_out->condition = arena_strdup(ctx->arena,
                                                      "human_approval");
        ctx->suspension_out->args_json = arena_strdup(ctx->arena, args_json);
        if (node_id) {
            trace_writer_end_node(ctx->trace, node_id,
                                  "suspended:human_approval");
        }
        free(args_json);
        return (exec_result_t){ EXEC_SUSPENDED, NULL };
    }

    char *result_json = NULL;
    char *err_json    = NULL;

    /* Resume: nodes the suspended trace already recorded are fixed
     * history — restore them instead of re-invoking. This is essential
     * for mutations (no repeated side effects) and keeps deterministic/
     * model outputs byte-identical so node ids stay aligned with the
     * original. The suspended node itself was handled above; nodes the
     * original never recorded (post-suspension) fall through and run
     * live. trace_reader_invocation_output returns NULL for those. */
    char *resume_restored = NULL;
    if (ctx->resume && node_id != NULL &&
        (resume_restored = trace_reader_invocation_output(
             ctx->resume->original, node_id, 0u)) != NULL) {
        if (!restore_inputs_match(ctx->resume->original, node_id, args_json)) {
            free(resume_restored);
            free(args_json);
            trace_writer_end_node(ctx->trace, node_id,
                                  "R157 resume input divergence");
            exec_err(ctx->diag, "R157", "call",
                     "resume: re-derived input for node '%s' differs from "
                     "the recorded input; the suspended trace cannot be "
                     "continued against changed inputs", node_id);
            return mk_err();
        }
        result_json = resume_restored;
        trace_writer_set_replay_of(ctx->trace, node_id,
            ctx->resume->original_dir, node_id, "restored_from_trace");
    } else if (t->level == EFFECT_MODEL) {
        /* Replay mode: restore the recorded output (same-model) or
         * re-invoke against a swapped model_id (model-versioned). */
        if (ctx->replay && node_id != NULL) {
            const char *replay_model = ctx->replay->new_model_id
                                       ? ctx->replay->new_model_id
                                       : NULL;
            if (replay_model == NULL) {
                /* Same-model: restore from the original trace. */
                char *recorded = trace_reader_invocation_output(
                    ctx->replay->original, node_id, 0u);
                if (recorded != NULL) {
                    result_json = recorded;
                    if (node_id) {
                        trace_writer_set_model(ctx->trace, node_id,
                                               "replay",
                                               t->model_id, NULL);
                        trace_writer_set_replay_of(ctx->trace, node_id,
                            ctx->replay->original_dir, node_id,
                            "restored_from_trace");
                    }
                } else {
                    free(args_json);
                    if (node_id) {
                        trace_writer_end_node(ctx->trace, node_id,
                                              "R155 replay value unavailable");
                    }
                    exec_err(ctx->diag, "R155", "call",
                             "replay: no recorded output for node '%s'",
                             node_id);
                    return mk_err();
                }
            } else {
                /* Model-versioned: invoke replay_model against the
                 * recorded inputs of the original node. We honor the
                 * caller's args (already evaluated) because the
                 * executor is re-running deterministically — the args
                 * are byte-identical to the recorded inputs by
                 * construction. */
                if (ctx->rt->gateway) {
                    gateway_result_meta_t meta;
                    result_json = gateway_invoke(ctx->rt->gateway,
                                                 replay_model,
                                                 args_json,
                                                 &t->retry,
                                                 ctx->rt->has_budget
                                                   ? &ctx->rt->budget : NULL,
                                                 &ctx->rt->cancel_requested,
                                                 &err_json,
                                                 &meta);
                    if (node_id) {
                        const char *prov = (result_json && meta.provider_name)
                                           ? meta.provider_name : "replay";
                        trace_writer_set_model(ctx->trace, node_id,
                                               prov, replay_model, NULL);
                        if (result_json) {
                            trace_writer_set_model_metrics(ctx->trace, node_id,
                                meta.tokens_in, meta.tokens_out,
                                meta.cost_cents, meta.retry_attempts);
                        }
                        trace_writer_set_replay_of(ctx->trace, node_id,
                            ctx->replay->original_dir, node_id,
                            "re_invoked");
                    }
                }
            }
            /* result_json non-NULL → join the success path below.
             * NULL with err_json → join the failure path. */
            goto model_result_check;
        }

        /* Try the gateway first. If a provider adapter
         * claims the model, dispatch through it. The gateway records
         * the provider name on the trace node for replay attribution.
         * If no adapter matches AND a flowd_register_model fn is
         * available, fall back to direct dispatch — that path is the
         * direct-dispatch contract still supported for hosts that haven't
         * adopted the gateway. */
        bool dispatched = false;
        if (ctx->rt->gateway && t->model_id != NULL) {
            gateway_result_meta_t meta;
            result_json = gateway_invoke(ctx->rt->gateway,
                                         t->model_id,
                                         args_json,
                                         &t->retry,
                                         ctx->rt->has_budget
                                           ? &ctx->rt->budget : NULL,
                                         &ctx->rt->cancel_requested,
                                         &err_json,
                                         &meta);
            if (result_json != NULL) {
                dispatched = true;
                if (meta.provider_name && node_id) {
                    trace_writer_set_model(ctx->trace, node_id,
                                           meta.provider_name,
                                           t->model_id, NULL);
                }
                if (node_id) {
                    trace_writer_set_model_metrics(ctx->trace, node_id,
                        meta.tokens_in, meta.tokens_out, meta.cost_cents,
                        meta.retry_attempts);
                }
            } else if (err_json &&
                       strstr(err_json, "no provider adapter") != NULL) {
                /* Sentinel, not a failure: the gateway returns this exact
                 * phrase when NO adapter claimed the model.  Swallow it and
                 * leave dispatched=false so we fall back to the host impl
                 * table below.  Any other NULL err_json is a genuine model
                 * failure (dispatched=true).  This couples to the gateway's
                 * wording -- keep them in sync. */
                free(err_json);
                err_json = NULL;
            } else {
                dispatched = true;
            }
        }
        if (!dispatched) {
            const tool_impl_t *impl = find_impl(ctx->rt, name, true);
            if (impl == NULL) {
                free(args_json);
                if (node_id) {
                    trace_writer_end_node(ctx->trace, node_id,
                                          "R101 no model impl");
                }
                exec_err(ctx->diag, "R101", "call",
                         "no model implementation or provider adapter "
                         "registered for '%s'",
                         name);
                return mk_err();
            }
            result_json = impl->model_fn(args_json, &err_json,
                                         impl->user_ctx);
        }
model_result_check:;
    } else if (ctx->replay && node_id != NULL) {
        /* Replay restores non-model nodes from the recorded trace:
         * Level 0 (pure) and Level 1 (deterministic) are restored for
         * fidelity, and Level 3 (mutation) MUST NOT re-invoke (replay
         * must never repeat external side effects). Only Level 2 (model)
         * re-invokes, and only on a model-versioned replay (handled
         * above). No host impl is required for these during replay. */
        char *recorded = trace_reader_invocation_output(
            ctx->replay->original, node_id, 0u);
        if (recorded == NULL) {
            free(args_json);
            trace_writer_end_node(ctx->trace, node_id,
                                  "R155 replay value unavailable");
            exec_err(ctx->diag, "R155", "call",
                     "replay: no recorded output for node '%s'", node_id);
            return mk_err();
        }
        if (!restore_inputs_match(ctx->replay->original, node_id, args_json)) {
            free(recorded);
            free(args_json);
            trace_writer_end_node(ctx->trace, node_id,
                                  "R157 replay input divergence");
            exec_err(ctx->diag, "R157", "call",
                     "replay: re-derived input for node '%s' differs from "
                     "the recorded input; the trace cannot be replayed "
                     "against changed inputs", node_id);
            return mk_err();
        }
        result_json = recorded;
        trace_writer_set_replay_of(ctx->trace, node_id,
            ctx->replay->original_dir, node_id, "restored_from_trace");
    } else {
        const tool_impl_t *impl = find_impl(ctx->rt, name, false);
        if (impl == NULL) {
            free(args_json);
            if (node_id) {
                trace_writer_end_node(ctx->trace, node_id,
                                      "R101 no tool impl");
            }
            exec_err(ctx->diag, "R101", "call",
                     "no implementation registered for tool '%s'",
                     name);
            return mk_err();
        }
        result_json = impl->fn(args_json, &err_json, impl->user_ctx);
    }

    if (result_json == NULL) {
        if (ctx->rt->cancel_requested) {
            /* The model gateway's retry loop observed the cooperative
             * cancel flag and bailed (leaving err_json NULL). Report
             * cancellation, not a tool failure, mirroring run_flow's
             * binding-boundary checks. */
            if (node_id) {
                trace_writer_end_node(ctx->trace, node_id, "R160 cancelled");
            }
            free(args_json);
            free(err_json);
            exec_err(ctx->diag, "R160", "call", "execution cancelled");
            return mk_cancel();
        }
        if (node_id) {
            trace_writer_end_node(ctx->trace, node_id,
                err_json ? err_json : "tool returned NULL");
        }
        free(args_json);
        /* This block is reached by BOTH a non-model tool failure
         * (one-shot; no retry) and the model gateway path
         * (gateway_invoke returned NULL after exhausting its retry
         * policy). The gateway/budget layers prefix err_json with their
         * own R-code, which we preserve so the diag code matches the
         * node's error_msg and callers can distinguish failure shapes:
         *
         *   - "R161 ..."  budget overrun — a spent budget, not a tool
         *                 that genuinely errored.
         *   - "R130 ..."  model retried, then failed after N attempts.
         *
         * All three of R161, R130, R101 are in try/else's recoverable
         * set, so try/else catches this regardless of which path
         * produced it. A bare tool failure with no recognized prefix
         * falls through to R101. */
        if (err_json && strncmp(err_json, "R161", 4) == 0) {
            exec_err(ctx->diag, "R161", "call", "%s", err_json);
            free(err_json);
            return mk_err();
        }
        if (err_json && strncmp(err_json, "R130", 4) == 0) {
            exec_err(ctx->diag, "R130", "call", "%s", err_json);
            free(err_json);
            return mk_err();
        }
        exec_err(ctx->diag, "R101", "call",
                 "tool '%s' failed: %s",
                 name, err_json ? err_json : "(no error message)");
        free(err_json);
        return mk_err();
    }
    free(err_json);

    /* Parse result as the tool's declared output type. */
    cJSON *parsed = cJSON_Parse(result_json);
    if (parsed == NULL) {
        if (node_id) {
            trace_writer_end_node(ctx->trace, node_id,
                                  "tool returned invalid JSON");
        }
        free(args_json);
        free(result_json);
        /* Tool returned malformed bytes — host bug, not recoverable. */
        exec_err(ctx->diag, "R101", "call",
                 "tool '%s' returned invalid JSON", name);
        return mk_err();
    }
    value_t *out = value_from_json(ctx->arena, flowd_types(ctx->rt),
                                   t->output, parsed, ctx->diag,
                                   "call.result");
    cJSON_Delete(parsed);
    free(result_json);
    if (out == NULL) {
        if (node_id) {
            trace_writer_end_node(ctx->trace, node_id,
                                  "tool output failed type check");
        }
        free(args_json);
        return mk_err();
    }
    if (node_id) {
        trace_writer_invocation(ctx->trace, node_id, args_json, out);
        trace_writer_end_node(ctx->trace, node_id, NULL);
    }
    free(args_json);
    return mk_ok(out);
}


/* ====================================================================
 * eval_subflow_call
 *
 * Build a fresh env frame with the callee's params bound to the
 * caller-supplied arg values, run the callee's bindings and return.
 * Recursion is bounded by SUBFLOW_DEPTH_MAX (ctx->subflow_depth) so
 * that (possibly mutually) recursive composition can't blow the C
 * stack before the per-run arena's memory ceiling would stop it.
 * ==================================================================== */

static exec_result_t
run_flow(exec_ctx_t *ctx, const flow_t *flow, env_t *env);

static exec_result_t
eval_subflow_call(exec_ctx_t *ctx, const cJSON *expr, env_t *env)
{
    const cJSON *flow_v = cJSON_GetObjectItemCaseSensitive(
        expr, "flow");
    const cJSON *args_v = cJSON_GetObjectItemCaseSensitive(
        expr, "args");
    if (!cJSON_IsString(flow_v) || !cJSON_IsArray(args_v)) {
        exec_err(ctx->diag, "R155", "subflow_call",
                 "missing 'flow' or 'args'");
        return mk_err();
    }
    const char *name = cJSON_GetStringValue(flow_v);
    const flow_t *callee = flowd_flow_by_name(ctx->rt, name);
    if (callee == NULL) {
        exec_err(ctx->diag, "R155", "subflow_call",
                 "unknown flow '%s'", name);
        return mk_err();
    }

    const char *sf_node = NULL;
    if (ctx->trace) {
        sf_node = trace_writer_begin_node(ctx->trace, "subflow_call",
                                          "deterministic");
        trace_writer_set_subflow(ctx->trace, sf_node, name);
    }

    /* Evaluate each arg in the caller's env. */
    int n = cJSON_GetArraySize(args_v);
    value_t **vals = n > 0
        ? arena_alloc_zero(ctx->arena, (size_t)n * sizeof *vals)
        : NULL;
    const char **anames = n > 0
        ? arena_alloc_zero(ctx->arena, (size_t)n * sizeof *anames)
        : NULL;
    for (int i = 0; i < n; i++) {
        cJSON *a = cJSON_GetArrayItem(args_v, i);
        cJSON *f = cJSON_GetObjectItemCaseSensitive(a, "field");
        cJSON *v = cJSON_GetObjectItemCaseSensitive(a, "value");
        if (!cJSON_IsString(f) || !cJSON_IsObject(v)) {
            exec_err(ctx->diag, "R155", "subflow_call",
                     "arg %d malformed", i);
            if (sf_node) {
                trace_writer_end_node(ctx->trace, sf_node,
                                      "subflow arg malformed");
            }
            return mk_err();
        }
        exec_result_t r = eval_expr(ctx, v, env);
        if (r.status != EXEC_OK) {
            if (sf_node) {
                trace_writer_end_node(ctx->trace, sf_node,
                                      "subflow arg evaluation failed");
            }
            return r;
        }
        anames[i] = cJSON_GetStringValue(f);
        vals[i]   = r.value;
    }

    /* Push a fresh frame for the callee — no access to caller's
     * locals, only to its declared params. */
    env_frame_t *prior = env_push(env);
    for (size_t i = 0; i < callee->n_params; i++) {
        const char *want = callee->params[i].name;
        value_t *bound = NULL;
        for (int j = 0; j < n; j++) {
            if (strcmp(anames[j], want) == 0) {
                bound = vals[j];
                break;
            }
        }
        if (bound == NULL) {
            env_pop(env, prior);
            exec_err(ctx->diag, "R155", "subflow_call",
                     "missing arg '%s' to flow '%s'",
                     want, name);
            return mk_err();
        }
        env_bind(env, want, bound);
    }
    if (++ctx->subflow_depth > SUBFLOW_DEPTH_MAX) {
        ctx->subflow_depth--;
        env_pop(env, prior);
        exec_err(ctx->diag, "R155", "subflow_call",
                 "subflow recursion too deep (limit %d)", SUBFLOW_DEPTH_MAX);
        if (sf_node) {
            trace_writer_end_node(ctx->trace, sf_node,
                                  "subflow recursion limit exceeded");
        }
        return mk_err();
    }
    exec_result_t r = run_flow(ctx, callee, env);
    ctx->subflow_depth--;
    env_pop(env, prior);
    if (sf_node) {
        if (r.status == EXEC_OK) {
            trace_writer_invocation(ctx->trace, sf_node, NULL, r.value);
            trace_writer_end_node(ctx->trace, sf_node, NULL);
        } else {
            trace_writer_end_node(ctx->trace, sf_node,
                                  "subflow failed");
        }
    }
    return r;
}


/* ====================================================================
 * eval_match
 *
 * Patterns:
 *   {"kind":"variant", "variant": "Name", "binder": "x"}    -- variant w/ binder
 *   {"kind":"wildcard"}                                      -- always matches
 *   {"kind":"wildcard", "binder": "x"}                       -- match-all + bind
 *
 * For a variant pattern, the binder is bound to a synthetic record
 * value carrying the variant's payload fields, so the body can
 * access fields via path: `pattern x -> x.field`.
 * ==================================================================== */

static value_t *
variant_payload_as_record(Arena *a, const value_t *variant)
{
    /* type_id_t for the synthetic record is borrowed from the
     * containing variant's type id — there is no record type id for
     * an anonymous variant payload, and the canonical serializer never
     * exposes type_id externally. Caveat: value_equal (value.c) does
     * compare type ids, so this synthetic record (carrying the sum's
     * type id) will never compare == to a genuinely-constructed record
     * of the same structure. In practice the typechecker constrains
     * which values can be compared, so this binder record is not used
     * as an == operand against a real record. */
    value_field_t *fields = variant->u.variant.n > 0
        ? arena_alloc_zero(a, variant->u.variant.n * sizeof *fields)
        : NULL;
    for (size_t i = 0; i < variant->u.variant.n; i++) {
        fields[i].name  = arena_strdup(a,
            variant->u.variant.fields[i].name);
        fields[i].value = variant->u.variant.fields[i].value;
    }
    return value_new_record_take(a, variant->type, fields,
                                 variant->u.variant.n);
}

static exec_result_t
eval_match(exec_ctx_t *ctx, const cJSON *expr, env_t *env)
{
    const cJSON *scrut_v = cJSON_GetObjectItemCaseSensitive(
        expr, "scrutinee");
    const cJSON *arms_v  = cJSON_GetObjectItemCaseSensitive(
        expr, "arms");
    if (!cJSON_IsObject(scrut_v) || !cJSON_IsArray(arms_v)) {
        exec_err(ctx->diag, "R155", "match",
                 "missing 'scrutinee' or 'arms'");
        return mk_err();
    }
    exec_result_t sr = eval_expr(ctx, scrut_v, env);
    if (sr.status != EXEC_OK) return sr;

    int n = cJSON_GetArraySize(arms_v);
    for (int i = 0; i < n; i++) {
        cJSON *arm = cJSON_GetArrayItem(arms_v, i);
        cJSON *pat = cJSON_GetObjectItemCaseSensitive(arm, "pattern");
        cJSON *body = cJSON_GetObjectItemCaseSensitive(arm, "body");
        if (!cJSON_IsObject(pat) || !cJSON_IsObject(body)) {
            exec_err(ctx->diag, "R155", "match", "arm %d malformed", i);
            return mk_err();
        }
        cJSON *pkind = cJSON_GetObjectItemCaseSensitive(pat, "kind");
        if (!cJSON_IsString(pkind)) {
            exec_err(ctx->diag, "R155", "match",
                     "arm %d pattern missing 'kind'", i);
            return mk_err();
        }
        const char *pk = cJSON_GetStringValue(pkind);
        cJSON *binder_v = cJSON_GetObjectItemCaseSensitive(
            pat, "binder");
        const char *binder = (binder_v && cJSON_IsString(binder_v))
            ? cJSON_GetStringValue(binder_v) : NULL;

        if (strcmp(pk, "variant") == 0) {
            cJSON *vname = cJSON_GetObjectItemCaseSensitive(
                pat, "variant");
            if (!cJSON_IsString(vname)) {
                exec_err(ctx->diag, "R155", "match",
                         "variant pattern missing 'variant'");
                return mk_err();
            }
            if (sr.value->kind != VAL_VARIANT) {
                exec_err(ctx->diag, "R155", "match",
                         "scrutinee is not a variant");
                return mk_err();
            }
            if (strcmp(sr.value->u.variant.variant_name,
                       cJSON_GetStringValue(vname)) != 0) {
                continue;
            }
            env_frame_t *prior = env_push(env);
            if (binder) {
                value_t *rec = variant_payload_as_record(ctx->arena,
                                                         sr.value);
                env_bind(env, binder, rec);
            }
            exec_result_t r = eval_expr(ctx, body, env);
            env_pop(env, prior);
            return r;
        }
        if (strcmp(pk, "wildcard") == 0) {
            env_frame_t *prior = env_push(env);
            if (binder) env_bind(env, binder, sr.value);
            exec_result_t r = eval_expr(ctx, body, env);
            env_pop(env, prior);
            return r;
        }
        exec_err(ctx->diag, "R155", "match",
                 "unknown pattern kind '%s' (arm %d)", pk, i);
        return mk_err();
    }
    exec_err(ctx->diag, "R155", "match",
             "no arm matched (exhaustiveness should have been "
             "enforced by the checker)");
    return mk_err();
}


/* ====================================================================
 * Pipelines
 *
 * Each non-terminal stage transforms a list of value_t* into another.
 * Terminal stages reduce to a scalar value.
 * ==================================================================== */

static exec_result_t
stage_where(exec_ctx_t *ctx, const cJSON *stage, value_t **rows, size_t n,
            value_t ***out_rows, size_t *out_n)
{
    const cJSON *pred_v = cJSON_GetObjectItemCaseSensitive(
        stage, "predicate");
    if (!cJSON_IsString(pred_v)) {
        exec_err(ctx->diag, "R155", "where",
                 "expected string predicate (flowc emits "
                 "where predicates as strings)");
        return mk_err();
    }
    const char *pred = cJSON_GetStringValue(pred_v);

    value_t **buf = arena_alloc_zero(ctx->arena, n * sizeof *buf);
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        bool ok;
        bool keep = where_predicate_eval(pred, rows[i],
                                         ctx->arena, ctx->diag, &ok);
        if (!ok) return mk_err();
        if (keep) buf[kept++] = rows[i];
    }
    *out_rows = buf;
    *out_n    = kept;
    return mk_ok(NULL);
}

static exec_result_t
stage_select(exec_ctx_t *ctx, const cJSON *stage, env_t *env,
             value_t **rows, size_t n,
             value_t ***out_rows, size_t *out_n)
{
    const cJSON *body = cJSON_GetObjectItemCaseSensitive(
        stage, "body");
    if (!cJSON_IsObject(body)) {
        exec_err(ctx->diag, "R155", "select", "missing 'body'");
        return mk_err();
    }
    value_t **buf = arena_alloc_zero(ctx->arena, n * sizeof *buf);
    for (size_t i = 0; i < n; i++) {
        env_frame_t *prior = env_push(env);
        env_bind(env, "row", rows[i]);
        /* Expose each field as an implicit binding so a bare field name
         * (`select cost`) resolves, matching `where`/`rank`/`any`/`all`
         * and the implicit-it rule. */
        if (rows[i]->kind == VAL_RECORD) {
            for (size_t k = 0; k < rows[i]->u.record.n; k++) {
                env_bind(env, rows[i]->u.record.fields[k].name,
                              rows[i]->u.record.fields[k].value);
            }
        }
        exec_result_t r = eval_expr(ctx, body, env);
        env_pop(env, prior);
        if (r.status != EXEC_OK) return r;
        buf[i] = r.value;
    }
    *out_rows = buf;
    *out_n    = n;
    return mk_ok(NULL);
}

/* dedupe by <expr> — compute key per row, keep first occurrence of
 * each key. Equality on keys uses value_equal. */
static exec_result_t
stage_dedupe(exec_ctx_t *ctx, const cJSON *stage, env_t *env,
             value_t **rows, size_t n,
             value_t ***out_rows, size_t *out_n)
{
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(
        stage, "key");
    if (!cJSON_IsObject(key)) {
        exec_err(ctx->diag, "R155", "dedupe", "missing 'key'");
        return mk_err();
    }
    value_t **buf  = arena_alloc_zero(ctx->arena, n * sizeof *buf);
    value_t **keys = arena_alloc_zero(ctx->arena, n * sizeof *keys);
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        env_frame_t *prior = env_push(env);
        env_bind(env, "row", rows[i]);
        /* Bare field names (`dedupe by id`) resolve as implicit bindings,
         * matching `where`/`rank`/`any`/`all`. */
        if (rows[i]->kind == VAL_RECORD) {
            for (size_t k = 0; k < rows[i]->u.record.n; k++) {
                env_bind(env, rows[i]->u.record.fields[k].name,
                              rows[i]->u.record.fields[k].value);
            }
        }
        exec_result_t r = eval_expr(ctx, key, env);
        env_pop(env, prior);
        if (r.status != EXEC_OK) return r;
        bool seen = false;
        for (size_t k = 0; k < kept; k++) {
            if (value_equal(keys[k], r.value)) { seen = true; break; }
        }
        if (!seen) { buf[kept] = rows[i]; keys[kept] = r.value; kept++; }
    }
    *out_rows = buf;
    *out_n    = kept;
    return mk_ok(NULL);
}

static exec_result_t
stage_concat(exec_ctx_t *ctx, const cJSON *stage, env_t *env,
             value_t **rows, size_t n,
             value_t ***out_rows, size_t *out_n)
{
    const cJSON *other = cJSON_GetObjectItemCaseSensitive(
        stage, "other");
    if (!cJSON_IsObject(other)) {
        exec_err(ctx->diag, "R155", "concat", "missing 'other'");
        return mk_err();
    }
    exec_result_t r = eval_expr(ctx, other, env);
    if (r.status != EXEC_OK) return r;
    if (r.value->kind != VAL_LIST) {
        exec_err(ctx->diag, "R155", "concat",
                 "concat operand is not a list");
        return mk_err();
    }
    size_t m = r.value->u.list.n;
    value_t **buf = arena_alloc_zero(ctx->arena, (n + m) * sizeof *buf);
    for (size_t i = 0; i < n; i++) buf[i] = rows[i];
    for (size_t i = 0; i < m; i++) buf[n + i] = r.value->u.list.items[i];
    *out_rows = buf;
    *out_n    = n + m;
    return mk_ok(NULL);
}

static exec_result_t
stage_take(exec_ctx_t *ctx, const cJSON *stage,
           value_t **rows, size_t n,
           value_t ***out_rows, size_t *out_n)
{
    const cJSON *cnt = cJSON_GetObjectItemCaseSensitive(
        stage, "count");
    if (!cJSON_IsNumber(cnt)) {
        exec_err(ctx->diag, "R155", "take", "missing 'count'");
        return mk_err();
    }
    bool ok; int64_t c = json_number_to_int(cnt, &ok);
    if (!ok || c < 0) {
        exec_err(ctx->diag, "R155", "take",
                 "take count must be a non-negative integer");
        return mk_err();
    }
    size_t kept = (size_t)c < n ? (size_t)c : n;
    *out_rows = rows;
    *out_n    = kept;
    return mk_ok(NULL);
}

/* Comparator state for rank. `err` is a side channel: a qsort
 * comparator can't return a failure, so a missing/unorderable field is
 * flagged here and checked once after qsort returns. */
typedef struct {
    const char *field;
    bool        ascending;
    bool        err;
} rank_ctx_t;

static int
rank_cmp_field(const value_t *a, const value_t *b,
               rank_ctx_t *rc)
{
    if (a->kind != VAL_RECORD || b->kind != VAL_RECORD) {
        rc->err = true;
        return 0;
    }
    const value_t *va = NULL, *vb = NULL;
    for (size_t i = 0; i < a->u.record.n; i++) {
        if (strcmp(a->u.record.fields[i].name, rc->field) == 0) {
            va = a->u.record.fields[i].value; break;
        }
    }
    for (size_t i = 0; i < b->u.record.n; i++) {
        if (strcmp(b->u.record.fields[i].name, rc->field) == 0) {
            vb = b->u.record.fields[i].value; break;
        }
    }
    if (va == NULL || vb == NULL) {
        rc->err = true; return 0;
    }
    if (va->kind == VAL_INT && vb->kind == VAL_INT) {
        if (va->u.i < vb->u.i) return rc->ascending ? -1 :  1;
        if (va->u.i > vb->u.i) return rc->ascending ?  1 : -1;
        return 0;
    }
    if (va->kind == VAL_FLOAT && vb->kind == VAL_FLOAT) {
        if (va->u.f < vb->u.f) return rc->ascending ? -1 :  1;
        if (va->u.f > vb->u.f) return rc->ascending ?  1 : -1;
        return 0;
    }
    if (va->kind == VAL_STRING && vb->kind == VAL_STRING) {
        int c = strcmp(va->u.s, vb->u.s);
        return rc->ascending ? c : -c;
    }
    rc->err = true;
    return 0;
}

/* qsort callback bouncing through a file-scope context pointer.
 * Portable qsort gives the comparator no context arg, so we stash the
 * rank context in this process-global static, set it before the call
 * and clear it after.
 *
 * THREAD-SAFETY: this static is shared by every thread in the process,
 * not scoped to a flowd_runtime handle. That matches — and reinforces —
 * the library's reentrancy contract (see flowd.h): flowd is NOT safe to
 * use concurrently across threads even on distinct runtimes, precisely
 * because unsynchronized process-global state like this (and the
 * last-error slot read by flowd_last_error_json) would race. Two threads
 * both reaching a rank stage at once would clobber each other's
 * g_rank_ctx (wrong field/direction, rc->err leaking across sorts). We
 * deliberately add no threading primitives here to stay -std=c99 /
 * portable; callers that need concurrency must serialize all library
 * calls process-wide, as that contract already requires. Lifting this
 * one site to per-handle concurrency would need qsort_r (BSD/glibc) /
 * qsort_s (C11 Annex K), neither of which is C99. */
static rank_ctx_t *g_rank_ctx = NULL;

static int
rank_cmp_qsort(const void *a, const void *b)
{
    const value_t *va = *(const value_t *const *)a;
    const value_t *vb = *(const value_t *const *)b;
    return rank_cmp_field(va, vb, g_rank_ctx);
}

static exec_result_t
stage_rank(exec_ctx_t *ctx, const cJSON *stage,
           value_t **rows, size_t n,
           value_t ***out_rows, size_t *out_n)
{
    const cJSON *by  = cJSON_GetObjectItemCaseSensitive(
        stage, "by");
    const cJSON *dir = cJSON_GetObjectItemCaseSensitive(
        stage, "direction");
    if (!cJSON_IsString(by) || !cJSON_IsString(dir)) {
        exec_err(ctx->diag, "R155", "rank", "missing 'by' or 'direction'");
        return mk_err();
    }
    rank_ctx_t rc;
    rc.field     = cJSON_GetStringValue(by);
    rc.ascending = strcmp(cJSON_GetStringValue(dir), "asc") == 0;
    rc.err       = false;

    value_t **buf = arena_alloc(ctx->arena, n * sizeof *buf);
    memcpy(buf, rows, n * sizeof *buf);
    g_rank_ctx = &rc;
    qsort(buf, n, sizeof *buf, rank_cmp_qsort);
    g_rank_ctx = NULL;
    if (rc.err) {
        exec_err(ctx->diag, "R155", "rank",
                 "field '%s' missing or not orderable", rc.field);
        return mk_err();
    }
    *out_rows = buf;
    *out_n    = n;
    return mk_ok(NULL);
}

/* Terminal: count, sum field, max field, min field, any pred, all
 * pred, top, pick using model. */
static exec_result_t
stage_terminal(exec_ctx_t *ctx, const cJSON *stage, env_t *env,
               value_t **rows, size_t n)
{
    const cJSON *op_v = cJSON_GetObjectItemCaseSensitive(
        stage, "op");
    if (!cJSON_IsString(op_v)) {
        exec_err(ctx->diag, "R155", "terminal", "missing 'op'");
        return mk_err();
    }
    const char *op = cJSON_GetStringValue(op_v);

    if (strcmp(op, "count") == 0) {
        return mk_ok(value_new_int(ctx->arena, TYPE_ID_INT, (int64_t)n));
    }

    if (strcmp(op, "top") == 0) {
        if (n == 0) {
            exec_err(ctx->diag, "R110", "top", "top of empty list");
            return mk_err();
        }
        return mk_ok(rows[0]);
    }

    if (strcmp(op, "any") == 0 || strcmp(op, "all") == 0) {
        const cJSON *pred = cJSON_GetObjectItemCaseSensitive(
            stage, "predicate");
        if (!cJSON_IsObject(pred)) {
            exec_err(ctx->diag, "R155", op, "missing 'predicate'");
            return mk_err();
        }
        bool want_any = (strcmp(op, "any") == 0);
        for (size_t i = 0; i < n; i++) {
            env_frame_t *prior = env_push(env);
            env_bind(env, "row", rows[i]);
            /* Also expose each field of the row as an implicit binding,
             * since the implicit-it rule applies inside any/all
             * predicates too (the typechecker accepts both forms). */
            if (rows[i]->kind == VAL_RECORD) {
                for (size_t k = 0; k < rows[i]->u.record.n; k++) {
                    env_bind(env, rows[i]->u.record.fields[k].name,
                                  rows[i]->u.record.fields[k].value);
                }
            }
            exec_result_t r = eval_expr(ctx, pred, env);
            env_pop(env, prior);
            if (r.status != EXEC_OK) return r;
            if (r.value->kind != VAL_BOOL) {
                exec_err(ctx->diag, "R155", op,
                         "predicate must produce a bool");
                return mk_err();
            }
            if (want_any && r.value->u.b)
                return mk_ok(value_new_bool(ctx->arena, TYPE_ID_BOOL, true));
            if (!want_any && !r.value->u.b)
                return mk_ok(value_new_bool(ctx->arena, TYPE_ID_BOOL, false));
        }
        /* Vacuous: any → false, all → true. */
        return mk_ok(value_new_bool(ctx->arena, TYPE_ID_BOOL, !want_any));
    }

    if (strcmp(op, "sum") == 0 || strcmp(op, "max") == 0
                              || strcmp(op, "min") == 0) {
        const cJSON *field = cJSON_GetObjectItemCaseSensitive(
            stage, "field");
        if (!cJSON_IsString(field)) {
            exec_err(ctx->diag, "R155", op, "missing 'field'");
            return mk_err();
        }
        const char *fname = cJSON_GetStringValue(field);
        if (n == 0) {
            if (strcmp(op, "sum") == 0)
                return mk_ok(value_new_int(ctx->arena, TYPE_ID_INT, 0));
            /* R141 covers max/min on empty. */
            exec_err(ctx->diag, "R141", op,
                     "%s of empty list (cannot determine value)",
                     op);
            return mk_err();
        }
        bool is_int = true;
        for (size_t i = 0; i < n; i++) {
            if (rows[i]->kind != VAL_RECORD) {
                exec_err(ctx->diag, "R155", op,
                         "row %zu is not a record", i);
                return mk_err();
            }
            const value_t *fv = NULL;
            for (size_t k = 0; k < rows[i]->u.record.n; k++) {
                if (strcmp(rows[i]->u.record.fields[k].name, fname) == 0) {
                    fv = rows[i]->u.record.fields[k].value;
                    break;
                }
            }
            if (fv == NULL) {
                exec_err(ctx->diag, "R155", op,
                         "row %zu lacks field '%s'", i, fname);
                return mk_err();
            }
            if (fv->kind != VAL_INT && fv->kind != VAL_FLOAT) {
                exec_err(ctx->diag, "R155", op,
                         "field '%s' is not numeric", fname);
                return mk_err();
            }
            if (fv->kind == VAL_FLOAT) is_int = false;
        }
        if (is_int) {
            int64_t acc = 0;
            if (strcmp(op, "sum") == 0) acc = 0;
            /* Seed is a throwaway: fields[0] is whatever field is declared
             * first, not `fname`, and its union member may not even be int --
             * but the `first` flag below overwrites acc with the real fname
             * value on iteration 0 before any max/min compare reads it.  Don't
             * "fix" this to look up fname here; the second pass already does. */
            else                        acc = rows[0]->u.record.fields[0].value->u.i;
            bool first = true;
            for (size_t i = 0; i < n; i++) {
                const value_t *fv = NULL;
                for (size_t k = 0; k < rows[i]->u.record.n; k++) {
                    if (strcmp(rows[i]->u.record.fields[k].name, fname) == 0) {
                        fv = rows[i]->u.record.fields[k].value; break;
                    }
                }
                int64_t x = fv->u.i;
                /* Signed int64 overflow is UB; detect it rather than wrap. */
                if (strcmp(op, "sum") == 0) {
                    if (__builtin_add_overflow(acc, x, &acc)) { exec_err(ctx->diag, "R140", op, "int arithmetic overflow"); return mk_err(); }
                }
                else if (first) { acc = x; first = false; }
                else if (strcmp(op, "max") == 0 && x > acc) acc = x;
                else if (strcmp(op, "min") == 0 && x < acc) acc = x;
            }
            return mk_ok(value_new_int(ctx->arena, TYPE_ID_INT, acc));
        }
        double facc = 0;
        bool first = true;
        for (size_t i = 0; i < n; i++) {
            const value_t *fv = NULL;
            for (size_t k = 0; k < rows[i]->u.record.n; k++) {
                if (strcmp(rows[i]->u.record.fields[k].name, fname) == 0) {
                    fv = rows[i]->u.record.fields[k].value; break;
                }
            }
            double x = (fv->kind == VAL_INT) ? (double)fv->u.i : fv->u.f;
            if (strcmp(op, "sum") == 0) facc += x;
            else if (first) { facc = x; first = false; }
            else if (strcmp(op, "max") == 0 && x > facc) facc = x;
            else if (strcmp(op, "min") == 0 && x < facc) facc = x;
        }
        return mk_ok(value_new_float(ctx->arena, TYPE_ID_FLOAT, facc));
    }

    if (strcmp(op, "pick") == 0) {
        /* pick using <model>.
         *
         * Contract: the named model receives a request of shape
         *   {"candidates": [<row0>, <row1>, ...]}
         * and returns
         *   {"index": N}
         * where N is the zero-based index of the chosen row. The
         * gateway routes by model_id; the returned index selects
         * from the current rows[] without revalidating the chosen
         * value's type (the typechecker proved the rows are uniform
         * and `pick` returns one row of that same type).
         *
         * R110 fires on empty input (matches `top` terminal). R155
         * on a model that doesn't return a valid index. */
        const cJSON *model_v = cJSON_GetObjectItemCaseSensitive(
            stage, "model");
        if (!cJSON_IsString(model_v)) {
            exec_err(ctx->diag, "R155", "pick", "missing 'model' field");
            return mk_err();
        }
        if (n == 0) {
            exec_err(ctx->diag, "R110", "pick",
                     "pick on empty list");
            return mk_err();
        }
        const char *model_id = cJSON_GetStringValue(model_v);

        char  *req_buf = NULL;
        size_t req_sz  = 0;
        FILE *fp = open_memstream(&req_buf, &req_sz);
        if (!fp) {
            exec_err(ctx->diag, "R155", "pick", "memstream failed");
            return mk_err();
        }
        fputs("{\"candidates\":[", fp);
        for (size_t i = 0; i < n; i++) {
            if (i > 0) fputc(',', fp);
            value_canonical_serialize(rows[i], fp);
        }
        fputs("]}", fp);
        fclose(fp);

        if (!ctx->rt->gateway) {
            free(req_buf);
            exec_err(ctx->diag, "R101", "pick",
                     "gateway not initialized");
            return mk_err();
        }
        char *err_json = NULL;
        gateway_result_meta_t meta;
        char *response = gateway_invoke(ctx->rt->gateway, model_id,
                                        req_buf, NULL,
                                        ctx->rt->has_budget
                                          ? &ctx->rt->budget : NULL,
                                        &ctx->rt->cancel_requested,
                                        &err_json, &meta);
        free(req_buf);
        if (response == NULL) {
            if (ctx->rt->cancel_requested) {
                free(err_json);
                exec_err(ctx->diag, "R160", "pick", "execution cancelled");
                return mk_cancel();
            }
            exec_err(ctx->diag, "R101", "pick",
                     "model '%s' failed: %s",
                     model_id,
                     err_json ? err_json : "(no error)");
            free(err_json);
            return mk_err();
        }
        free(err_json);

        cJSON *parsed = cJSON_Parse(response);
        free(response);
        if (!parsed) {
            exec_err(ctx->diag, "R155", "pick",
                     "model '%s' returned invalid JSON", model_id);
            return mk_err();
        }
        cJSON *idx_v = cJSON_GetObjectItemCaseSensitive(parsed, "index");
        if (!cJSON_IsNumber(idx_v)) {
            cJSON_Delete(parsed);
            exec_err(ctx->diag, "R155", "pick",
                     "model '%s' response missing integer 'index'",
                     model_id);
            return mk_err();
        }
        int64_t idx = (int64_t)idx_v->valuedouble;
        cJSON_Delete(parsed);
        if (idx < 0 || (size_t)idx >= n) {
            exec_err(ctx->diag, "R155", "pick",
                     "model '%s' returned out-of-range index %" PRId64
                     " (n=%zu)", model_id, idx, n);
            return mk_err();
        }
        return mk_ok(rows[idx]);
    }

    exec_err(ctx->diag, "R155", "terminal", "unknown terminal op '%s'", op);
    return mk_err();
}

static exec_result_t
eval_pipeline(exec_ctx_t *ctx, const cJSON *expr, env_t *env)
{
    const cJSON *source_v = cJSON_GetObjectItemCaseSensitive(
        expr, "source");
    const cJSON *stages_v = cJSON_GetObjectItemCaseSensitive(
        expr, "stages");
    if (!cJSON_IsObject(source_v) || !cJSON_IsArray(stages_v)) {
        exec_err(ctx->diag, "R155", "pipeline",
                 "missing 'source' or 'stages'");
        return mk_err();
    }
    exec_result_t sr = eval_expr(ctx, source_v, env);
    if (sr.status != EXEC_OK) return sr;
    if (sr.value->kind != VAL_LIST) {
        exec_err(ctx->diag, "R155", "pipeline",
                 "pipeline source is not a list");
        return mk_err();
    }
    value_t **rows = sr.value->u.list.items;
    size_t    n    = sr.value->u.list.n;
    /* type_registry_get can return NULL for an unknown id, and only a
     * list type has an `elem` field; guard before dereferencing. */
    const type_t *st = type_registry_get(flowd_types(ctx->rt),
                                          sr.value->type);
    if (!st || st->kind != TYPE_KIND_LIST) {
        exec_err(ctx->diag, "R155", "pipeline",
                 "pipeline source is not a list");
        return mk_err();
    }
    type_id_t elem_type = st->elem;

    int ns = cJSON_GetArraySize(stages_v);
    for (int i = 0; i < ns; i++) {
        cJSON *stage = cJSON_GetArrayItem(stages_v, i);
        cJSON *kind_v = cJSON_GetObjectItemCaseSensitive(stage, "kind");
        if (!cJSON_IsString(kind_v)) {
            exec_err(ctx->diag, "R155", "pipeline",
                     "stage %d missing 'kind'", i);
            return mk_err();
        }
        const char *sk = cJSON_GetStringValue(kind_v);
        value_t **out_rows = NULL;
        size_t out_n = 0;

        if      (strcmp(sk, "filter") == 0) {
            exec_result_t r = stage_where(ctx, stage, rows, n,
                                          &out_rows, &out_n);
            if (r.status != EXEC_OK) return r;
        }
        else if (strcmp(sk, "select") == 0) {
            exec_result_t r = stage_select(ctx, stage, env, rows, n,
                                           &out_rows, &out_n);
            if (r.status != EXEC_OK) return r;
            /* select replaces the element type; recompute. */
            if (out_n > 0) {
                elem_type = out_rows[0]->type;
            }
        }
        else if (strcmp(sk, "dedupe") == 0) {
            exec_result_t r = stage_dedupe(ctx, stage, env, rows, n,
                                           &out_rows, &out_n);
            if (r.status != EXEC_OK) return r;
        }
        else if (strcmp(sk, "concat") == 0) {
            exec_result_t r = stage_concat(ctx, stage, env, rows, n,
                                           &out_rows, &out_n);
            if (r.status != EXEC_OK) return r;
        }
        else if (strcmp(sk, "take") == 0) {
            exec_result_t r = stage_take(ctx, stage, rows, n,
                                         &out_rows, &out_n);
            if (r.status != EXEC_OK) return r;
        }
        else if (strcmp(sk, "sort") == 0) {
            exec_result_t r = stage_rank(ctx, stage, rows, n,
                                         &out_rows, &out_n);
            if (r.status != EXEC_OK) return r;
        }
        else if (strcmp(sk, "terminal") == 0) {
            /* Terminal must be the last stage; produces a scalar. */
            if (i != ns - 1) {
                exec_err(ctx->diag, "R155", "pipeline",
                         "terminal stage must be last");
                return mk_err();
            }
            return stage_terminal(ctx, stage, env, rows, n);
        }
        else {
            exec_err(ctx->diag, "R155", "pipeline",
                     "unknown stage kind '%s'", sk);
            return mk_err();
        }
        rows = out_rows;
        n    = out_n;
    }
    /* No terminal: produce a list. */
    return mk_ok(value_new_list_take(ctx->arena,
                                     type_registry_intern_list(
                                         (type_registry_t *)flowd_types(ctx->rt),
                                         elem_type),
                                     rows, n));
}


/* ====================================================================
 * Top-level dispatch
 * ==================================================================== */

static exec_result_t
eval_expr(exec_ctx_t *ctx, const cJSON *expr, env_t *env)
{
    if (!cJSON_IsObject(expr)) {
        exec_err(ctx->diag, "R155", "expr", "expected an expression object");
        return mk_err();
    }
    cJSON *kind_v = cJSON_GetObjectItemCaseSensitive(expr, "kind");
    if (!cJSON_IsString(kind_v)) {
        exec_err(ctx->diag, "R155", "expr", "missing 'kind' on expression");
        return mk_err();
    }
    const char *kind = cJSON_GetStringValue(kind_v);

    if (strcmp(kind, "literal")           == 0) return eval_literal          (ctx, expr);
    if (strcmp(kind, "path")              == 0) return eval_path             (ctx, expr, env);
    if (strcmp(kind, "construct")         == 0) return eval_construct        (ctx, expr, env);
    if (strcmp(kind, "construct_variant") == 0) return eval_construct_variant(ctx, expr, env);
    if (strcmp(kind, "list_literal")      == 0) return eval_list_literal     (ctx, expr, env);
    if (strcmp(kind, "binop")             == 0) return eval_binop            (ctx, expr, env);
    if (strcmp(kind, "unop")              == 0) return eval_unop             (ctx, expr, env);
    if (strcmp(kind, "conditional")       == 0) return eval_conditional      (ctx, expr, env);
    if (strcmp(kind, "try_else")          == 0) return eval_try_else         (ctx, expr, env);
    if (strcmp(kind, "call")              == 0) return eval_call             (ctx, expr, env);
    if (strcmp(kind, "subflow_call")      == 0) return eval_subflow_call     (ctx, expr, env);
    if (strcmp(kind, "match")             == 0) return eval_match            (ctx, expr, env);
    if (strcmp(kind, "pipeline")          == 0) return eval_pipeline         (ctx, expr, env);

    exec_err(ctx->diag, "R155", "expr",
             "expression kind '%s' is not implemented", kind);
    return mk_err();
}


/* ====================================================================
 * Flow body execution
 *
 * Walks the bindings array in order; each binding extends the env.
 * Then evaluates the return expression. Used both by the top-level
 * flowd_run_impl and by subflow_call.
 * ==================================================================== */

static exec_result_t
run_flow(exec_ctx_t *ctx, const flow_t *flow, env_t *env)
{
    if (flow->bindings) {
        int nb = cJSON_GetArraySize(flow->bindings);
        for (int i = 0; i < nb; i++) {
            /* Cooperative cancellation: checked at each binding boundary. */
            if (ctx->rt->cancel_requested) {
                exec_err(ctx->diag, "R160", NULL, "execution cancelled");
                return mk_cancel();
            }
            cJSON *b = cJSON_GetArrayItem(flow->bindings, i);
            cJSON *name_v = cJSON_GetObjectItemCaseSensitive(b, "name");
            cJSON *expr_v = cJSON_GetObjectItemCaseSensitive(b, "expr");
            if (!cJSON_IsString(name_v) || !cJSON_IsObject(expr_v)) {
                exec_err(ctx->diag, "R155", NULL, "binding %d malformed", i);
                return mk_err();
            }
            exec_result_t r = eval_expr(ctx, expr_v, env);
            if (r.status != EXEC_OK) return r;
            env_bind(env, cJSON_GetStringValue(name_v), r.value);
        }
    }
    if (ctx->rt->cancel_requested) {
        exec_err(ctx->diag, "R160", NULL, "execution cancelled");
        return mk_cancel();
    }
    if (flow->return_expr == NULL) {
        exec_err(ctx->diag, "R155", NULL, "flow has no return expression");
        return mk_err();
    }
    return eval_expr(ctx, flow->return_expr, env);
}


/* ====================================================================
 * Engine entry
 * ==================================================================== */

/* Classify input as keyed form (a top-level object whose keys are the
 * flow's param names) vs. value-direct (the whole input is the single
 * param's value). Keyed wins on any param-name match.
 *
 * Ambiguity to be aware of: a single-param flow whose sole param is a
 * record type that itself has a field named identically to the param
 * is indistinguishable here — a value-direct record input is then
 * classified as keyed, and the param gets bound from input[param.name]
 * (the inner field) rather than from the whole record. This is a
 * silent misinterpretation, not an error. Keyed form takes precedence;
 * callers who need value-direct for such a flow must avoid the
 * colliding field name (or pass the record nested under the param key
 * to force keyed binding intentionally). */
static bool
input_is_keyed_form(const cJSON *input, const flow_t *flow)
{
    if (!cJSON_IsObject(input)) return false;
    for (size_t i = 0; i < flow->n_params; i++) {
        if (cJSON_GetObjectItemCaseSensitive(input,
                                             flow->params[i].name)) {
            return true;
        }
    }
    return false;
}

/* Synthesize the canonical JSON for the entire input value (one
 * cohesive record-or-scalar). When the input is keyed, we wrap it
 * up as an object so the trace records a single coherent input
 * value; when value-direct, the value itself is the input. */
static value_t *
build_input_value(Arena *arena, const type_registry_t *types,
                  const flow_t *flow, const cJSON *input,
                  bool keyed, DiagStream *diag)
{
    if (!keyed && flow->n_params == 1) {
        return value_from_json(arena, types, flow->params[0].type,
                               input, diag, flow->params[0].name);
    }
    /* Build a synthetic record with one field per param. */
    value_field_t *fields = arena_alloc_zero(arena,
        flow->n_params * sizeof *fields);
    for (size_t i = 0; i < flow->n_params; i++) {
        const flow_param_t *p = &flow->params[i];
        cJSON *src = cJSON_GetObjectItemCaseSensitive(input,
                                                       p->name);
        if (!src) return NULL;
        fields[i].name  = arena_strdup(arena, p->name);
        fields[i].value = value_from_json(arena, types, p->type, src,
                                          diag, p->name);
        if (!fields[i].value) return NULL;
    }
    return value_new_record_take(arena, TYPE_ID_NONE, fields,
                                 flow->n_params);
}

/* Shared body for normal run, replay, and resume. A plain replay
 * passes only `replay`; a resume passes BOTH — it layers decision
 * injection (`resume`) on top of replay's restore-from-trace logic
 * (see flowd_resume_impl), so the two are not mutually exclusive. */
static exec_status_t
run_or_replay(flowd_runtime *rt, size_t flow_idx,
              const char *input_json, const char *trace_dir,
              exec_replay_t *replay,
              exec_resume_t *resume,
              char **out_json, char **out_suspension_token,
              DiagStream *diag);

exec_status_t
flowd_run_impl(flowd_runtime *rt, size_t flow_idx,
               const char *input_json, const char *trace_dir,
               char **out_json, char **out_suspension_token,
               DiagStream *diag)
{
    return run_or_replay(rt, flow_idx, input_json, trace_dir,
                         NULL, NULL, out_json, out_suspension_token,
                         diag);
}

exec_status_t
flowd_replay_impl(flowd_runtime *rt, size_t flow_idx,
                  const char *original_dir, const char *new_trace_dir,
                  const char *new_model_id,
                  char **out_json, DiagStream *diag)
{
    if (!rt || !original_dir) {
        if (out_json) *out_json = NULL;
        diag_emit(diag, SRCLOC_NONE, DIAG_ERROR, "R155",
                  "flowd_replay_impl: null arguments");
        return EXEC_ERROR;
    }
    trace_reader_t *original = trace_reader_open(original_dir,
                                                 "flowd-replay", diag);
    if (!original) {
        if (out_json) *out_json = NULL;
        return EXEC_ERROR;
    }
    /* Extract the recorded input — node n0's only invocation carries
     * it as `output` (the synthetic input node). */
    char *input_json = trace_reader_invocation_output(original, "n0", 0u);
    if (!input_json) {
        trace_reader_close(original);
        if (out_json) *out_json = NULL;
        diag_emit(diag, SRCLOC_NONE, DIAG_ERROR, "R155",
                  "flowd_replay_impl: original trace has no n0 input");
        return EXEC_ERROR;
    }

    /* Reject replaying a different flow than the one recorded: the
     * recorded manifest's `flow` must match the flow being replayed,
     * else node-by-node restoration is meaningless. */
    {
        const cJSON *manifest = trace_reader_manifest(original);
        const cJSON *recf = manifest
            ? cJSON_GetObjectItemCaseSensitive(manifest, "flow")
            : NULL;
        const flow_t *want = flowd_flow_at(rt, flow_idx);
        if (cJSON_IsString(recf) && want &&
            strcmp(cJSON_GetStringValue(recf), want->name) != 0) {
            diag_emit(diag, SRCLOC_NONE, DIAG_ERROR, "R155",
                      "replay: recorded flow '%s' does not match "
                      "requested flow '%s'",
                      cJSON_GetStringValue(recf), want->name);
            free(input_json);
            trace_reader_close(original);
            if (out_json) *out_json = NULL;
            return EXEC_ERROR;
        }
    }

    exec_replay_t replay = {
        .original      = original,
        .original_dir  = original_dir,
        .new_model_id  = new_model_id,
    };
    exec_status_t st = run_or_replay(rt, flow_idx, input_json,
                                     new_trace_dir, &replay, NULL,
                                     out_json, NULL, diag);
    free(input_json);
    trace_reader_close(original);
    return st;
}

exec_status_t
flowd_resume_impl(flowd_runtime *rt,
                  const char    *suspension_token,
                  const char    *decision_json,
                  const char    *new_trace_dir,
                  char         **out_json,
                  char         **out_suspension_token,
                  DiagStream    *diag)
{
    if (out_json) *out_json = NULL;
    if (out_suspension_token) *out_suspension_token = NULL;
    if (!rt || !suspension_token || !decision_json || !new_trace_dir) {
        diag_emit(diag, SRCLOC_NONE, DIAG_ERROR, "R155",
                  "flowd_resume_impl: null argument");
        return EXEC_ERROR;
    }
    /* The token is the path to suspensions/<id>.json. Read and
     * parse it. The parent's parent is the original trace dir. */
    FILE *fp = fopen(suspension_token, "rb");
    if (!fp) {
        diag_emit(diag, SRCLOC_NONE, DIAG_ERROR, "R155",
                  "flowd_resume_impl: cannot open suspension file '%s'",
                  suspension_token);
        return EXEC_ERROR;
    }
    /* Guard fseek/ftell like the other file-slurp sites (ir_load.c,
     * trace.c, util.c): an unseekable token (pipe/FIFO) makes ftell
     * return -1, and (size_t)(-1)+1 == 0 would malloc(0) then fread
     * SIZE_MAX bytes into it (and write buf[-1]). */
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        diag_emit(diag, SRCLOC_NONE, DIAG_ERROR, "R155",
                  "flowd_resume_impl: cannot seek suspension file");
        return EXEC_ERROR;
    }
    long sz = ftell(fp);
    rewind(fp);
    if (sz < 0) {
        fclose(fp);
        diag_emit(diag, SRCLOC_NONE, DIAG_ERROR, "R155",
                  "flowd_resume_impl: cannot size suspension file");
        return EXEC_ERROR;
    }
    char *buf = malloc((size_t)sz + 1u);
    if (!buf) { fclose(fp); return EXEC_ERROR; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp); free(buf); return EXEC_ERROR;
    }
    buf[sz] = '\0';
    fclose(fp);
    cJSON *susp = cJSON_Parse(buf);
    free(buf);
    if (!susp) {
        diag_emit(diag, SRCLOC_NONE, DIAG_ERROR, "R155",
                  "flowd_resume_impl: suspension file is not valid JSON");
        return EXEC_ERROR;
    }
    const char *flow_name = NULL;
    const char *input_js  = NULL;
    const char *susp_node = NULL;
    {
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(susp, "flow");
        cJSON *in = cJSON_GetObjectItemCaseSensitive(susp, "input_json");
        cJSON *nd = cJSON_GetObjectItemCaseSensitive(susp, "node_id");
        if (!cJSON_IsString(fn) || !cJSON_IsString(in)
                                || !cJSON_IsString(nd)) {
            cJSON_Delete(susp);
            diag_emit(diag, SRCLOC_NONE, DIAG_ERROR, "R155",
                      "flowd_resume_impl: suspension missing flow/input/node");
            return EXEC_ERROR;
        }
        flow_name = cJSON_GetStringValue(fn);
        input_js  = cJSON_GetStringValue(in);
        susp_node = cJSON_GetStringValue(nd);
    }

    /* Find the flow by name. */
    size_t idx = (size_t)-1;
    size_t nf = flowd_flow_count(rt);
    for (size_t i = 0; i < nf; i++) {
        const flow_t *f = flowd_flow_at(rt, i);
        if (f && strcmp(f->name, flow_name) == 0) { idx = i; break; }
    }
    if (idx == (size_t)-1) {
        cJSON_Delete(susp);
        diag_emit(diag, SRCLOC_NONE, DIAG_ERROR, "R155",
                  "flowd_resume_impl: flow '%s' not in loaded IR",
                  flow_name);
        return EXEC_ERROR;
    }

    /* Derive original_dir from the token path: parent of parent. */
    char *path_copy = strdup(suspension_token);
    if (!path_copy) {
        cJSON_Delete(susp);
        diag_emit(diag, SRCLOC_NONE, DIAG_ERROR, "R155",
                  "flowd_resume_impl: out of memory");
        return EXEC_ERROR;
    }
    char *p = strrchr(path_copy, '/');
    if (p) *p = '\0';
    p = strrchr(path_copy, '/');
    if (p) *p = '\0';
    const char *original_dir = path_copy;

    /* Open the original trace as a reader so we can restore prior
     * nodes via the replay path. */
    trace_reader_t *original = trace_reader_open(original_dir,
                                                 "flowd-resume", diag);
    if (!original) {
        cJSON_Delete(susp);
        free(path_copy);
        return EXEC_ERROR;
    }

    /* The resume reuses the replay's restore-from-trace logic; we
     * also set the resume context so the suspended node injects
     * the decision instead of suspending again. */
    exec_replay_t replay_ctx = {
        .original     = original,
        .original_dir = original_dir,
        .new_model_id = NULL,
    };
    /* The resume must also know the suspended node for the inject. */
    char *node_dup = strdup(susp_node);
    if (!node_dup) {
        cJSON_Delete(susp);
        trace_reader_close(original);
        free(path_copy);
        diag_emit(diag, SRCLOC_NONE, DIAG_ERROR, "R155",
                  "flowd_resume_impl: out of memory");
        return EXEC_ERROR;
    }
    exec_resume_t resume_ctx = {
        .original        = original,
        .original_dir    = original_dir,
        .suspended_node  = node_dup,
        .decision_json   = decision_json,
    };

    /* Make a heap copy of input_js before we delete the cJSON. */
    char *input_owned = strdup(input_js);
    if (!input_owned) {
        cJSON_Delete(susp);
        free(node_dup);
        trace_reader_close(original);
        free(path_copy);
        diag_emit(diag, SRCLOC_NONE, DIAG_ERROR, "R155",
                  "flowd_resume_impl: out of memory");
        return EXEC_ERROR;
    }
    cJSON_Delete(susp);

    exec_status_t st = run_or_replay(rt, idx, input_owned,
                                     new_trace_dir,
                                     &replay_ctx, &resume_ctx,
                                     out_json, out_suspension_token, diag);
    free(input_owned);
    free(node_dup);
    trace_reader_close(original);
    free(path_copy);
    return st;
}

static exec_status_t
run_or_replay(flowd_runtime *rt, size_t flow_idx,
              const char *input_json, const char *trace_dir,
              exec_replay_t *replay,
              exec_resume_t *resume,
              char **out_json, char **out_suspension_token,
              DiagStream *diag)
{
    if (out_suspension_token) *out_suspension_token = NULL;
    if (out_json) *out_json = NULL;

    if (rt == NULL) {
        exec_err(diag, "R155", NULL, "flowd_run_impl: null runtime");
        return EXEC_ERROR;
    }
    if (flow_idx >= flowd_flow_count(rt)) {
        exec_err(diag, "R155", NULL,
                 "flow index %zu out of range (flows=%zu)",
                 flow_idx, flowd_flow_count(rt));
        return EXEC_ERROR;
    }
    const flow_t *flow = flowd_flow_at(rt, flow_idx);

    Arena *run_arena = arena_create(0);
    if (run_arena == NULL) {
        exec_err(diag, "R155", NULL, "arena_create failed");
        return EXEC_ERROR;
    }

    cJSON *input = input_json ? cJSON_Parse(input_json) : NULL;
    if (input == NULL) {
        exec_err(diag, "R155", NULL, "input is not valid JSON");
        arena_destroy(run_arena);
        return EXEC_ERROR;
    }

    env_t env;
    env_init(&env, run_arena);
    env_push(&env);

    bool keyed = input_is_keyed_form(input, flow);
    for (size_t i = 0; i < flow->n_params; i++) {
        const flow_param_t *p = &flow->params[i];
        const cJSON *src;
        if (keyed) {
            src = cJSON_GetObjectItemCaseSensitive(input, p->name);
            if (src == NULL) {
                exec_err(diag, "R155", NULL,
                         "input missing parameter '%s'", p->name);
                cJSON_Delete(input);
                arena_destroy(run_arena);
                return EXEC_ERROR;
            }
        } else {
            if (flow->n_params != 1) {
                exec_err(diag, "R155", NULL,
                         "value-direct input only allowed for "
                         "single-parameter flows");
                cJSON_Delete(input);
                arena_destroy(run_arena);
                return EXEC_ERROR;
            }
            src = input;
        }
        value_t *v = value_from_json(run_arena, flowd_types(rt),
                                     p->type, src, diag, p->name);
        if (v == NULL) {
            cJSON_Delete(input);
            arena_destroy(run_arena);
            return EXEC_ERROR;
        }
        env_bind(&env, p->name, v);
    }

    /* Open the trace writer if a directory was requested. The
     * writer's lifetime is the execution; it's sealed at the
     * happy end or closed-without-seal on error. */
    trace_writer_t *tw = NULL;
    if (trace_dir != NULL && trace_dir[0] != '\0') {
        /* Canonicalize the IR JSON for hashing. We don't have the
         * original IR bytes the host passed in (the runtime parsed
         * them into structures); re-emit a canonical form using the
         * existing JSON dumper. */
        char *ir_canonical = NULL;
        size_t ir_sz = 0;
        FILE *fp = open_memstream(&ir_canonical, &ir_sz);
        if (fp) {
            flowd_canonical_dump_json(rt, fp);
            fclose(fp);
        }
        /* A resume writes a sibling exec dir tagged "_resumed" so the
         * lineage shows by name; replay and normal runs are untagged. */
        tw = trace_writer_open(trace_dir, flow->name,
                               ir_canonical ? ir_canonical : "",
                               0, diag, resume ? "resumed" : NULL);
        free(ir_canonical);
        if (tw) {
            /* Auto-wire the host-installed redactor.
             * Hosts that want per-run control over redaction skip
             * flowd_set_redactor and call trace_writer_set_redactor
             * on a manually-opened writer instead. */
            if (rt->redactor) {
                trace_writer_set_redactor(tw, rt->redactor,
                                          rt->redactor_ctx);
            }
            value_t *iv = build_input_value(run_arena, flowd_types(rt),
                                            flow, input, keyed, diag);
            if (iv) trace_writer_set_input(tw, iv);

            /* Provenance linkage for replay/resume: the synthesized
             * input node (n0) is restored from the original, and a
             * resumed trace records which trace it continues. */
            const char *orig = replay ? replay->original_dir
                             : resume ? resume->original_dir : NULL;
            if (orig) {
                trace_writer_set_replay_of(tw, "n0", orig, "n0",
                                           "restored_from_trace");
            }
            if (resume) {
                trace_writer_set_resumed_from(tw, resume->original_dir);
            }
        }
    }

    exec_suspension_t susp = {0};
    /* Positional init -- order MUST track the exec_ctx_t field declaration
     * exactly (rt, arena, diag, trace, replay, resume, suspension_out,
     * subflow_depth).  Reorder the struct and you silently rewire these; the
     * trailing 0 is subflow_depth. */
    exec_ctx_t ctx = { rt, run_arena, diag, tw, replay, resume, &susp, 0 };

    /* Per-run reset: budget usage starts at 0, and any prior cancel
     * request is cleared so cancellation applies only to this run. */
    if (rt->has_budget) {
        rt->budget.tokens_used     = 0;
        rt->budget.cost_cents_used = 0.0;
        rt->budget.elapsed_ms_used = 0;
    }
    rt->cancel_requested = 0;

    exec_result_t r = run_flow(&ctx, flow, &env);

    if (r.status == EXEC_SUSPENDED && susp.token != NULL) {
        /* Write suspensions/<token>.json and seal manifest as
         * "suspended". The token (heap-owned) is returned to the
         * caller for later resume. */
        if (tw) {
            size_t plen = strlen(trace_writer_dir(tw))
                        + strlen("/suspensions/") + strlen(susp.token)
                        + strlen(".json") + 1u;
            char *susp_path = malloc(plen);
            snprintf(susp_path, plen, "%s/suspensions/%s.json",
                     trace_writer_dir(tw), susp.token);
            /* Ensure suspensions/ exists; ignore the result since an
             * already-existing dir (EEXIST) is the expected case.
             * Allocate the path to its exact length (like susp_path
             * above) rather than a fixed buffer that would silently
             * truncate a long trace dir and mkdir the wrong path. */
            size_t dlen = strlen(trace_writer_dir(tw))
                        + strlen("/suspensions") + 1u;
            char *dir = malloc(dlen);
            snprintf(dir, dlen, "%s/suspensions",
                     trace_writer_dir(tw));
            mkdir(dir, 0755);
            free(dir);
            FILE *fp = fopen(susp_path, "wb");
            if (fp) {
                fprintf(fp,
                    "{\n"
                    "  \"token\": \"%s\",\n"
                    "  \"flow\": \"%s\",\n"
                    "  \"node_id\": \"%s\",\n"
                    "  \"condition\": \"%s\",\n"
                    "  \"args\": %s,\n"
                    "  \"input_json\": ",
                    susp.token, flow->name, susp.node_id,
                    susp.condition, susp.args_json);
                /* input_json is itself JSON; we wrap it as a string
                 * literal in the suspension file. */
                value_emit_json_string(fp, input_json);
                fputs("\n}\n", fp);
                fclose(fp);
            }
            trace_writer_seal(tw, TRACE_STATUS_SUSPENDED);
            trace_writer_close(tw);
            if (out_suspension_token) {
                *out_suspension_token = susp_path;
            } else {
                free(susp_path);
            }
        }
        free(susp.token);
        cJSON_Delete(input);
        arena_destroy(run_arena);
        return EXEC_SUSPENDED;
    }
    free(susp.token);

    if (r.status != EXEC_OK) {
        if (tw) {
            trace_writer_seal(tw,
                r.status == EXEC_CANCELLED ? TRACE_STATUS_CANCELLED
                                           : TRACE_STATUS_FAILED);
            trace_writer_close(tw);
        }
        cJSON_Delete(input);
        arena_destroy(run_arena);
        return r.status;
    }

    if (tw) {
        trace_writer_set_output(tw, r.value);
        /* Link the synthesized output node back to the original trace.
         * On a replay the output is reconstructed from restored nodes
         * (restored_from_trace); on a resume it is produced by live
         * post-suspension execution (re_invoked). Not every node is
         * linked — nodes that ran live after a suspension point carry
         * no replay_of (see eval_call's live tool/model paths). */
        {
            const char *orig = replay ? replay->original_dir
                             : resume ? resume->original_dir : NULL;
            const char *out_id = trace_writer_last_node_id(tw);
            if (orig && out_id) {
                trace_writer_set_replay_of(tw, out_id, orig, out_id,
                    resume ? "re_invoked" : "restored_from_trace");
            }
        }
        trace_writer_seal(tw, TRACE_STATUS_COMPLETE);
        trace_writer_close(tw);
    }

    char *json = value_to_json_canonical(r.value);
    cJSON_Delete(input);
    arena_destroy(run_arena);

    if (json == NULL) {
        exec_err(diag, "R155", NULL, "value_to_json_canonical failed");
        return EXEC_ERROR;
    }
    if (out_json) *out_json = json;
    return EXEC_OK;
}
