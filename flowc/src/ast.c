/* src/ast.c
 *
 * Constructors and small helpers for the AST defined in src/ast.h.
 *
 * Every constructor allocates from the arena, zeroes the node so
 * that uninitialized union members never leak, sets the discriminant
 * and the per-kind payload, and returns the node.
 *
 * Allocation failure is fatal (FLOWC_ICE inside arena_alloc); every
 * other precondition is the caller's responsibility, documented in
 * the header next to the constructor declaration.
 */

#include "ast.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ====================================================================
 * Internal helper: allocate-and-zero of a typed node
 *
 * Wraps arena_alloc_zero so the call site reads as a type, not as a
 * sizeof. Aborts on out-of-memory (the arena does), so the return
 * is always valid.
 * ==================================================================== */

#define NEW(arena, T) ((T *)arena_alloc_zero((arena), sizeof(T)))


/* ====================================================================
 * Types
 * ==================================================================== */

Type *ast_type_primitive(Arena *a, TypeKind kind, SrcLoc loc)
{
    Type *t = NEW(a, Type);
    t->kind = kind;
    t->loc  = loc;
    return t;
}

Type *ast_type_named(Arena *a, const char *name, SrcLoc loc)
{
    Type *t = NEW(a, Type);
    t->kind = TYPE_NAMED;
    t->loc  = loc;
    t->name = name;
    return t;
}

Type *ast_type_list(Arena *a, Type *elem, SrcLoc loc)
{
    Type *t = NEW(a, Type);
    t->kind = TYPE_LIST;
    t->loc  = loc;
    t->elem = elem;
    return t;
}


/* ====================================================================
 * Records, parameters, arguments
 * ==================================================================== */

Field *ast_field(Arena *a, const char *name, Type *type, SrcLoc loc)
{
    Field *f = NEW(a, Field);
    f->loc  = loc;
    f->name = name;
    f->type = type;
    return f;
}

Variant *ast_variant(Arena *a, const char *name,
                     Field **fields, size_t n, SrcLoc loc)
{
    Variant *v = NEW(a, Variant);
    v->loc      = loc;
    v->name     = name;
    v->fields   = fields;
    v->n_fields = n;
    return v;
}

Param *ast_param(Arena *a, const char *name, Type *type, SrcLoc loc)
{
    Param *p = NEW(a, Param);
    p->loc  = loc;
    p->name = name;
    p->type = type;
    return p;
}

Arg *ast_arg(Arena *a, const char *field, Expr *value, SrcLoc loc)
{
    Arg *r = NEW(a, Arg);
    r->loc   = loc;
    r->field = field;   /* may be NULL for shorthand */
    r->value = value;
    return r;
}


/* ====================================================================
 * Predicates
 * ==================================================================== */

CmpExpr *ast_cmp(Arena *a, Expr *lhs, CmpOp op, Expr *rhs, SrcLoc loc)
{
    CmpExpr *c = NEW(a, CmpExpr);
    c->loc = loc;
    c->lhs = lhs;
    c->op  = op;
    c->rhs = rhs;
    return c;
}

Predicate *ast_pred_cmp(Arena *a, CmpExpr *cmp)
{
    Predicate *p = NEW(a, Predicate);
    p->kind = PRED_CMP;
    p->loc  = cmp->loc;
    p->cmp  = cmp;
    return p;
}

Predicate *ast_pred_and(Arena *a, Predicate *left, Predicate *right, SrcLoc loc)
{
    Predicate *p = NEW(a, Predicate);
    p->kind  = PRED_AND;
    p->loc   = loc;
    p->left  = left;
    p->right = right;
    return p;
}

Predicate *ast_pred_or(Arena *a, Predicate *left, Predicate *right, SrcLoc loc)
{
    Predicate *p = NEW(a, Predicate);
    p->kind  = PRED_OR;
    p->loc   = loc;
    p->left  = left;
    p->right = right;
    return p;
}


/* ====================================================================
 * Pipeline stages
 * ==================================================================== */

Stage *ast_stage_where(Arena *a, Predicate *p, SrcLoc loc)
{
    Stage *s = NEW(a, Stage);
    s->kind      = STAGE_WHERE;
    s->loc       = loc;
    s->predicate = p;
    return s;
}

