/* src/check.c
 *
 * Type checker.  Walks the AST after the resolver has succeeded.
 *
 *
 * Strategy
 * --------
 *
 * Bottom-up inference, with a context check at every boundary:
 *
 *   - check_expr returns the inferred Type * of an expression.
 *   - At call sites, constructor sites, the pipeline source slot,
 *     and the flow return slot, the inferred type is compared to
 *     the expected type and a diagnostic is emitted on mismatch.
 *
 * The checker does not mutate the AST.  Inferred types accumulate
 * in a CheckedFlow per flow, parallel to ResolveLocals.names.
 *
 *
 * Pipeline-stage scope
 * --------------------
 *
 * Inside a pipeline stage that operates on `[T]`, bare identifiers
 * see both the element fields of `T` and the outer flow-scope
 * locals.  The two stage families resolve this differently:
 *
 *   - WHERE predicates go through resolve_stage_ident: an element
 *     field and an outer local of the same name are treated as a
 *     hard collision and rejected with E155 (returns NULL — the
 *     name is never silently bound to either one).  E155 is emitted
 *     on every colliding use, not just the first.
 *
 *   - SELECT / DEDUPE / ANY / ALL per-row bodies go through
 *     check_per_row_body, which pushes the element fields after the
 *     outer locals.  Because env_index searches most-recently-pushed
 *     first, an element field silently SHADOWS a same-named outer
 *     local here — no E155 is raised in these stages.
 *
 * Example: in `users | where age > 18`, if an outer binding named
 * `age` is also in scope, the use is reported as a collision (E155);
 * with no outer `age`, it resolves to the element field.
 *
 *
 * Error tolerance
 * ---------------
 *
 * On a type error inside an expression, the checker emits its
 * diagnostic and returns NULL.  Callers propagate NULL by returning
 * NULL themselves (or by suppressing dependent checks).  This is
 * enough to avoid cascade diagnostics in most cases without
 * complex recovery.
 */

#include "check.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "ast.h"
#include "resolve.h"
#include "util.h"


/* --------------------------------------------------------------------
 * Type helpers
 * -------------------------------------------------------------------- */

static int type_equal(const Type *a, const Type *b)
{
    if (a == NULL || b == NULL) return 0;
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case TYPE_STRING:
    case TYPE_INT:
    case TYPE_FLOAT:
    case TYPE_BOOL:
        return 1;
    case TYPE_NAMED:
        return strcmp(a->name, b->name) == 0;
    case TYPE_LIST:
        return type_equal(a->elem, b->elem);
    }
    return 0;
}

static int type_is_list(const Type *t)
{
    return t != NULL && t->kind == TYPE_LIST;
}

static int type_is_numeric(const Type *t)
{
    return t != NULL && (t->kind == TYPE_INT || t->kind == TYPE_FLOAT);
}

/* Linear scan rather than a hash: a type's field count is tiny, so
 * the scan is cheaper than building and probing a table. */
static const Field *type_decl_field(const TypeDecl *t, const char *name)
{
    size_t i;
    for (i = 0; i < t->n_fields; i++) {
        if (strcmp(t->fields[i]->name, name) == 0) return t->fields[i];
    }
    return NULL;
}

/* A mistyped field deserves the same "did you mean 'x'?" help a mistyped
 * name gets (the E124 path): collect the record/variant's field names and
 * run them through the shared OSA suggester. Returns the nearest field name,
 * or NULL if nothing is close. The fixed cap bounds the stack array — a
 * record with more fields than this is implausible, and truncating the
 * candidate list past it only forgoes a hint on an already-erroring path
 * (resolution itself stays uncapped), never misleads. */
#define FIELD_SUGGEST_CAP 64u
static const char *field_suggest(Field **fields, size_t n, const char *name)
{
    const char *names[FIELD_SUGGEST_CAP];
    size_t i, m = n < FIELD_SUGGEST_CAP ? n : FIELD_SUGGEST_CAP;
    for (i = 0; i < m; i++) names[i] = fields[i]->name;
    return flowc_suggest(name, names, m);
}

/* Only TYPE_NAMED resolves to a declaration; primitives and lists
 * have no TypeDecl, so they return NULL. */
static const TypeDecl *type_to_decl(const ResolveGlobals *g, const Type *t)
{
    if (t == NULL || t->kind != TYPE_NAMED) return NULL;
    return globals_find_type(g, t->name);
}


/* --------------------------------------------------------------------
 * Flow-local type environment
 * -------------------------------------------------------------------- */

typedef struct {
    const char    **names;
    const Type    **types;
    /* Parallel: for match binders the env carries the matched
     * variant so check_path's first descent uses the variant's
     * fields directly. NULL for every non-binder entry. */
    const Variant **variants;
    size_t          n;
    size_t          cap;
} TypeEnv;

static int env_index(const TypeEnv *e, const char *name)
{
    /* Search from most-recently-pushed downwards so match binders
     * and per-row stage fields shadow outer scope. */
    if (e->n == 0) return -1;
    size_t i = e->n;
    do {
        i--;
        if (strcmp(e->names[i], name) == 0) return (int)i;
    } while (i > 0);
    return -1;
}

static const Type *env_lookup(const TypeEnv *e, const char *name)
{
    int i = env_index(e, name);
    return i < 0 ? NULL : e->types[i];
}

static const Variant *env_lookup_variant(const TypeEnv *e, const char *name)
{
    int i = env_index(e, name);
    return i < 0 ? NULL : e->variants[i];
}

static void env_push(TypeEnv *e, const char *name, const Type *t)
{
    /* Callers size cap (outer + row + fields + match-binder slack);
     * env_push trusts that. Guard the invariant so an undersized cap
     * fails loudly in test builds instead of corrupting the arena. */
    assert(e->n < e->cap);
    e->names   [e->n] = name;
    e->types   [e->n] = t;
    e->variants[e->n] = NULL;
    e->n++;
}

static void env_push_variant_binder(TypeEnv *e, const char *name,
                                    const Type *t, const Variant *v)
{
    assert(e->n < e->cap);
    e->names   [e->n] = name;
    e->types   [e->n] = t;
    e->variants[e->n] = v;
    e->n++;
}


/* --------------------------------------------------------------------
 * Apply-argument checks (shared by call and construct)
 *
 * `kind_word` is "tool" or "type" (used in diagnostics).
 * `formals` and `n_formals` describe the expected slots (a Param[]
 * or Field[]).  This little adapter lets one routine handle both
 * shapes without duplicating the body.
 * -------------------------------------------------------------------- */

typedef struct {
    const char  *name;
    const Type  *type;
} Formal;

static void check_arg_list(const ResolveGlobals *g,
                           Arena *a,
                           DiagStream *diag,
                           TypeEnv *env,
                           const ExprApply *apply,
                           const Formal *formals,
                           size_t n_formals,
                           SrcLoc apply_loc,
                           const char *kind_word);