Stage *ast_stage_rank(Arena *a, const char *field, SortDir dir, SrcLoc loc)
{
    Stage *s = NEW(a, Stage);
    s->kind       = STAGE_RANK;
    s->loc        = loc;
    s->sort_field = field;
    s->sort_dir   = dir;
    return s;
}

Stage *ast_stage_top(Arena *a, SrcLoc loc)
{
    Stage *s = NEW(a, Stage);
    s->kind          = STAGE_TERMINAL;
    s->loc           = loc;
    s->terminal_kind = TERMINAL_TOP;
    return s;
}

Stage *ast_stage_pick(Arena *a, const char *model, SrcLoc loc)
{
    Stage *s = NEW(a, Stage);
    s->kind          = STAGE_TERMINAL;
    s->loc           = loc;
    s->terminal_kind = TERMINAL_PICK;
    s->model         = model;
    return s;
}

Stage *ast_stage_select(Arena *a, Expr *body, SrcLoc loc)
{
    Stage *s = NEW(a, Stage);
    s->kind = STAGE_SELECT;
    s->loc  = loc;
    s->body = body;
    return s;
}

Stage *ast_stage_dedupe(Arena *a, Expr *key, SrcLoc loc)
{
    Stage *s = NEW(a, Stage);
    s->kind = STAGE_DEDUPE;
    s->loc  = loc;
    s->body = key;
    return s;
}

Stage *ast_stage_concat(Arena *a, Expr *other, SrcLoc loc)
{
    Stage *s = NEW(a, Stage);
    s->kind = STAGE_CONCAT;
    s->loc  = loc;
    s->body = other;
    return s;
}

Stage *ast_stage_take(Arena *a, long n, SrcLoc loc)
{
    Stage *s = NEW(a, Stage);
    s->kind       = STAGE_TAKE;
    s->loc        = loc;
    s->take_count = n;
    return s;
}

Stage *ast_stage_count(Arena *a, SrcLoc loc)
{
    Stage *s = NEW(a, Stage);
    s->kind          = STAGE_TERMINAL;
    s->loc           = loc;
    s->terminal_kind = TERMINAL_COUNT;
    return s;
}

static Stage *ast_stage_agg_field(Arena *a, TerminalKind k, const char *field,
                                  SrcLoc field_loc, SrcLoc loc)
{
    Stage *s = NEW(a, Stage);
    s->kind          = STAGE_TERMINAL;
    s->loc           = loc;
    s->terminal_kind = k;
    s->agg_field     = field;
    s->agg_field_loc = field_loc;
    return s;
}

Stage *ast_stage_sum(Arena *a, const char *field, SrcLoc field_loc, SrcLoc loc)
{ return ast_stage_agg_field(a, TERMINAL_SUM, field, field_loc, loc); }

Stage *ast_stage_max(Arena *a, const char *field, SrcLoc field_loc, SrcLoc loc)
{ return ast_stage_agg_field(a, TERMINAL_MAX, field, field_loc, loc); }

Stage *ast_stage_min(Arena *a, const char *field, SrcLoc field_loc, SrcLoc loc)
{ return ast_stage_agg_field(a, TERMINAL_MIN, field, field_loc, loc); }

static Stage *ast_stage_agg_pred(Arena *a, TerminalKind k,
                                 Expr *pred, SrcLoc loc)
{
    Stage *s = NEW(a, Stage);
    s->kind          = STAGE_TERMINAL;
    s->loc           = loc;
    s->terminal_kind = k;
    s->agg_pred      = pred;
    return s;
}

Stage *ast_stage_any(Arena *a, Expr *pred, SrcLoc loc)
{ return ast_stage_agg_pred(a, TERMINAL_ANY, pred, loc); }

Stage *ast_stage_all(Arena *a, Expr *pred, SrcLoc loc)
{ return ast_stage_agg_pred(a, TERMINAL_ALL, pred, loc); }


/* ====================================================================
 * Expressions
 * ==================================================================== */

Expr *ast_expr_int(Arena *a, long v, SrcLoc loc)
{
    Expr *e = NEW(a, Expr);
    e->kind             = EXPR_LITERAL;
    e->loc              = loc;
    e->as.literal.kind  = LIT_INT;
    e->as.literal.int_val = v;
    return e;
}

Expr *ast_expr_float(Arena *a, double v, SrcLoc loc)
{
    Expr *e = NEW(a, Expr);
    e->kind                = EXPR_LITERAL;
    e->loc                 = loc;
    e->as.literal.kind     = LIT_FLOAT;
    e->as.literal.float_val = v;
    return e;
}

Expr *ast_expr_list_literal(Arena *a, Expr **elements, size_t n, SrcLoc loc)
{
    Expr *e = NEW(a, Expr);
    e->kind                          = EXPR_LIST_LITERAL;
    e->loc                           = loc;
    e->as.list_literal.elements      = elements;
    e->as.list_literal.n_elements    = n;
    e->as.list_literal.element_type  = NULL;
    return e;
}

Expr *ast_expr_bool(Arena *a, int v, SrcLoc loc)
{
    Expr *e = NEW(a, Expr);
    e->kind                = EXPR_LITERAL;
    e->loc                 = loc;
    e->as.literal.kind     = LIT_BOOL;
    e->as.literal.bool_val = v ? 1 : 0;
    return e;
}

Expr *ast_expr_path(Arena *a, const char **segments,
                    const SrcLoc *seg_locs, size_t n, SrcLoc loc)
{
    Expr *e = NEW(a, Expr);
    e->kind             = EXPR_PATH;
    e->loc              = loc;
    e->as.path.segments = segments;
    e->as.path.seg_locs = seg_locs;
    e->as.path.n        = n;
    return e;
}

Expr *ast_expr_call(Arena *a, const char *name, Arg **args, size_t n, SrcLoc loc)
{
    Expr *e = NEW(a, Expr);
    e->kind                       = EXPR_CALL;
    e->loc                        = loc;
    e->as.apply.name              = name;
    e->as.apply.args              = args;
    e->as.apply.n_args            = n;
    return e;
}

Expr *ast_expr_construct(Arena *a, const char *name, Arg **args, size_t n, SrcLoc loc)
{
    Expr *e = NEW(a, Expr);
    e->kind                       = EXPR_CONSTRUCT;
    e->loc                        = loc;
    e->as.apply.name              = name;
    e->as.apply.args              = args;
    e->as.apply.n_args            = n;
    return e;
}

Expr *ast_expr_pipeline(Arena *a, Expr *source, Stage **stages, size_t n, SrcLoc loc)
{
    Expr *e = NEW(a, Expr);
    e->kind                 = EXPR_PIPELINE;
    e->loc                  = loc;
    e->as.pipeline.source   = source;
    e->as.pipeline.stages   = stages;
    e->as.pipeline.n_stages = n;
    return e;
}

Expr *ast_expr_match(Arena *a, Expr *scrutinee, MatchArm **arms, size_t n,
                     SrcLoc loc)
{
    Expr *e = NEW(a, Expr);
    e->kind                  = EXPR_MATCH;
    e->loc                   = loc;
    e->as.match.scrutinee    = scrutinee;
    e->as.match.arms         = arms;
    e->as.match.n_arms       = n;
    e->as.match.resolved_sum = NULL;
    return e;
}

Expr *ast_expr_conditional(Arena *a, CondBranch **branches, size_t n,
                           Expr *else_expr, SrcLoc loc)
{
    Expr *e = NEW(a, Expr);
    e->kind                        = EXPR_CONDITIONAL;
    e->loc                         = loc;
    e->as.conditional.branches     = branches;
    e->as.conditional.n_branches   = n;
    e->as.conditional.else_expr    = else_expr;
    return e;
}

Expr *ast_expr_try_else(Arena *a, Expr *try_expr, Expr *else_expr, SrcLoc loc)
{
    Expr *e = NEW(a, Expr);
    e->kind                  = EXPR_TRY_ELSE;
    e->loc                   = loc;
    e->as.try_else.try_expr  = try_expr;
    e->as.try_else.else_expr = else_expr;
    return e;
}

Expr *ast_expr_binop(Arena *a, BinopOp op, Expr *lhs, Expr *rhs, SrcLoc loc)
{
    Expr *e = NEW(a, Expr);
    e->kind         = EXPR_BINOP;
    e->loc          = loc;
    e->as.binop.op  = op;
    e->as.binop.lhs = lhs;
    e->as.binop.rhs = rhs;
    return e;
}