static const Type *check_expr(const ResolveGlobals *g,
                              Arena *a,
                              DiagStream *diag,
                              TypeEnv *env,
                              Expr *e);

/* Variant of check_expr that knows the surrounding context's
 * expected type. Used by call argument checking, record / variant
 * field checking, and flow-return checking so empty list literals
 * `[]` infer their element type from position instead of failing
 * E145. Every other case delegates straight to check_expr. */
static const Type *check_expr_expecting(const ResolveGlobals *g,
                                        Arena *a,
                                        DiagStream *diag,
                                        TypeEnv *env,
                                        Expr *e,
                                        const Type *expected)
{
    if (e != NULL &&
        e->kind == EXPR_LIST_LITERAL &&
        e->as.list_literal.n_elements == 0 &&
        expected != NULL &&
        expected->kind == TYPE_LIST &&
        expected->elem != NULL)
    {
        e->as.list_literal.element_type = expected->elem;
        Type *t = arena_alloc_zero(a, sizeof(*t));
        t->kind = TYPE_LIST;
        t->loc  = e->loc;
        t->elem = (Type *)expected->elem;
        return t;
    }
    return check_expr(g, a, diag, env, e);
}


/* --------------------------------------------------------------------
 * Path expression
 *
 * env_lookup the head; for each tail segment, descend through
 * field types. Errors:
 *   - head not in env: NULL (the resolver's E124 should have fired
 *     first; we land here only after earlier failures aborted the
 *     pipeline mid-flight)
 *   - field access through a non-record type: E140
 *   - field not found on record: E140
 * -------------------------------------------------------------------- */

static const Type *check_path(const ResolveGlobals *g,
                              DiagStream *diag,
                              const TypeEnv *env,
                              const Expr *e)
{
    if (e->as.path.n == 0) return NULL;

    const Type *t = env_lookup(env, e->as.path.segments[0]);
    if (t == NULL) {
        /* A genuinely unknown head reaches the checker only from inside
         * a per-row stage body (select/dedupe/any/all): the resolver
         * defers E124 there (permissive_paths, see resolve.c) because it
         * cannot see the element type's fields, leaving it to the checker
         * — which can — to reject a name that is neither an element field
         * nor an outer local. An unknown head in any ordinary expression
         * is caught by the resolver, and api.c gates check on a clean
         * resolve, so the checker never runs in that case. Emit here so
         * the deferred name is not silently accepted into the IR. */
        const char *head = e->as.path.segments[0];
        const char *sug  = flowc_suggest(head, env->names, env->n);
        if (sug != NULL) {
            diag_emit(diag, e->loc, DIAG_ERROR, "E124",
                      "reference to unknown name '%s'; did you mean '%s'?",
                      head, sug);
        } else {
            diag_emit(diag, e->loc, DIAG_ERROR, "E124",
                      "reference to unknown name '%s'", head);
        }
        return NULL;
    }

    /* Match-binder head: resolve the first descent against the
     * matched variant's record body. Subsequent descents fall back
     * to the regular record-field path. */
    const Variant *binder_variant =
        env_lookup_variant(env, e->as.path.segments[0]);

    size_t i;
    for (i = 1; i < e->as.path.n; i++) {
        if (i == 1 && binder_variant != NULL) {
            const Field *f = NULL;
            size_t k;
            for (k = 0; k < binder_variant->n_fields; k++) {
                if (strcmp(binder_variant->fields[k]->name,
                           e->as.path.segments[i]) == 0) {
                    f = binder_variant->fields[k];
                    break;
                }
            }
            if (f == NULL) {
                const char *seg = e->as.path.segments[i];
                SrcLoc loc = e->as.path.seg_locs[i];
                const char *sug = field_suggest(binder_variant->fields,
                                                binder_variant->n_fields, seg);
                if (sug != NULL)
                    diag_emit(diag, loc, DIAG_ERROR, "E140",
                              "no field '%s' on variant '%s'; did you mean '%s'?",
                              seg, binder_variant->name, sug);
                else
                    diag_emit(diag, loc, DIAG_ERROR, "E140",
                              "no field '%s' on variant '%s'",
                              seg, binder_variant->name);
                return NULL;
            }
            t = f->type;
            continue;
        }
        const TypeDecl *td = type_to_decl(g, t);
        if (td == NULL || td->kind != TYPE_DECL_RECORD) {
            diag_emit(diag, e->as.path.seg_locs[i], DIAG_ERROR, "E140",
                      "field access on non-record type");
            return NULL;
        }
        const Field *f = type_decl_field(td, e->as.path.segments[i]);
        if (f == NULL) {
            const char *seg = e->as.path.segments[i];
            SrcLoc loc = e->as.path.seg_locs[i];
            const char *sug = field_suggest(td->fields, td->n_fields, seg);
            if (sug != NULL)
                diag_emit(diag, loc, DIAG_ERROR, "E140",
                          "no field '%s' on type '%s'; did you mean '%s'?",
                          seg, td->name, sug);
            else
                diag_emit(diag, loc, DIAG_ERROR, "E140",
                          "no field '%s' on type '%s'", seg, td->name);
            return NULL;
        }
        t = f->type;
    }
    return t;
}


/* --------------------------------------------------------------------
 * Stage-internal predicates and identifiers
 *
 * Inside a stage, a "stage env" exposes the element fields of the
 * upstream value's [T].  Lookup order: stage env (element fields)
 * first, then flow env (outer-scope locals).  Collisions are E155
 * on first use.  Total miss is E151 (where) or E152 (rank).
 * -------------------------------------------------------------------- */

typedef struct {
    const TypeDecl *elem_decl;    /* may be NULL if T is not a record */
    const TypeEnv  *flow_env;
} StageEnv;

/* Resolve a single identifier inside a stage.  Returns its type,
 * or NULL on error.  Only where-predicate idents reach here, so a
 * total miss is always E151. */
static const Type *resolve_stage_ident(DiagStream *diag,
                                       const StageEnv *se,
                                       SrcLoc loc,
                                       const char *name)
{
    const Field *fld = NULL;
    if (se->elem_decl != NULL) {
        fld = type_decl_field(se->elem_decl, name);
    }
    const Type *outer = env_lookup(se->flow_env, name);

    if (fld != NULL && outer != NULL) {
        diag_emit(diag, loc, DIAG_ERROR, "E155",
                  "name '%s' collides with element field inside stage",
                  name);
        return NULL;
    }
    if (fld != NULL) return fld->type;
    if (outer != NULL) return outer;

    diag_emit(diag, loc, DIAG_ERROR, "E151",
              "where predicate references unknown field '%s'", name);
    return NULL;
}