Expr *ast_expr_unop(Arena *a, UnopOp op, Expr *operand, SrcLoc loc)
{
    Expr *e = NEW(a, Expr);
    e->kind            = EXPR_UNOP;
    e->loc             = loc;
    e->as.unop.op      = op;
    e->as.unop.operand = operand;
    return e;
}

const char *ast_binop_str(BinopOp op)
{
    switch (op) {
    case BINOP_OR:  return "or";
    case BINOP_AND: return "and";
    case BINOP_EQ:  return "==";
    case BINOP_NEQ: return "!=";
    case BINOP_LT:  return "<";
    case BINOP_GT:  return ">";
    case BINOP_LE:  return "<=";
    case BINOP_GE:  return ">=";
    case BINOP_ADD: return "+";
    case BINOP_SUB: return "-";
    case BINOP_MUL: return "*";
    case BINOP_DIV: return "/";
    case BINOP_MOD: return "%";
    }
    FLOWC_ICE("ast_binop_str: bad op %d", (int)op);
}

const char *ast_unop_str(UnopOp op)
{
    switch (op) {
    case UNOP_NEG: return "-";
    case UNOP_NOT: return "not";
    }
    FLOWC_ICE("ast_unop_str: bad op %d", (int)op);
}

CondBranch *ast_cond_branch(Arena *a, Expr *cond, Expr *consequent)
{
    CondBranch *b = NEW(a, CondBranch);
    b->cond       = cond;
    b->consequent = consequent;
    return b;
}

MatchArm *ast_match_arm(Arena *a, Pattern pattern, Expr *body, SrcLoc loc)
{
    MatchArm *m = NEW(a, MatchArm);
    m->loc     = loc;
    m->pattern = pattern;
    m->body    = body;
    return m;
}

Pattern ast_pat_wildcard(SrcLoc loc)
{
    Pattern p;
    p.kind             = PAT_WILDCARD;
    p.loc              = loc;
    p.variant_name     = NULL;
    p.binder_name      = NULL;
    p.resolved_variant = NULL;
    return p;
}

Pattern ast_pat_bind(const char *binder, SrcLoc loc)
{
    Pattern p;
    p.kind             = PAT_BIND;
    p.loc              = loc;
    p.variant_name     = NULL;
    p.binder_name      = binder;
    p.resolved_variant = NULL;
    return p;
}

Pattern ast_pat_variant(const char *name, SrcLoc loc)
{
    Pattern p;
    p.kind             = PAT_VARIANT;
    p.loc              = loc;
    p.variant_name     = name;
    p.binder_name      = NULL;
    p.resolved_variant = NULL;
    return p;
}

Pattern ast_pat_variant_bind(const char *name, const char *binder, SrcLoc loc)
{
    Pattern p;
    p.kind             = PAT_VARIANT_BIND;
    p.loc              = loc;
    p.variant_name     = name;
    p.binder_name      = binder;
    p.resolved_variant = NULL;
    return p;
}


/* ====================================================================
 * Bindings
 * ==================================================================== */

Binding *ast_binding(Arena *a, const char *name, Expr *value, SrcLoc loc)
{
    Binding *b = NEW(a, Binding);
    b->loc   = loc;
    b->name  = name;
    b->value = value;
    return b;
}


/* ====================================================================
 * Declarations
 * ==================================================================== */

TypeDecl *ast_type_decl_record(Arena *a, const char *name,
                               Field **fields, size_t n, SrcLoc loc)
{
    TypeDecl *t = NEW(a, TypeDecl);
    t->loc        = loc;
    t->name       = name;
    t->kind       = TYPE_DECL_RECORD;
    t->fields     = fields;
    t->n_fields   = n;
    return t;
}

TypeDecl *ast_type_decl_sum(Arena *a, const char *name,
                            Variant **variants, size_t n, SrcLoc loc)
{
    TypeDecl *t = NEW(a, TypeDecl);
    t->loc        = loc;
    t->name       = name;
    t->kind       = TYPE_DECL_SUM;
    t->variants   = variants;
    t->n_variants = n;
    return t;
}