/* Type of an Expr appearing inside a where predicate.  Paths are
 * looked up through the stage env (E151 on miss).  Literals are
 * straightforward.  Other Expr kinds are not expected here in v0
 * but we degrade gracefully. */
static const Type *check_stage_expr(const ResolveGlobals *g,
                                    Arena *a,
                                    DiagStream *diag,
                                    const StageEnv *se,
                                    const Expr *e)
{
    (void)a; (void)g;
    if (e == NULL) return NULL;
    switch (e->kind) {
    case EXPR_LITERAL: {
        Type *t = arena_alloc_zero(a, sizeof(*t));
        t->loc = e->loc;
        switch (e->as.literal.kind) {
        case LIT_INT:   t->kind = TYPE_INT;   break;
        case LIT_FLOAT: t->kind = TYPE_FLOAT; break;
        case LIT_BOOL:  t->kind = TYPE_BOOL;  break;
        }
        return t;
    }
    case EXPR_PATH: {
        /* Inside a stage, a single-segment path is resolved via
         * resolve_stage_ident.  Multi-segment paths first locate
         * the head (stage or outer), then walk fields. */
        if (e->as.path.n == 0) return NULL;
        const Type *head = resolve_stage_ident(diag, se, e->loc,
                                               e->as.path.segments[0]);
        if (head == NULL) return NULL;
        const Type *t = head;
        size_t i;
        for (i = 1; i < e->as.path.n; i++) {
            const TypeDecl *td = type_to_decl(g, t);
            if (td == NULL) {
                diag_emit(diag, e->as.path.seg_locs[i], DIAG_ERROR, "E140",
                          "field access on non-record type");
                return NULL;
            }
            const Field *f = type_decl_field(td, e->as.path.segments[i]);
            if (f == NULL) {
                const char *seg = e->as.path.segments[i];
                SrcLoc loc = e->as.path.seg_locs[i];
                const char *sug = field_suggest(td->fields, td->n_fields, seg);
                if (sug != NULL)
                    diag_emit(diag, loc, DIAG_ERROR, "E140",
                              "no field '%s' on type '%s'; did you mean '%s'?",
                              seg, td->name, sug);
                else
                    diag_emit(diag, loc, DIAG_ERROR, "E140",
                              "no field '%s' on type '%s'", seg, td->name);
                return NULL;
            }
            t = f->type;
        }
        return t;
    }
    default:
        /* CALL/CONSTRUCT/PIPELINE not expected as predicate operands
         * in v0.  If they appear, defer to the general checker. */
        return NULL;
    }
}

static int expr_is_literal(const Expr *e)
{
    return e != NULL && e->kind == EXPR_LITERAL;
}

static void check_cmp(const ResolveGlobals *g,
                      Arena *a,
                      DiagStream *diag,
                      const StageEnv *se,
                      const CmpExpr *c)
{
    const Type *lt = check_stage_expr(g, a, diag, se, c->lhs);
    const Type *rt = check_stage_expr(g, a, diag, se, c->rhs);
    if (lt == NULL || rt == NULL) return;

    if (!type_equal(lt, rt)) {
        /* If one side is a literal and the other a path, prefer
         * E153 (literal-vs-field mismatch).  Otherwise generic
         * E140. */
        int lit_vs_field =
            (expr_is_literal(c->lhs) && c->rhs && c->rhs->kind == EXPR_PATH) ||
            (expr_is_literal(c->rhs) && c->lhs && c->lhs->kind == EXPR_PATH);
        if (lit_vs_field) {
            diag_emit(diag, c->loc, DIAG_ERROR, "E153",
                      "where predicate literal incompatible with field type: %s vs %s",
                      ast_type_to_string(a, lt),
                      ast_type_to_string(a, rt));
        } else {
            diag_emit(diag, c->loc, DIAG_ERROR, "E140",
                      "type mismatch in comparison: %s vs %s",
                      ast_type_to_string(a, lt),
                      ast_type_to_string(a, rt));
        }
        return;
    }

    /* Ordering operators require numeric types; equality works on
     * any primitive. */
    int ordering = (c->op == CMP_LE || c->op == CMP_GE ||
                    c->op == CMP_LT || c->op == CMP_GT);
    if (ordering && !type_is_numeric(lt)) {
        diag_emit(diag, c->loc, DIAG_ERROR, "E140",
                  "ordering operator requires numeric operands, got %s",
                  ast_type_to_string(a, lt));
    }
}

static void check_predicate(const ResolveGlobals *g,
                            Arena *a,
                            DiagStream *diag,
                            const StageEnv *se,
                            const Predicate *p)
{
    if (p == NULL) return;
    switch (p->kind) {
    case PRED_CMP:
        check_cmp(g, a, diag, se, p->cmp);
        break;
    case PRED_AND:
    case PRED_OR:
        check_predicate(g, a, diag, se, p->left);
        check_predicate(g, a, diag, se, p->right);
        break;
    }
}


/* --------------------------------------------------------------------
 * Pipeline
 *
 * The source must yield [T].  Thread `cur_elem` through stages.
 * E150 on non-list source; E154 when a terminal stage is not last
 * (anything following it is unreachable); E151/E152 inside stages.
 * The final stage's signature determines the pipeline's overall
 * type ([T] for filter/sort, T for terminal).
 * -------------------------------------------------------------------- */

/* Evaluate `body` in a per-row scope: outer TypeEnv extended with
 * `row` bound to the whole element type plus every field of that
 * element type. The per-row bindings shadow
 * outer scope (env_index searches most-recently-pushed first so
 * the push order — outer, row, fields — gives the right precedence).
 * `elem_type` carries the [T]'s T as a Type*; `elem_decl` is its
 * record TypeDecl when T is a named record (NULL for primitives
 * or sums, in which case no fields are pushed). Returns the body's
 * inferred type or NULL if check_expr did. */
static const Type *check_per_row_body(const ResolveGlobals *g,
                                      Arena *a,
                                      DiagStream *diag,
                                      const TypeEnv *outer,
                                      const Type *elem_type,
                                      const TypeDecl *elem_decl,
                                      Expr *body)
{
    size_t outer_n = outer->n;
    size_t field_n = (elem_decl != NULL) ? elem_decl->n_fields : 0;
    /* +1 slot for the `row` binding, plus slack for any match binders
     * pushed while checking the body's arms. env_push has no bounds
     * check, so the cap must cover them — same discipline as check_flow
     * (a per-row body like `select match row { A a -> .., other -> .. }`
     * would otherwise overflow ext.names/types/variants). */
    size_t binder_slack = ast_count_match_binders(body);
    TypeEnv ext;
    ext.cap      = outer_n + 1 + field_n + binder_slack;
    ext.n        = 0;
    ext.names    = arena_alloc      (a, ext.cap * sizeof(*ext.names));
    ext.types    = arena_alloc      (a, ext.cap * sizeof(*ext.types));
    ext.variants = arena_alloc_zero (a, ext.cap * sizeof(*ext.variants));
    size_t k;
    for (k = 0; k < outer_n; k++) {
        ext.names   [k] = outer->names   [k];
        ext.types   [k] = outer->types   [k];
        ext.variants[k] = outer->variants[k];
    }
    ext.n = outer_n;
    /* `row` — bound to the element type T directly.
     * Field access `row.field` works via check_path's normal
     * record-field descent. */
    if (elem_type != NULL) {
        env_push(&ext, "row", elem_type);
    }
    for (k = 0; k < field_n; k++) {
        env_push(&ext, elem_decl->fields[k]->name, elem_decl->fields[k]->type);
    }
    return check_expr(g, a, diag, &ext, body);
}

/* Look up a field's type on a record element. Emits the diagnostic itself so
 * the aggregate terminals can call it without repeating the boilerplate, and
 * takes two locations because the two errors point at different things: E149
 * (the element type is not a record at all) is a stage-level problem and points
 * at `stage_loc`; E140 (that record has no such field) points at `field_loc`,
 * the field name. Returns NULL in both error cases. */
static const Type *elem_field_type(DiagStream *diag,
                                   const TypeDecl *elem_decl,
                                   const char *field,
                                   SrcLoc stage_loc, SrcLoc field_loc,
                                   const char *stage_name)
{
    if (elem_decl == NULL) {
        diag_emit(diag, stage_loc, DIAG_ERROR, "E149",
                  "%s requires a record element type", stage_name);
        return NULL;
    }
    const Field *f = type_decl_field(elem_decl, field);
    if (f == NULL) {
        const char *sug = field_suggest(elem_decl->fields,
                                        elem_decl->n_fields, field);
        if (sug != NULL)
            diag_emit(diag, field_loc, DIAG_ERROR, "E140",
                      "no field '%s' on type '%s'; did you mean '%s'?",
                      field, elem_decl->name, sug);
        else
            diag_emit(diag, field_loc, DIAG_ERROR, "E140",
                      "no field '%s' on type '%s'", field, elem_decl->name);
        return NULL;
    }
    return f->type;
}

static int type_is_primitive(const Type *t)
{
    if (t == NULL) return 0;
    return t->kind == TYPE_STRING || t->kind == TYPE_INT
        || t->kind == TYPE_FLOAT  || t->kind == TYPE_BOOL;
}

static const Type *check_pipeline(const ResolveGlobals *g,
                                  Arena *a,
                                  DiagStream *diag,
                                  TypeEnv *env,
                                  Expr *e)
{
    const Type *src_t = check_expr(g, a, diag, env, e->as.pipeline.source);
    if (src_t == NULL) return NULL;
    if (!type_is_list(src_t)) {
        diag_emit(diag, e->loc, DIAG_ERROR, "E150",
                  "pipeline applied to non-list value (source type %s)",
                  ast_type_to_string(a, src_t));
        return NULL;
    }

    /* cur_elem advances when STAGE_SELECT changes the element type;
     * stays the same otherwise. cur_decl is its TypeDecl when the
     * element is a named record (for field lookups in rank /
     * select / dedupe / sum / max / min / any / all). */
    const Type     *cur_elem = src_t->elem;
    const TypeDecl *cur_decl = type_to_decl(g, cur_elem);

    StageEnv se;
    se.elem_decl = cur_decl;
    se.flow_env  = env;

    int saw_terminal = 0;
    const Type *result_t = src_t;   /* [T] until a terminal reduces */
    size_t i;

    for (i = 0; i < e->as.pipeline.n_stages; i++) {
        Stage *s = e->as.pipeline.stages[i];

        if (saw_terminal) {
            diag_emit(diag, s->loc, DIAG_ERROR, "E154",
                      "terminal stage appears before the end of the pipeline");
        }

        switch (s->kind) {
        case STAGE_WHERE:
            check_predicate(g, a, diag, &se, s->predicate);
            break;
        case STAGE_RANK: {
            const Field *f = (cur_decl != NULL)
                ? type_decl_field(cur_decl, s->sort_field) : NULL;
            if (f == NULL) {
                diag_emit(diag, s->loc, DIAG_ERROR, "E152",
                          "rank clause references unknown field '%s'",
                          s->sort_field);
            } else if (!type_is_numeric(f->type)) {
                diag_emit(diag, s->loc, DIAG_ERROR, "E149",
                          "rank field '%s' must be numeric, got %s",
                          s->sort_field, ast_type_to_string(a, f->type));
            }
            break;
        }
        case STAGE_SELECT: {
            const Type *body_t = check_per_row_body(g, a, diag, env,
                                                    cur_elem, cur_decl, s->body);
            if (body_t != NULL) {
                cur_elem = body_t;
                cur_decl = type_to_decl(g, cur_elem);
                /* Refresh per-row scope env metadata for stages
                 * that come after this select. */
                se.elem_decl = cur_decl;
                Type *new_list = arena_alloc_zero(a, sizeof(*new_list));
                new_list->kind = TYPE_LIST;
                new_list->elem = (Type *)cur_elem;
                new_list->loc  = s->loc;
                result_t = new_list;
            }
            break;
        }
        case STAGE_DEDUPE: {
            const Type *key_t = check_per_row_body(g, a, diag, env,
                                                   cur_elem, cur_decl, s->body);
            if (key_t != NULL && !type_is_primitive(key_t)) {
                diag_emit(diag, s->loc, DIAG_ERROR, "E149",
                          "dedupe key must be a primitive type, got %s",
                          ast_type_to_string(a, key_t));
            }
            break;
        }
        case STAGE_CONCAT: {
            /* `concat ys` evaluates ys in outer scope (no per-row);
             * its type must match the pipeline's current [T]. */
            const Type *other_t = check_expr(g, a, diag, env, s->body);
            if (other_t != NULL) {
                if (!type_is_list(other_t) ||
                    !type_equal(other_t->elem, cur_elem)) {
                    diag_emit(diag, s->loc, DIAG_ERROR, "E141",
                              "concat operand must be %s, got %s",
                              ast_type_to_string(a, result_t),
                              ast_type_to_string(a, other_t));
                }
            }
            break;
        }
        case STAGE_TAKE:
            /* Grammar already enforced INT_LIT. Nothing to type-check;
             * the value is preserved in s->take_count. */
            break;
        case STAGE_TERMINAL: {
            saw_terminal = 1;
            switch (s->terminal_kind) {
            case TERMINAL_TOP:
            case TERMINAL_PICK:
                result_t = cur_elem;
                break;
            case TERMINAL_COUNT: {
                Type *int_t = arena_alloc_zero(a, sizeof(*int_t));
                int_t->kind = TYPE_INT;
                int_t->loc  = s->loc;
                result_t = int_t;
                break;
            }
            case TERMINAL_SUM:
            case TERMINAL_MAX:
            case TERMINAL_MIN: {
                const char *name =
                    (s->terminal_kind == TERMINAL_SUM) ? "sum" :
                    (s->terminal_kind == TERMINAL_MAX) ? "max" : "min";
                const Type *ft = elem_field_type(diag, cur_decl, s->agg_field,
                                                 s->loc, s->agg_field_loc, name);
                if (ft != NULL && !type_is_numeric(ft)) {
                    diag_emit(diag, s->loc, DIAG_ERROR, "E149",
                              "%s field '%s' must be numeric, got %s",
                              name, s->agg_field,
                              ast_type_to_string(a, ft));
                    result_t = NULL;
                } else if (ft != NULL) {
                    result_t = ft;
                }
                break;
            }
            case TERMINAL_ANY:
            case TERMINAL_ALL: {
                const Type *pt = check_per_row_body(g, a, diag, env,
                                                    cur_elem, cur_decl, s->agg_pred);
                if (pt != NULL && pt->kind != TYPE_BOOL) {
                    diag_emit(diag, s->loc, DIAG_ERROR, "E143",
                              "%s predicate must have type 'bool', found %s",
                              (s->terminal_kind == TERMINAL_ANY) ? "any" : "all",
                              ast_type_to_string(a, pt));
                }
                Type *bool_t = arena_alloc_zero(a, sizeof(*bool_t));
                bool_t->kind = TYPE_BOOL;
                bool_t->loc  = s->loc;
                result_t = bool_t;
                break;
            }
            }
            break;
        }
        }
    }

    return result_t;
}