EffectClause *ast_effect_clause(Arena *a, EffectLevel level,
                                const char *model_name,
                                RetryPolicy retry, SrcLoc loc)
{
    EffectClause *e = NEW(a, EffectClause);
    e->loc        = loc;
    e->level      = level;
    e->model_name = model_name;
    e->retry      = retry;
    return e;
}

ToolDecl *ast_tool_decl(Arena *a, const char *name,
                        Param **params, size_t n,
                        Type *return_type,
                        EffectClause *effect, SrcLoc loc)
{
    ToolDecl *t = NEW(a, ToolDecl);
    t->loc         = loc;
    t->name        = name;
    t->params      = params;
    t->n_params    = n;
    t->return_type = return_type;
    t->effect      = effect;
    return t;
}

FlowDecl *ast_flow_decl(Arena *a, const char *name,
                        int implicit_it, Param **params, size_t n_params,
                        Type *return_type,
                        Binding **bindings, size_t n_bindings,
                        Expr *return_expr,
                        SrcLoc loc)
{
    FlowDecl *f = NEW(a, FlowDecl);
    f->loc          = loc;
    f->name         = name;
    f->implicit_it  = implicit_it;
    f->params       = params;
    f->n_params     = n_params;
    f->return_type  = return_type;
    f->bindings     = bindings;
    f->n_bindings   = n_bindings;
    f->return_expr  = return_expr;
    return f;
}

Decl *ast_decl_type(Arena *a, TypeDecl *t)
{
    Decl *d = NEW(a, Decl);
    d->kind         = DECL_TYPE;
    d->as.type_decl = t;
    return d;
}

Decl *ast_decl_tool(Arena *a, ToolDecl *t)
{
    Decl *d = NEW(a, Decl);
    d->kind         = DECL_TOOL;
    d->as.tool_decl = t;
    return d;
}

Decl *ast_decl_flow(Arena *a, FlowDecl *f)
{
    Decl *d = NEW(a, Decl);
    d->kind         = DECL_FLOW;
    d->as.flow_decl = f;
    return d;
}

Program *ast_program(Arena *a, const char *source_file,
                     Decl **decls, size_t n)
{
    Program *p = NEW(a, Program);
    p->source_file = source_file;
    p->decls       = decls;
    p->n_decls     = n;
    return p;
}


/* ====================================================================
 * AstList — growable scratch array
 *
 * Backed by malloc, not the arena, because the geometric growth
 * pattern wastes arena space (every realloc orphans the previous
 * allocation). Lifetime is short: a single grammar production.
 *
 * Growth is 2x from an initial capacity of 8. Realistic n (e.g. the
 * bindings in a flow body) stays small, so 8 rarely grows.
 *
 * Allocation failure goes through FLOWC_ICE: the scratch lists are
 * on the critical path of parsing and there is no useful recovery.
 * ==================================================================== */

#define AST_LIST_INIT_CAP 8

void ast_list_init(AstList *xs)
{
    xs->items = NULL;
    xs->n     = 0;
    xs->cap   = 0;
}

void ast_list_push(AstList *xs, void *item)
{
    if (xs->n == xs->cap) {
        size_t new_cap = xs->cap == 0 ? AST_LIST_INIT_CAP : xs->cap * 2u;
        void **new_items;
        /* Guard the doubling and the byte multiply against wraparound:
         * a wrapped new_cap would under-allocate the backing store.
         * Unreachable for realistic input (n is bounded by source-file
         * size). */
        if (new_cap <= xs->cap || new_cap > SIZE_MAX / sizeof(void *)) {
            FLOWC_ICE("ast_list_push: capacity overflow (cap %zu)", xs->cap);
        }
        new_items = realloc(xs->items, new_cap * sizeof(void *));
        if (new_items == NULL) {
            FLOWC_ICE("ast_list_push: realloc failed (cap %zu)", new_cap);
        }
        xs->items = new_items;
        xs->cap   = new_cap;
    }
    xs->items[xs->n++] = item;
}

void **ast_list_finalize(AstList *xs, Arena *a)
{
    /* n==0 still returns a unique non-NULL pointer (arena_alloc bumps 0 to
     * 1 byte); the guard skips the memcpy because xs->items may be NULL and
     * memcpy(_, NULL, 0) is UB. Callers keep the pointer as a valid empty
     * array and never deref it. */
    void **out = arena_alloc(a, xs->n * sizeof(void *));
    if (xs->n != 0) {
        memcpy(out, xs->items, xs->n * sizeof(void *));
    }
    free(xs->items);
    xs->items = NULL;
    xs->cap   = 0;
    /* xs->n is preserved so the caller can pass it alongside `out` */
    return out;
}

void ast_list_free(AstList *xs)
{
    free(xs->items);
    xs->items = NULL;
    xs->n     = 0;
    xs->cap   = 0;
}


/* ====================================================================
 * Type printing
 *
 * Canonical form. Uses a growable string buffer and a recursive
 * helper because nested list types are recursive in source. No
 * nesting limit: the helper recurses per `[...]` level and the
 * buffer grows on demand.
 * ==================================================================== */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_grow(StrBuf *sb, size_t need)
{
    size_t want;
    size_t new_cap;
    char  *p;
    /* Minimum capacity needed for the new bytes plus the NUL. Guard
     * the addition against wraparound; unreachable for realistic type
     * names but a wrapped `want` would under-grow the buffer. */
    if (need > SIZE_MAX - 1u || sb->len > SIZE_MAX - 1u - need) {
        FLOWC_ICE("ast_type_to_string: size overflow (len %zu need %zu)",
                  sb->len, need);
    }
    want = sb->len + need + 1u;
    if (sb->cap >= want) return;
    new_cap = sb->cap ? sb->cap * 2u : 64u;
    /* Double until large enough, but bound the doubling so it cannot
     * wrap to a smaller value and spin; clamp to `want` instead. */
    while (new_cap < want) {
        if (new_cap > SIZE_MAX / 2u) {
            new_cap = want;
            break;
        }
        new_cap *= 2u;
    }
    p = realloc(sb->data, new_cap);
    if (p == NULL) {
        FLOWC_ICE("ast_type_to_string: realloc failed (cap %zu)", new_cap);
    }
    sb->data = p;
    sb->cap  = new_cap;
}