/* --------------------------------------------------------------------
 * Call / Construct argument checking
 * -------------------------------------------------------------------- */

/* Returns 1 iff `name` appears earlier in `apply->args[0..idx]`. */
static int arg_name_duplicated(const ExprApply *apply, size_t idx)
{
    const char *name = apply->args[idx]->field;
    if (name == NULL) return 0;
    size_t j;
    for (j = 0; j < idx; j++) {
        const char *other = apply->args[j]->field;
        if (other != NULL && strcmp(other, name) == 0) return 1;
    }
    return 0;
}

static int formal_index(const Formal *formals, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (strcmp(formals[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static void check_arg_list(const ResolveGlobals *g,
                           Arena *a,
                           DiagStream *diag,
                           TypeEnv *env,
                           const ExprApply *apply,
                           const Formal *formals,
                           size_t n_formals,
                           SrcLoc apply_loc,
                           const char *kind_word)
{
    /* Per-formal satisfied flags; the trailing loop reports any
     * formal left unsatisfied as a missing argument (E141). */
    int *satisfied = arena_alloc_zero(a, n_formals * sizeof(int));

    /* Shorthand binds by absolute position: the Nth unnamed arg always
     * targets the Nth formal, regardless of which formals named args already
     * filled (so f(x:1, 2) with x==formal[0] makes 2 also hit formal[0] and
     * trips E143, not formal[1]). ir.c's emit_args_from_formals uses the same
     * rule; otherwise the checker and the IR would disagree on the mapping. */
    size_t shorthand_i = 0;
    size_t i;
    for (i = 0; i < apply->n_args; i++) {
        const Arg *arg = apply->args[i];

        if (arg->field != NULL && arg_name_duplicated(apply, i)) {
            diag_emit(diag, arg->loc, DIAG_ERROR, "E143",
                      "duplicate argument '%s'", arg->field);
            continue;
        }

        int target = -1;
        if (arg->field != NULL) {
            target = formal_index(formals, n_formals, arg->field);
            if (target < 0) {
                diag_emit(diag, arg->loc, DIAG_ERROR, "E142",
                          "argument '%s' does not name a field of %s",
                          arg->field, kind_word);
                continue;
            }
        } else {
            if (shorthand_i >= n_formals) {
                diag_emit(diag, arg->loc, DIAG_ERROR, "E142",
                          "too many positional arguments for %s", kind_word);
                continue;
            }
            target = (int)shorthand_i++;
        }

        if (satisfied[target]) {
            diag_emit(diag, arg->loc, DIAG_ERROR, "E143",
                      "duplicate argument '%s'", formals[target].name);
            continue;
        }

        const Type *vt = check_expr_expecting(g, a, diag, env,
                                              arg->value,
                                              formals[target].type);
        if (vt != NULL && !type_equal(vt, formals[target].type)) {
            diag_emit(diag, arg->loc, DIAG_ERROR, "E144",
                      "argument type incompatible: expected %s, got %s",
                      ast_type_to_string(a, formals[target].type),
                      ast_type_to_string(a, vt));
        }
        satisfied[target] = 1;
    }

    for (i = 0; i < n_formals; i++) {
        if (!satisfied[i]) {
            diag_emit(diag, apply_loc, DIAG_ERROR, "E141",
                      "missing required argument '%s'", formals[i].name);
        }
    }
}


/* --------------------------------------------------------------------
 * Top-level expression checker
 * -------------------------------------------------------------------- */

static const Type *check_expr(const ResolveGlobals *g,
                              Arena *a,
                              DiagStream *diag,
                              TypeEnv *env,
                              Expr *e)
{
    if (e == NULL) return NULL;
    switch (e->kind) {

    case EXPR_LITERAL: {
        Type *t = arena_alloc_zero(a, sizeof(*t));
        t->loc = e->loc;
        switch (e->as.literal.kind) {
        case LIT_INT:   t->kind = TYPE_INT;   break;
        case LIT_FLOAT: t->kind = TYPE_FLOAT; break;
        case LIT_BOOL:  t->kind = TYPE_BOOL;  break;
        }
        return t;
    }

    case EXPR_LIST_LITERAL: {
        if (e->as.list_literal.n_elements == 0) {
            /* Reached only when no expected type was supplied;
             * check_expr_expecting intercepts `[]` before here when
             * it has context. Bare check_expr has nothing to infer the
             * element type from, so emit E145 and return NULL. */
            diag_emit(diag, e->loc, DIAG_ERROR, "E145",
                      "empty list literal '[]' requires a type "
                      "annotation or context");
            return NULL;
        }
        const Type *elem_t = NULL;
        size_t i;
        for (i = 0; i < e->as.list_literal.n_elements; i++) {
            const Type *et = check_expr(g, a, diag, env,
                                        e->as.list_literal.elements[i]);
            if (et == NULL) continue;
            if (elem_t == NULL) {
                elem_t = et;
            } else if (!type_equal(elem_t, et)) {
                diag_emit(diag,
                          e->as.list_literal.elements[i]->loc,
                          DIAG_ERROR, "E141",
                          "list element type %s differs from earlier "
                          "elements' type %s",
                          ast_type_to_string(a, et),
                          ast_type_to_string(a, elem_t));
            }
        }
        if (elem_t == NULL) return NULL;
        e->as.list_literal.element_type = elem_t;
        Type *t = arena_alloc_zero(a, sizeof(*t));
        t->kind = TYPE_LIST;
        t->loc  = e->loc;
        t->elem = (Type *)elem_t;
        return t;
    }

    case EXPR_PATH:
        return check_path(g, diag, env, e);

    case EXPR_CALL: {
        /* Sub-flow call (resolver matched a flow decl): formals come
         * from the flow's parameter list; return type is the flow's
         * declared output. */
        if (e->as.apply.resolved_flow != NULL) {
            const FlowDecl *flow = e->as.apply.resolved_flow;
            Formal *formals = arena_alloc(a, flow->n_params * sizeof(*formals));
            size_t i;
            for (i = 0; i < flow->n_params; i++) {
                formals[i].name = flow->params[i]->name;
                formals[i].type = flow->params[i]->type;
            }
            check_arg_list(g, a, diag, env, &e->as.apply,
                           formals, flow->n_params, e->loc, "flow");
            return flow->return_type;
        }
        /* Tool call. */
        const ToolDecl *tool = globals_find_tool(g, e->as.apply.name);
        if (tool == NULL) {
            /* resolver already emitted E122; just propagate */
            return NULL;
        }
        Formal *formals = arena_alloc(a, tool->n_params * sizeof(*formals));
        size_t i;
        for (i = 0; i < tool->n_params; i++) {
            formals[i].name = tool->params[i]->name;
            formals[i].type = tool->params[i]->type;
        }
        check_arg_list(g, a, diag, env, &e->as.apply, formals, tool->n_params,
                       e->loc, "tool");
        return tool->return_type;
    }

    case EXPR_CONSTRUCT: {
        /* Variant construct (resolver matched a sum-type variant): type-
         * check args against the variant's field list, return the sum
         * type as the construct's value type. */
        if (e->as.apply.resolved_variant != NULL) {
            const TypeDecl *sum = e->as.apply.resolved_sum;
            const Variant  *var = e->as.apply.resolved_variant;
            Formal *formals = arena_alloc(a, var->n_fields * sizeof(*formals));
            size_t i;
            for (i = 0; i < var->n_fields; i++) {
                formals[i].name = var->fields[i]->name;
                formals[i].type = var->fields[i]->type;
            }
            check_arg_list(g, a, diag, env, &e->as.apply,
                           formals, var->n_fields, e->loc, "variant");
            Type *t = arena_alloc_zero(a, sizeof(*t));
            t->kind = TYPE_NAMED;
            t->name = sum->name;
            t->loc  = e->loc;
            return t;
        }
        /* Record construct. */
        const TypeDecl *td = globals_find_type(g, e->as.apply.name);
        if (td == NULL || td->kind != TYPE_DECL_RECORD) {
            /* resolver already emitted E123; propagate */
            return NULL;
        }
        Formal *formals = arena_alloc(a, td->n_fields * sizeof(*formals));
        size_t i;
        for (i = 0; i < td->n_fields; i++) {
            formals[i].name = td->fields[i]->name;
            formals[i].type = td->fields[i]->type;
        }
        check_arg_list(g, a, diag, env, &e->as.apply, formals, td->n_fields,
                       e->loc, "type");
        Type *t = arena_alloc_zero(a, sizeof(*t));
        t->kind = TYPE_NAMED;
        t->name = td->name;
        t->loc  = e->loc;
        return t;
    }

    case EXPR_PIPELINE:
        return check_pipeline(g, a, diag, env, e);

    case EXPR_CONDITIONAL: {
        /* Each cond must be bool (E143); all branches
         * + else_expr unify (E141); result is the unified type. */
        const Type *result = NULL;
        size_t i;
        for (i = 0; i < e->as.conditional.n_branches; i++) {
            const Type *ct = check_expr(g, a, diag, env,
                                        e->as.conditional.branches[i]->cond);
            if (ct != NULL && ct->kind != TYPE_BOOL) {
                diag_emit(diag, e->as.conditional.branches[i]->cond->loc,
                          DIAG_ERROR, "E143",
                          "predicate must have type 'bool', found %s",
                          ast_type_to_string(a, ct));
            }
            const Type *bt = check_expr(g, a, diag, env,
                                        e->as.conditional.branches[i]->consequent);
            if (bt != NULL) {
                if (result == NULL) {
                    result = bt;
                } else if (!type_equal(result, bt)) {
                    diag_emit(diag,
                              e->as.conditional.branches[i]->consequent->loc,
                              DIAG_ERROR, "E141",
                              "conditional branch type %s differs from earlier branch type %s",
                              ast_type_to_string(a, bt),
                              ast_type_to_string(a, result));
                }
            }
        }
        const Type *et = check_expr(g, a, diag, env,
                                    e->as.conditional.else_expr);
        if (et != NULL) {
            if (result == NULL) {
                result = et;
            } else if (!type_equal(result, et)) {
                diag_emit(diag, e->as.conditional.else_expr->loc,
                          DIAG_ERROR, "E141",
                          "else branch type %s differs from earlier branch type %s",
                          ast_type_to_string(a, et),
                          ast_type_to_string(a, result));
            }
        }
        return result;
    }

    case EXPR_TRY_ELSE: {
        /* Try and else types must match (E141);
         * result is the unified type. */
        const Type *tt = check_expr(g, a, diag, env, e->as.try_else.try_expr);
        const Type *et = check_expr(g, a, diag, env, e->as.try_else.else_expr);
        if (tt == NULL) return et;
        if (et == NULL) return tt;
        if (!type_equal(tt, et)) {
            diag_emit(diag, e->as.try_else.else_expr->loc, DIAG_ERROR, "E141",
                      "try/else types disagree: try has %s, else has %s",
                      ast_type_to_string(a, tt),
                      ast_type_to_string(a, et));
        }
        return tt;
    }

    case EXPR_BINOP: {
        const Type *lt = check_expr(g, a, diag, env, e->as.binop.lhs);
        const Type *rt = check_expr(g, a, diag, env, e->as.binop.rhs);
        if (lt == NULL || rt == NULL) return NULL;
        BinopOp op = e->as.binop.op;
        Type *bool_t = NULL;
        switch (op) {
        case BINOP_OR:
        case BINOP_AND:
            if (lt->kind != TYPE_BOOL || rt->kind != TYPE_BOOL) {
                diag_emit(diag, e->loc, DIAG_ERROR, "E149",
                          "operator '%s' requires bool operands, got %s and %s",
                          ast_binop_str(op),
                          ast_type_to_string(a, lt),
                          ast_type_to_string(a, rt));
            }
            bool_t = arena_alloc_zero(a, sizeof(*bool_t));
            bool_t->kind = TYPE_BOOL;
            bool_t->loc  = e->loc;
            return bool_t;
        case BINOP_EQ:
        case BINOP_NEQ:
            if (!type_is_primitive(lt) || !type_is_primitive(rt)) {
                diag_emit(diag, e->loc, DIAG_ERROR, "E146",
                          "equality '%s' not admitted for %s; only primitives compare",
                          ast_binop_str(op), ast_type_to_string(a, lt));
            } else if (!type_equal(lt, rt)) {
                diag_emit(diag, e->loc, DIAG_ERROR, "E142",
                          "operands of '%s' must have the same type, got %s and %s",
                          ast_binop_str(op),
                          ast_type_to_string(a, lt),
                          ast_type_to_string(a, rt));
            }
            bool_t = arena_alloc_zero(a, sizeof(*bool_t));
            bool_t->kind = TYPE_BOOL;
            bool_t->loc  = e->loc;
            return bool_t;
        case BINOP_LT:
        case BINOP_GT:
        case BINOP_LE:
        case BINOP_GE:
            if (!type_equal(lt, rt)) {
                diag_emit(diag, e->loc, DIAG_ERROR, "E142",
                          "operands of '%s' must have the same type, got %s and %s",
                          ast_binop_str(op),
                          ast_type_to_string(a, lt),
                          ast_type_to_string(a, rt));
            } else if (!type_is_numeric(lt) && lt->kind != TYPE_STRING) {
                diag_emit(diag, e->loc, DIAG_ERROR, "E149",
                          "operator '%s' requires numeric or string operands, got %s",
                          ast_binop_str(op), ast_type_to_string(a, lt));
            }
            bool_t = arena_alloc_zero(a, sizeof(*bool_t));
            bool_t->kind = TYPE_BOOL;
            bool_t->loc  = e->loc;
            return bool_t;
        case BINOP_ADD:
        case BINOP_SUB:
        case BINOP_MUL:
        case BINOP_DIV:
            if (!type_equal(lt, rt)) {
                diag_emit(diag, e->loc, DIAG_ERROR, "E142",
                          "operands of '%s' must have the same numeric type, got %s and %s",
                          ast_binop_str(op),
                          ast_type_to_string(a, lt),
                          ast_type_to_string(a, rt));
                return NULL;
            }
            if (!type_is_numeric(lt)) {
                diag_emit(diag, e->loc, DIAG_ERROR, "E149",
                          "operator '%s' requires numeric operands, got %s",
                          ast_binop_str(op), ast_type_to_string(a, lt));
                return NULL;
            }
            return lt;
        case BINOP_MOD:
            if (lt->kind != TYPE_INT || rt->kind != TYPE_INT) {
                diag_emit(diag, e->loc, DIAG_ERROR, "E149",
                          "operator '%%' requires int operands, got %s and %s",
                          ast_type_to_string(a, lt),
                          ast_type_to_string(a, rt));
                return NULL;
            }
            return lt;
        }
        return NULL;
    }

    case EXPR_UNOP: {
        const Type *ot = check_expr(g, a, diag, env, e->as.unop.operand);
        if (ot == NULL) return NULL;
        if (e->as.unop.op == UNOP_NEG) {
            if (!type_is_numeric(ot)) {
                diag_emit(diag, e->loc, DIAG_ERROR, "E149",
                          "unary '-' requires a numeric operand, got %s",
                          ast_type_to_string(a, ot));
                return NULL;
            }
            return ot;
        }
        /* UNOP_NOT */
        if (ot->kind != TYPE_BOOL) {
            diag_emit(diag, e->loc, DIAG_ERROR, "E149",
                      "'not' requires a bool operand, got %s",
                      ast_type_to_string(a, ot));
        }
        Type *bt = arena_alloc_zero(a, sizeof(*bt));
        bt->kind = TYPE_BOOL;
        bt->loc  = e->loc;
        return bt;
    }

    case EXPR_MATCH: {
        /* 1. Scrutinee must be a sum type (E161). */
        const Type *st = check_expr(g, a, diag, env, e->as.match.scrutinee);
        if (st == NULL) return NULL;
        const TypeDecl *sum = type_to_decl(g, st);
        if (sum == NULL || sum->kind != TYPE_DECL_SUM) {
            diag_emit(diag, e->loc, DIAG_ERROR, "E161",
                      "match expression's scrutinee must have sum type, "
                      "found %s",
                      ast_type_to_string(a, st));
            return NULL;
        }
        e->as.match.resolved_sum = sum;

        /* 2. Walk arms: resolve pattern variants against the sum,
         *    push binder, check body, unify body type, track coverage. */
        int *covered = arena_alloc_zero(a, sum->n_variants * sizeof(int));
        int  wildcard_seen = 0;
        const Type *result_type = NULL;
        size_t i, j;
        for (i = 0; i < e->as.match.n_arms; i++) {
            MatchArm *arm = e->as.match.arms[i];
            const Variant *matched_variant = NULL;
            if (arm->pattern.kind == PAT_WILDCARD ||
                arm->pattern.kind == PAT_BIND) {
                /* Catch-all arms (wildcard or bare-lowercase binder).
                 * Both cover every remaining variant
                 * for exhaustiveness. PAT_BIND additionally binds
                 * the matched value to a name in the arm body. */
                wildcard_seen = 1;
            } else {
                /* Look up the named variant in this sum. E128 if not
                 * found in the scrutinee's sum. */
                for (j = 0; j < sum->n_variants; j++) {
                    if (strcmp(sum->variants[j]->name,
                               arm->pattern.variant_name) == 0) {
                        matched_variant = sum->variants[j];
                        break;
                    }
                }
                if (matched_variant == NULL) {
                    diag_emit(diag, arm->pattern.loc, DIAG_ERROR, "E128",
                              "unknown variant '%s' in sum type '%s'",
                              arm->pattern.variant_name, sum->name);
                } else {
                    /* E162 duplicate variant arm. */
                    size_t k;
                    for (k = 0; k < sum->n_variants; k++) {
                        if (sum->variants[k] == matched_variant) break;
                    }
                    if (k < sum->n_variants && covered[k]) {
                        diag_emit(diag, arm->pattern.loc, DIAG_ERROR, "E162",
                                  "duplicate variant '%s' in match arms",
                                  arm->pattern.variant_name);
                    } else if (k < sum->n_variants) {
                        covered[k] = 1;
                    }
                    arm->pattern.resolved_variant = matched_variant;
                }
            }

            /* Push binder (if any) for the arm body. The variant-
             * binder form binds the variant payload (resolved via
             * env_push_variant_binder + check_path's variant
             * descent). The catch-all bind form binds the whole
             * scrutinee value (typed as the sum). */
            size_t snapshot = env->n;
            if (arm->pattern.kind == PAT_VARIANT_BIND &&
                matched_variant != NULL) {
                Type *binder_t = arena_alloc_zero(a, sizeof(*binder_t));
                binder_t->kind = TYPE_NAMED;
                binder_t->name = sum->name;
                binder_t->loc  = arm->pattern.loc;
                env_push_variant_binder(env, arm->pattern.binder_name,
                                        binder_t, matched_variant);
            } else if (arm->pattern.kind == PAT_BIND) {
                /* The binder receives the scrutinee value as-is, so
                 * its type is the sum type. No variant pointer —
                 * field access on it falls through to check_path's
                 * regular record-field path, where type_to_decl yields
                 * a TYPE_DECL_SUM (not a record) and the access is
                 * rejected with E140 'field access on non-record type'
                 * (a sum has no top-level fields to descend into). */
                Type *binder_t = arena_alloc_zero(a, sizeof(*binder_t));
                binder_t->kind = TYPE_NAMED;
                binder_t->name = sum->name;
                binder_t->loc  = arm->pattern.loc;
                env_push(env, arm->pattern.binder_name, binder_t);
            }

            const Type *body_t = check_expr(g, a, diag, env, arm->body);
            env->n = snapshot;

            if (body_t != NULL) {
                if (result_type == NULL) {
                    result_type = body_t;
                } else if (!type_equal(result_type, body_t)) {
                    diag_emit(diag, arm->loc, DIAG_ERROR, "E141",
                              "match arm body type %s differs from earlier arm type %s",
                              ast_type_to_string(a, body_t),
                              ast_type_to_string(a, result_type));
                }
            }
        }

        /* 3. Exhaustiveness (E160). */
        if (!wildcard_seen) {
            for (j = 0; j < sum->n_variants; j++) {
                if (!covered[j]) {
                    diag_emit(diag, e->loc, DIAG_ERROR, "E160",
                              "non-exhaustive match: uncovered variant '%s'",
                              sum->variants[j]->name);
                    /* Continue reporting more uncovered variants per
                     * call. One diagnostic per uncovered variant is
                     * the simplest faithful approach. */
                }
            }
        }

        return result_type;
    }
    }
    return NULL;
}


/* --------------------------------------------------------------------
 * Per-flow checker
 * -------------------------------------------------------------------- */

static CheckedFlow *check_flow(Arena *a, DiagStream *diag,
                               const ResolveGlobals *g,
                               const FlowDecl *f,
                               const ResolveLocals *locals)
{
    CheckedFlow *cf = arena_alloc_zero(a, sizeof(*cf));
    cf->n = locals->n;
    cf->types = arena_alloc_zero(a, cf->n * sizeof(*cf->types));

    /* Build the type env in declaration order: parameters first
     * (with their declared types), then bindings (with inferred
     * types).  We populate cf->types in parallel as we go.
     *
     * The cap includes slack for match binders that get pushed
     * during arm-body checking — counted by ast_count_flow_binders so
     * env_push never overflows. */
    size_t binder_slack = ast_count_flow_binders(f);
    TypeEnv env;
    env.cap      = locals->n + binder_slack;
    env.n        = 0;
    env.names    = arena_alloc(a, env.cap * sizeof(*env.names));
    env.types    = arena_alloc(a, env.cap * sizeof(*env.types));
    env.variants = arena_alloc_zero(a, env.cap * sizeof(*env.variants));

    size_t i;
    for (i = 0; i < f->n_params; i++) {
        const Param *p = f->params[i];
        env_push(&env, p->name, p->type);
        cf->types[env.n - 1] = p->type;
    }

    /* Bindings.  ResolveLocals's ordering is params first, then
     * bindings in source order, so we walk f->bindings here and
     * assume the corresponding slot in cf->types is env.n. */
    for (i = 0; i < f->n_bindings; i++) {
        const Binding *b = f->bindings[i];
        const Type    *t = check_expr(g, a, diag, &env, b->value);
        env_push(&env, b->name, t);
        cf->types[env.n - 1] = t;
    }

    /* Return expression. */
    cf->return_type = check_expr_expecting(g, a, diag, &env,
                                           f->return_expr,
                                           f->return_type);
    if (cf->return_type != NULL && f->return_type != NULL &&
        !type_equal(cf->return_type, f->return_type))
    {
        diag_emit(diag, f->return_expr->loc, DIAG_ERROR, "E140",
                  "flow return type mismatch: expected %s, got %s",
                  ast_type_to_string(a, f->return_type),
                  ast_type_to_string(a, cf->return_type));
    }

    return cf;
}


/* --------------------------------------------------------------------
 * Public entry point
 * -------------------------------------------------------------------- */

Checked *check_run(Arena *a, DiagStream *diag, Resolved *r)
{
    Checked *c = arena_alloc_zero(a, sizeof(*c));
    c->resolved = r;
    c->n_flows  = r->globals.n_flows;
    c->flows    = arena_alloc_zero(a, c->n_flows * sizeof(*c->flows));

    size_t i;
    for (i = 0; i < c->n_flows; i++) {
        c->flows[i] = check_flow(a, diag, &r->globals,
                                 r->globals.flows[i],
                                 &r->flow_locals[i]);
    }
    return c;
}


/* --------------------------------------------------------------------
 * Dump format
 * --------------------------------------------------------------------
 *
 *   Checked <source-file>
 *     Flow @line:col <name>
 *       Param   <name> : <type>
 *       Binding <name> : <type>
 *       Return        : <type>
 *
 * Types are rendered via ast_type_to_string.  A NULL type slot
 * prints as <error> (it means the local could not be inferred,
 * which means a prior diagnostic was emitted).
 */

static const char *type_str_or_error(Arena *a, const Type *t)
{
    return (t == NULL) ? "<error>" : ast_type_to_string(a, t);
}

void check_dump(FILE *out, Arena *a, const Checked *c)
{
    if (c == NULL || c->resolved == NULL || c->resolved->program == NULL) {
        fputs("<null-checked>\n", out);
        return;
    }
    fprintf(out, "Checked %s\n",
            c->resolved->program->source_file
                ? c->resolved->program->source_file : "<unknown>");

    size_t i, j;
    for (i = 0; i < c->n_flows; i++) {
        const FlowDecl     *f  = c->resolved->globals.flows[i];
        const ResolveLocals *ls = &c->resolved->flow_locals[i];
        const CheckedFlow   *cf = c->flows[i];
        fprintf(out, "  Flow @%d:%d %s\n", f->loc.line, f->loc.column, f->name);
        for (j = 0; j < ls->n; j++) {
            const char *kind = ls->is_binding[j] ? "Binding" : "Param  ";
            fprintf(out, "    %s %s : %s\n",
                    kind, ls->names[j], type_str_or_error(a, cf->types[j]));
        }
        fprintf(out, "    Return         : %s\n",
                type_str_or_error(a, cf->return_type));
    }
}