static void sb_puts(StrBuf *sb, const char *s)
{
    size_t n = strlen(s);
    sb_grow(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void type_to_string(StrBuf *sb, const Type *t)
{
    switch (t->kind) {
    case TYPE_STRING: sb_puts(sb, "string"); break;
    case TYPE_INT:    sb_puts(sb, "int");    break;
    case TYPE_FLOAT:  sb_puts(sb, "float");  break;
    case TYPE_BOOL:   sb_puts(sb, "bool");   break;
    case TYPE_NAMED:  sb_puts(sb, t->name);  break;
    case TYPE_LIST:
        sb_puts(sb, "[");
        type_to_string(sb, t->elem);
        sb_puts(sb, "]");
        break;
    }
}

const char *ast_type_to_string(Arena *a, const Type *t)
{
    StrBuf sb = { NULL, 0, 0 };
    type_to_string(&sb, t);
    char *out = arena_strndup(a, sb.data ? sb.data : "", sb.len);
    free(sb.data);
    return out;
}


/* ====================================================================
 * Operator names
 * ==================================================================== */

const char *ast_cmp_op_str(CmpOp op)
{
    switch (op) {
    case CMP_LE:  return "<=";
    case CMP_GE:  return ">=";
    case CMP_LT:  return "<";
    case CMP_GT:  return ">";
    case CMP_EQ:  return "==";
    case CMP_NEQ: return "!=";
    }
    /* The switch is exhaustive over CmpOp, so this is unreachable. It
     * exists because C does not treat an exhaustive enum switch as
     * covering every path, so without a statement here the function
     * would trip -Wreturn-type; using FLOWC_ICE instead of a default:
     * label keeps -Wswitch able to flag any newly added CmpOp. */
    FLOWC_ICE("ast_cmp_op_str: bad op %d", (int)op);
}

const char *ast_sort_dir_str(SortDir d)
{
    switch (d) {
    case SORT_ASC:  return "asc";
    case SORT_DESC: return "desc";
    }
    FLOWC_ICE("ast_sort_dir_str: bad dir %d", (int)d);
}


/* ====================================================================
 * Match binder counter
 * ==================================================================== */

/* Count match binders reachable through a where-predicate. The
 * predicate is a tree of PRED_AND/PRED_OR nodes whose leaves are
 * PRED_CMP comparisons; each comparison's lhs/rhs is a primary_expr
 * (see parse.y pred_cmp_op_expr), which the grammar permits to be a
 * match / conditional / try expression carrying PAT_BIND or
 * PAT_VARIANT_BIND binders. Those operands are walked here so the
 * upper bound stays correct if the resolver/checker ever resolve
 * predicate operands for real. */
static size_t count_pred_binders(const Predicate *p)
{
    if (p == NULL) return 0;
    switch (p->kind) {
    case PRED_CMP:
        if (p->cmp == NULL) return 0;
        return ast_count_match_binders(p->cmp->lhs) +
               ast_count_match_binders(p->cmp->rhs);
    case PRED_AND:
    case PRED_OR:
        return count_pred_binders(p->left) +
               count_pred_binders(p->right);
    }
    return 0;
}

size_t ast_count_match_binders(const Expr *e)
{
    if (e == NULL) return 0;
    size_t n = 0;
    size_t i;
    switch (e->kind) {
    case EXPR_LITERAL:
    case EXPR_PATH:
        return 0;
    case EXPR_LIST_LITERAL:
        for (i = 0; i < e->as.list_literal.n_elements; i++) {
            n += ast_count_match_binders(e->as.list_literal.elements[i]);
        }
        return n;
    case EXPR_CALL:
    case EXPR_CONSTRUCT:
        for (i = 0; i < e->as.apply.n_args; i++) {
            n += ast_count_match_binders(e->as.apply.args[i]->value);
        }
        return n;
    case EXPR_PIPELINE:
        n += ast_count_match_binders(e->as.pipeline.source);
        /* Stages can carry expressions that might nest matches: the
         * where predicate operands, the select body, dedupe key,
         * concat other, and any/all predicate. STAGE_RANK (sort field
         * name) and STAGE_TAKE (int literal) carry no expressions. */
        for (i = 0; i < e->as.pipeline.n_stages; i++) {
            const Stage *st = e->as.pipeline.stages[i];
            switch (st->kind) {
                case STAGE_WHERE:
                    n += count_pred_binders(st->predicate);
                    break;
                case STAGE_RANK:
                case STAGE_TAKE:
                    break;
                case STAGE_SELECT:
                case STAGE_DEDUPE:
                case STAGE_CONCAT:
                    n += ast_count_match_binders(st->body);
                    break;
                case STAGE_TERMINAL:
                    if (st->terminal_kind == TERMINAL_ANY ||
                        st->terminal_kind == TERMINAL_ALL) {
                        n += ast_count_match_binders(st->agg_pred);
                    }
                    break;
            }
        }
        return n;
    case EXPR_MATCH:
        n += ast_count_match_binders(e->as.match.scrutinee);
        for (i = 0; i < e->as.match.n_arms; i++) {
            const MatchArm *arm = e->as.match.arms[i];
            if (arm->pattern.kind == PAT_VARIANT_BIND ||
                arm->pattern.kind == PAT_BIND) n++;
            n += ast_count_match_binders(arm->body);
        }
        return n;
    case EXPR_CONDITIONAL:
        for (i = 0; i < e->as.conditional.n_branches; i++) {
            n += ast_count_match_binders(e->as.conditional.branches[i]->cond);
            n += ast_count_match_binders(e->as.conditional.branches[i]->consequent);
        }
        n += ast_count_match_binders(e->as.conditional.else_expr);
        return n;
    case EXPR_TRY_ELSE:
        n += ast_count_match_binders(e->as.try_else.try_expr);
        n += ast_count_match_binders(e->as.try_else.else_expr);
        return n;
    case EXPR_BINOP:
        n += ast_count_match_binders(e->as.binop.lhs);
        n += ast_count_match_binders(e->as.binop.rhs);
        return n;
    case EXPR_UNOP:
        n += ast_count_match_binders(e->as.unop.operand);
        return n;
    }
    return n;
}

size_t ast_count_flow_binders(const FlowDecl *f)
{
    size_t n = 0;
    size_t i;
    for (i = 0; i < f->n_bindings; i++) {
        n += ast_count_match_binders(f->bindings[i]->value);
    }
    n += ast_count_match_binders(f->return_expr);
    return n;
}
