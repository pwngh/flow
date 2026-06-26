/* src/resolve.c
 *
 * Name resolution pass.
 *
 *
 * Passes
 * ------
 *
 *   1. build_globals       -- register every top-level declaration
 *                             in the global symbol table, emit E120
 *                             on redeclaration.  First-wins.
 *
 *   2a. check_tool_effects -- emit E198 on a tool that lacks an
 *                             effect clause.
 *
 *   2b. check_variant_name_collisions
 *                          -- emit E125 when a sum variant's name
 *                             collides with a record type, tool, or
 *                             flow (cross-role collision).
 *
 *   2c. check_self_recursion
 *                          -- best-effort E129 when a flow's return
 *                             expression is an unconditional call to
 *                             itself.
 *
 *   2d. resolve_all_types  -- walk every Type node attached to the
 *                             AST (Field types in TypeDecls, Param
 *                             types and return_types in ToolDecls
 *                             and FlowDecls), emit E123 on unknown
 *                             named types.
 *
 *   3. resolve_flow_body   -- for each FlowDecl, build the local
 *                             symbol table (parameters first, then
 *                             bindings in source order) and walk
 *                             every Expr node, emit E122 (unknown
 *                             tool or flow), E124 (unknown identifier
 *                             head), E128 (unknown variant or type in
 *                             a construct), E127 (ambiguous variant),
 *                             and E120 (binding name collision).
 *
 *
 * What the resolver does NOT do
 * -----------------------------
 *
 * The resolver does not walk pipeline-stage internals.  A `where`
 * predicate's identifiers refer to fields of the upstream value's
 * element type; a `rank` field is likewise; a `pick using <model>`
 * model name is implementation-defined and resolved by the runtime.
 * All three require knowledge the resolver does not have -- the
 * upstream value's type is the type checker's job (step 4, E15x).
 *
 * The resolver also does not check Path tails (a.b.c with the head
 * resolved): segments beyond the first are field accesses whose
 * validity depends on the type of the preceding segment.  Checker.
 *
 * A flow without a trailing return is enforced by the grammar -- a
 * flow body that lacks a trailing expression is rejected as E110 at
 * parse time.  FlowDecl.return_expr is therefore never NULL here.
 *
 * E126 (rebind `it`) is currently unreachable. The lexer returns
 * KW_IT (not IDENT) for the literal "it", and the binding rule
 * requires IDENT for the bound name, so `it = ...` is rejected
 * with E110 at parse time. E126 stays reserved; if
 * a future flow form ever admits `it` as an ordinary identifier
 * the check belongs in resolve_flow_body's binding loop.
 *
 *
 * Symbol table layout
 * -------------------
 *
 * Three small arrays (types, tools, flows) form the global table.
 * Each per-flow local table is a single array of names. Lookup is
 * linear strcmp. At v0 scale (a handful of declarations per file,
 * a handful of bindings per flow) the constant factors and cache
 * behaviour beat any hash structure, and the absence of a hash
 * also keeps iteration order deterministic for diagnostics.
 * Revisit if realistic inputs grow to hundreds of decls per flow.
 */

#include "resolve.h"

#include <stdio.h>
#include <string.h>

#include "ast.h"
#include "util.h"


/* --------------------------------------------------------------------
 * Decl helpers (the AST's three Decl shapes share name + loc fields
 * but not through a common struct prefix, so we project them here)
 * -------------------------------------------------------------------- */

static const char *decl_name(const Decl *d)
{
    switch (d->kind) {
    case DECL_TYPE: return d->as.type_decl->name;
    case DECL_TOOL: return d->as.tool_decl->name;
    case DECL_FLOW: return d->as.flow_decl->name;
    }
    return NULL;  /* unreachable; all kinds handled above */
}

static SrcLoc decl_loc(const Decl *d)
{
    SrcLoc zero = {0, 0, 0};
    switch (d->kind) {
    case DECL_TYPE: return d->as.type_decl->loc;
    case DECL_TOOL: return d->as.tool_decl->loc;
    case DECL_FLOW: return d->as.flow_decl->loc;
    }
    return zero;  /* unreachable */
}


/* --------------------------------------------------------------------
 * Pass 1: build the global symbol table
 * -------------------------------------------------------------------- */

static int globals_contains(const ResolveGlobals *g, const char *name)
{
    size_t i;
    for (i = 0; i < g->n_types; i++) {
        if (strcmp(g->types[i]->name, name) == 0) return 1;
    }
    for (i = 0; i < g->n_tools; i++) {
        if (strcmp(g->tools[i]->name, name) == 0) return 1;
    }
    for (i = 0; i < g->n_flows; i++) {
        if (strcmp(g->flows[i]->name, name) == 0) return 1;
    }
    return 0;
}

const TypeDecl *globals_find_type(const ResolveGlobals *g, const char *name)
{
    size_t i;
    for (i = 0; i < g->n_types; i++) {
        if (strcmp(g->types[i]->name, name) == 0) return g->types[i];
    }
    return NULL;
}

const ToolDecl *globals_find_tool(const ResolveGlobals *g, const char *name)
{
    size_t i;
    for (i = 0; i < g->n_tools; i++) {
        if (strcmp(g->tools[i]->name, name) == 0) return g->tools[i];
    }
    return NULL;
}

static const FlowDecl *globals_find_flow(const ResolveGlobals *g, const char *name)
{
    size_t i;
    for (i = 0; i < g->n_flows; i++) {
        if (strcmp(g->flows[i]->name, name) == 0) return g->flows[i];
    }
    return NULL;
}

/* Location of the first declaration that registered `name`, across all three
 * global namespaces (first-wins, so exactly one matches). Lets a redeclaration
 * diagnostic point a "first declared here" note back at the original, the way
 * rustc threads a primary error to its related span. SRCLOC_NONE if absent. */
static SrcLoc globals_first_loc(const ResolveGlobals *g, const char *name)
{
    const TypeDecl *t = globals_find_type(g, name);
    if (t != NULL) return t->loc;
    const ToolDecl *to = globals_find_tool(g, name);
    if (to != NULL) return to->loc;
    const FlowDecl *f = globals_find_flow(g, name);
    if (f != NULL) return f->loc;
    return SRCLOC_NONE;
}

static void build_globals(Arena *a, DiagStream *diag,
                          ResolveGlobals *g, const Program *p)
{
    size_t n_t = 0, n_tools = 0, n_f = 0;
    size_t i;

    /* One pass to size; second pass to fill.  The arena allocates
     * each array once; redeclarations leave some slots unused, which
     * is fine. */
    for (i = 0; i < p->n_decls; i++) {
        switch (p->decls[i]->kind) {
        case DECL_TYPE: n_t++;     break;
        case DECL_TOOL: n_tools++; break;
        case DECL_FLOW: n_f++;     break;
        }
    }
    g->types   = arena_alloc(a, n_t     * sizeof(*g->types));
    g->tools   = arena_alloc(a, n_tools * sizeof(*g->tools));
    g->flows   = arena_alloc(a, n_f     * sizeof(*g->flows));
    g->n_types = g->n_tools = g->n_flows = 0;

    for (i = 0; i < p->n_decls; i++) {
        const Decl *d = p->decls[i];
        const char *name = decl_name(d);
        if (globals_contains(g, name)) {
            /* Notes bypass -fmax-errors, so only attach "first declared here"
             * when the error itself will be recorded — otherwise a capped run
             * (e.g. -fmax-errors=1 with three redeclarations) would emit an
             * orphan note for a dropped error. should_stop is true iff the
             * error we are about to emit will be dropped (the count has already
             * reached max_errors). */
            int recorded = !diag_should_stop(diag);
            diag_emit(diag, decl_loc(d), DIAG_ERROR, "E120",
                      "redeclaration of '%s'", name);
            SrcLoc first = globals_first_loc(g, name);
            if (recorded && first.line > 0)
                diag_note(diag, first, "'%s' first declared here", name);
            continue;
        }
        switch (d->kind) {
        case DECL_TYPE: g->types[g->n_types++] = d->as.type_decl; break;
        case DECL_TOOL: g->tools[g->n_tools++] = d->as.tool_decl; break;
        case DECL_FLOW: g->flows[g->n_flows++] = d->as.flow_decl; break;
        }
    }
}


/* --------------------------------------------------------------------
 * Pass 2a: every tool must carry an effect clause (E198)
 *
 * The grammar admits a missing clause so we can issue a single
 * targeted diagnostic per tool instead of a generic E110 syntax
 * error pointing at the next-token-after-the-arrow. The IR emitter
 * requires effect to be present; emit suppression downstream relies
 * on diag_error_count rising here.
 * -------------------------------------------------------------------- */

static void check_tool_effects(DiagStream *diag, const ResolveGlobals *g)
{
    size_t i;
    for (i = 0; i < g->n_tools; i++) {
        const ToolDecl *t = g->tools[i];
        if (t->effect == NULL) {
            diag_emit(diag, t->loc, DIAG_ERROR, "E198",
                      "tool '%s' missing 'effect' clause", t->name);
        }
    }
}


/* --------------------------------------------------------------------
 * Pass 2b: variant names must not collide with top-level entity
 * names (E125 cross-role collision)
 *
 * A variant lives inside its sum's namespace, but for bare
 * construction (`Variant { ... }`) the resolver walks the global
 * variant index. If a record type also shares the name, the bare
 * construct is ambiguous between record-construct and variant-
 * construct. We report it at decl time so the user fixes the source
 * rather than discovering it at every use site.
 * -------------------------------------------------------------------- */

static void check_variant_name_collisions(DiagStream *diag,
                                          const ResolveGlobals *g)
{
    size_t i, j;
    for (i = 0; i < g->n_types; i++) {
        const TypeDecl *td = g->types[i];
        if (td->kind != TYPE_DECL_SUM) continue;
        for (j = 0; j < td->n_variants; j++) {
            const Variant *v = td->variants[j];
            /* Only a *record* type collides: a same-named sum type is
             * fine because the ambiguity is specifically between bare
             * record-construct and variant-construct. The three checks
             * below have no early-out, but build_globals enforces a
             * unified global namespace (globals_contains rejects any
             * name already registered in types, tools, or flows with
             * E120, first-wins), so v->name occupies at most one global
             * role. At most one of the three lookups can match, so a
             * given variant raises E125 at most once. */
            /* Like E120, point a "declared here" note at the colliding
             * declaration rather than inlining its location in the message,
             * and guard the note on `recorded` so a capped run never leaves an
             * orphan note (see the E120 site above). */
            const TypeDecl *other = globals_find_type(g, v->name);
            if (other != NULL && other->kind == TYPE_DECL_RECORD) {
                int recorded = !diag_should_stop(diag);
                diag_emit(diag, v->loc, DIAG_ERROR, "E125",
                          "variant '%s' collides with record type '%s'",
                          v->name, other->name);
                if (recorded && other->loc.line > 0)
                    diag_note(diag, other->loc,
                              "record type '%s' declared here", other->name);
            }
            const ToolDecl *otool = globals_find_tool(g, v->name);
            if (otool != NULL) {
                int recorded = !diag_should_stop(diag);
                diag_emit(diag, v->loc, DIAG_ERROR, "E125",
                          "variant '%s' collides with tool of the same name",
                          v->name);
                if (recorded && otool->loc.line > 0)
                    diag_note(diag, otool->loc, "tool '%s' declared here", otool->name);
            }
            const FlowDecl *oflow = globals_find_flow(g, v->name);
            if (oflow != NULL) {
                int recorded = !diag_should_stop(diag);
                diag_emit(diag, v->loc, DIAG_ERROR, "E125",
                          "variant '%s' collides with flow of the same name",
                          v->name);
                if (recorded && oflow->loc.line > 0)
                    diag_note(diag, oflow->loc, "flow '%s' declared here", oflow->name);
            }
        }
    }
}


/* --------------------------------------------------------------------
 * Pass 2c: best-effort E129 — flat self-recursion without a base case
 *
 * The full check (every path reaches a base case) requires control-
 * flow analysis we do not do; this is best-effort.
 * The narrow case we catch: the flow's return_expr is itself a
 * direct call to the flow, with no surrounding conditional/match/
 * try-else that would shield it on at least one path.
 * -------------------------------------------------------------------- */

static void check_self_recursion(DiagStream *diag, const ResolveGlobals *g)
{
    size_t i;
    for (i = 0; i < g->n_flows; i++) {
        const FlowDecl *f = g->flows[i];
        const Expr *re = f->return_expr;
        if (re != NULL &&
            re->kind == EXPR_CALL &&
            strcmp(re->as.apply.name, f->name) == 0)
        {
            diag_emit(diag, re->loc, DIAG_ERROR, "E129",
                      "self-recursive flow '%s' must have a base case "
                      "(the recursive call appears unconditionally)",
                      f->name);
        }
    }
}


/* --------------------------------------------------------------------
 * Pass 2d: walk every Type node and verify named references
 * -------------------------------------------------------------------- */

static void resolve_type(DiagStream *diag,
                         const ResolveGlobals *g, const Type *t)
{
    if (t == NULL) return;
    switch (t->kind) {
    case TYPE_STRING:
    case TYPE_INT:
    case TYPE_FLOAT:
    case TYPE_BOOL:
        break;
    case TYPE_NAMED:
        if (globals_find_type(g, t->name) == NULL) {
            diag_emit(diag, t->loc, DIAG_ERROR, "E123",
                      "reference to unknown type '%s'", t->name);
        }
        break;
    case TYPE_LIST:
        resolve_type(diag, g, t->elem);
        break;
    }
}

static void resolve_all_types(DiagStream *diag,
                              const ResolveGlobals *g, const Program *p)
{
    size_t i, j;
    for (i = 0; i < p->n_decls; i++) {
        const Decl *d = p->decls[i];
        switch (d->kind) {
        case DECL_TYPE: {
            const TypeDecl *td = d->as.type_decl;
            if (td->kind == TYPE_DECL_SUM) {
                size_t k;
                for (j = 0; j < td->n_variants; j++) {
                    const Variant *v = td->variants[j];
                    for (k = 0; k < v->n_fields; k++) {
                        resolve_type(diag, g, v->fields[k]->type);
                    }
                }
            } else {
                for (j = 0; j < td->n_fields; j++) {
                    resolve_type(diag, g, td->fields[j]->type);
                }
            }
            break;
        }
        case DECL_TOOL: {
            const ToolDecl *t = d->as.tool_decl;
            for (j = 0; j < t->n_params; j++) {
                resolve_type(diag, g, t->params[j]->type);
            }
            resolve_type(diag, g, t->return_type);
            break;
        }
        case DECL_FLOW: {
            const FlowDecl *f = d->as.flow_decl;
            for (j = 0; j < f->n_params; j++) {
                resolve_type(diag, g, f->params[j]->type);
            }
            resolve_type(diag, g, f->return_type);
            break;
        }
        }
    }
}


/* --------------------------------------------------------------------
 * Pass 3: per-flow body walk
 * -------------------------------------------------------------------- */

typedef struct {
    const char **names;
    SrcLoc      *locs;
    int         *is_binding;
    size_t       n;
} LocalScope;

static int scope_index(const LocalScope *s, const char *name)
{
    size_t i;
    for (i = 0; i < s->n; i++) {
        if (strcmp(s->names[i], name) == 0) return (int)i;
    }
    return -1;
}

static void scope_push(LocalScope *s, const char *name, SrcLoc loc, int is_binding)
{
    /* No bounds check: resolve_flow_body sizes the arrays up front to
     * params + bindings + worst-case match binders, and every push
     * (param, binding, or transient match binder) consumes one of those
     * pre-counted slots, so s->n can never reach cap. */
    s->names     [s->n] = name;
    s->locs      [s->n] = loc;
    s->is_binding[s->n] = is_binding;
    s->n++;
}

static void resolve_expr_mode(DiagStream *diag, const ResolveGlobals *g,
                              LocalScope *s, Expr *e, int permissive_paths);

static void resolve_expr(DiagStream *diag, const ResolveGlobals *g,
                         LocalScope *s, Expr *e)
{
    resolve_expr_mode(diag, g, s, e, 0);
}

static void resolve_arg_mode(DiagStream *diag, const ResolveGlobals *g,
                             LocalScope *s, Arg *a, int permissive_paths)
{
    resolve_expr_mode(diag, g, s, a->value, permissive_paths);
}

static void resolve_expr_mode(DiagStream *diag, const ResolveGlobals *g,
                              LocalScope *s, Expr *e, int permissive_paths)
{
    if (e == NULL) return;
    switch (e->kind) {

    case EXPR_LITERAL:
        break;

    case EXPR_PATH:
        /* Head segment only; tail segments are field accesses left to
         * the checker (see file header). In permissive_paths mode the
         * head may name an element-type field inside a per-row stage,
         * so suppress E124 and let the checker (env extension in
         * check_per_row_body) catch genuinely missing names. */
        if (e->as.path.n > 0 && !permissive_paths) {
            const char *head = e->as.path.segments[0];
            if (scope_index(s, head) < 0) {
                const char *sug = flowc_suggest(head, s->names, s->n);
                if (sug != NULL) {
                    diag_emit(diag, e->loc, DIAG_ERROR, "E124",
                              "reference to unknown name '%s'; did you mean "
                              "'%s'?", head, sug);
                } else {
                    diag_emit(diag, e->loc, DIAG_ERROR, "E124",
                              "reference to unknown name '%s'", head);
                }
            }
        }
        break;

    case EXPR_CALL: {
        size_t i;
        /* `f(args)` resolves to a tool or a sub-flow. Tools take
         * precedence; cross-role collisions are E125 at decl time so a
         * name belongs to at most one of {tool, flow}. */
        const ToolDecl *tool = globals_find_tool(g, e->as.apply.name);
        const FlowDecl *flow = NULL;
        if (tool == NULL) {
            flow = globals_find_flow(g, e->as.apply.name);
            if (flow != NULL) {
                e->as.apply.resolved_flow = flow;
            } else {
                diag_emit(diag, e->loc, DIAG_ERROR, "E122",
                          "reference to unknown tool or flow '%s'",
                          e->as.apply.name);
            }
        }
        for (i = 0; i < e->as.apply.n_args; i++) {
            resolve_arg_mode(diag, g, s, e->as.apply.args[i],
                             permissive_paths);
        }
        break;
    }

    case EXPR_CONSTRUCT: {
        size_t i;
        /* Resolution dispatch:
         *   1. If the name matches a record type, this is a record
         *      construct. Done.
         *   2. Otherwise scan every sum type's variants for an exact
         *      name match. Zero matches → E128 unknown variant or type.
         *      One match → variant construct (record the sum + variant
         *      on the apply node for the checker/IR emitter). Two or
         *      more → E127 ambiguous variant, because a bare construct
         *      cannot say which sum it means. */
        const char *name = e->as.apply.name;
        const TypeDecl *record_td = globals_find_type(g, name);
        if (record_td != NULL && record_td->kind == TYPE_DECL_RECORD) {
            /* record construct — nothing to record; checker takes over */
        } else {
            const TypeDecl *first_sum = NULL;
            const Variant  *first_var = NULL;
            int matches = 0;
            size_t ti;
            for (ti = 0; ti < g->n_types; ti++) {
                const TypeDecl *td = g->types[ti];
                if (td->kind != TYPE_DECL_SUM) continue;
                size_t vi;
                for (vi = 0; vi < td->n_variants; vi++) {
                    if (strcmp(td->variants[vi]->name, name) == 0) {
                        if (matches == 0) {
                            first_sum = td;
                            first_var = td->variants[vi];
                        }
                        matches++;
                    }
                }
            }
            if (matches == 0) {
                diag_emit(diag, e->loc, DIAG_ERROR, "E128",
                          "unknown variant or type '%s'", name);
            } else if (matches > 1) {
                diag_emit(diag, e->loc, DIAG_ERROR, "E127",
                          "variant '%s' is ambiguous; qualify with the sum type",
                          name);
            } else {
                e->as.apply.resolved_sum     = first_sum;
                e->as.apply.resolved_variant = first_var;
            }
        }
        for (i = 0; i < e->as.apply.n_args; i++) {
            resolve_arg_mode(diag, g, s, e->as.apply.args[i],
                             permissive_paths);
        }
        break;
    }

    case EXPR_LIST_LITERAL: {
        size_t i;
        for (i = 0; i < e->as.list_literal.n_elements; i++) {
            resolve_expr_mode(diag, g, s, e->as.list_literal.elements[i],
                              permissive_paths);
        }
        break;
    }

    case EXPR_PIPELINE: {
        /* Source resolves in outer scope. Per-row stage payloads
         * (select, dedupe, any/all) walk in permissive_paths mode:
         * bare identifiers may name element-type fields, which the
         * checker resolves against the per-row env extension.
         * concat's argument is outer-scope, so non-permissive. */
        size_t i;
        resolve_expr(diag, g, s, e->as.pipeline.source);
        for (i = 0; i < e->as.pipeline.n_stages; i++) {
            Stage *st = e->as.pipeline.stages[i];
            switch (st->kind) {
                case STAGE_WHERE:
                case STAGE_RANK:
                case STAGE_TAKE:
                    break;
                case STAGE_SELECT:
                case STAGE_DEDUPE:
                    resolve_expr_mode(diag, g, s, st->body, 1);
                    break;
                case STAGE_CONCAT:
                    resolve_expr_mode(diag, g, s, st->body, 0);
                    break;
                case STAGE_TERMINAL:
                    if (st->terminal_kind == TERMINAL_ANY ||
                        st->terminal_kind == TERMINAL_ALL) {
                        resolve_expr_mode(diag, g, s, st->agg_pred, 1);
                    }
                    break;
            }
        }
        break;
    }

    case EXPR_CONDITIONAL: {
        size_t i;
        for (i = 0; i < e->as.conditional.n_branches; i++) {
            resolve_expr_mode(diag, g, s,
                              e->as.conditional.branches[i]->cond,
                              permissive_paths);
            resolve_expr_mode(diag, g, s,
                              e->as.conditional.branches[i]->consequent,
                              permissive_paths);
        }
        resolve_expr_mode(diag, g, s, e->as.conditional.else_expr,
                          permissive_paths);
        break;
    }

    case EXPR_TRY_ELSE: {
        resolve_expr_mode(diag, g, s, e->as.try_else.try_expr,
                          permissive_paths);
        resolve_expr_mode(diag, g, s, e->as.try_else.else_expr,
                          permissive_paths);
        break;
    }

    case EXPR_BINOP: {
        resolve_expr_mode(diag, g, s, e->as.binop.lhs, permissive_paths);
        resolve_expr_mode(diag, g, s, e->as.binop.rhs, permissive_paths);
        break;
    }

    case EXPR_UNOP: {
        resolve_expr_mode(diag, g, s, e->as.unop.operand, permissive_paths);
        break;
    }

    case EXPR_MATCH: {
        /* Scrutinee resolves before any arm binder is pushed, so an
         * arm's binder name is deliberately invisible to the value
         * being matched. */
        resolve_expr_mode(diag, g, s, e->as.match.scrutinee,
                          permissive_paths);

        /* Each arm runs with its binder (if any) pushed for the arm
         * body only, then popped. The arm body's scope is the outer
         * scope plus exactly one extra slot. */
        size_t i;
        for (i = 0; i < e->as.match.n_arms; i++) {
            MatchArm *arm = e->as.match.arms[i];
            size_t snapshot = s->n;
            if (arm->pattern.kind == PAT_VARIANT_BIND ||
                arm->pattern.kind == PAT_BIND) {
                scope_push(s,
                           arm->pattern.binder_name,
                           arm->pattern.loc,
                           1 /* binding-style scope entry */);
            }
            resolve_expr_mode(diag, g, s, arm->body, permissive_paths);
            s->n = snapshot;  /* pop this arm's binder */
        }
        /* Pattern variant names (e.g. `DeployCaused`) are not
         * resolved here — the checker validates them against the
         * scrutinee's sum type. */
        break;
    }

    }
}

static void resolve_flow_body(Arena *a, DiagStream *diag,
                              const ResolveGlobals *g,
                              const FlowDecl *f,
                              ResolveLocals *out)
{
    LocalScope s;
    size_t i;
    /* Worst-case binder count from match arms in this flow body.
     * Used as an upper bound on the scope-array capacity so that
     * scope_push for match binders never overflows. */
    size_t binder_slack = ast_count_flow_binders(f);
    size_t cap = f->n_params + f->n_bindings + binder_slack;

    s.names      = arena_alloc(a, cap * sizeof(*s.names));
    s.locs       = arena_alloc(a, cap * sizeof(*s.locs));
    s.is_binding = arena_alloc(a, cap * sizeof(*s.is_binding));
    s.n          = 0;

    /* Parameters first.  The parser records the implicit `it`
     * binding as params[0] with name == "it" for single-input
     * flows (FlowDecl.implicit_it); we treat it the same as any
     * other parameter here. */
    for (i = 0; i < f->n_params; i++) {
        const Param *p = f->params[i];
        scope_push(&s, p->name, p->loc, 0);
    }

    /* Bindings in source order.  Each binding's RHS sees the names
     * introduced by earlier bindings but not its own name. */
    for (i = 0; i < f->n_bindings; i++) {
        const Binding *b = f->bindings[i];
        resolve_expr(diag, g, &s, b->value);
        if (scope_index(&s, b->name) >= 0) {
            diag_emit(diag, b->loc, DIAG_ERROR, "E120",
                      "redeclaration of '%s'", b->name);
        } else {
            scope_push(&s, b->name, b->loc, 1);
        }
    }

    /* The return expression sees all locals. */
    resolve_expr(diag, g, &s, f->return_expr);

    /* Hand the arena-allocated arrays straight to the caller; they
     * outlive this frame because the arena owns them, so no copy. */
    out->names      = s.names;
    out->locs       = s.locs;
    out->is_binding = s.is_binding;
    out->n          = s.n;
}


/* --------------------------------------------------------------------
 * Public entry point
 * -------------------------------------------------------------------- */

Resolved *resolve_run(Arena *a, DiagStream *diag, Program *p)
{
    Resolved *r = arena_alloc_zero(a, sizeof(*r));
    size_t i;

    r->program = p;

    build_globals(a, diag, &r->globals, p);
    check_tool_effects(diag, &r->globals);
    check_variant_name_collisions(diag, &r->globals);
    check_self_recursion(diag, &r->globals);
    resolve_all_types(diag, &r->globals, p);

    r->n_flow_locals = r->globals.n_flows;
    r->flow_locals   = arena_alloc_zero(a,
                          r->n_flow_locals * sizeof(*r->flow_locals));
    for (i = 0; i < r->globals.n_flows; i++) {
        resolve_flow_body(a, diag, &r->globals, r->globals.flows[i],
                          &r->flow_locals[i]);
    }

    return r;
}


/* --------------------------------------------------------------------
 * Dump format
 * --------------------------------------------------------------------
 *
 *   Resolved <source-file>
 *     Globals
 *       Type @line:col <name>
 *       Tool @line:col <name>
 *       Flow @line:col <name>
 *     Flow @line:col <flow-name>
 *       Locals
 *         Param @line:col <name>
 *         Binding @line:col <name>
 *
 * Globals listed deduplicated (a redeclaration that triggered E120
 * appears only once, as the first declaration won).  Listed in
 * source order within each kind (types, then tools, then flows).
 * Locals listed in declaration order: parameters first, then
 * bindings in source order.
 */

void resolve_dump(FILE *out, const Resolved *r)
{
    size_t i, j;

    if (r == NULL || r->program == NULL) {
        fputs("<null-resolved>\n", out);
        return;
    }

    fprintf(out, "Resolved %s\n",
            r->program->source_file ? r->program->source_file : "<unknown>");
    fputs("  Globals\n", out);
    for (i = 0; i < r->globals.n_types; i++) {
        const TypeDecl *t = r->globals.types[i];
        fprintf(out, "    Type @%d:%d %s\n",
                t->loc.line, t->loc.column, t->name);
    }
    for (i = 0; i < r->globals.n_tools; i++) {
        const ToolDecl *t = r->globals.tools[i];
        fprintf(out, "    Tool @%d:%d %s\n",
                t->loc.line, t->loc.column, t->name);
    }
    for (i = 0; i < r->globals.n_flows; i++) {
        const FlowDecl *f = r->globals.flows[i];
        fprintf(out, "    Flow @%d:%d %s\n",
                f->loc.line, f->loc.column, f->name);
    }
    for (i = 0; i < r->globals.n_flows; i++) {
        const FlowDecl     *f  = r->globals.flows[i];
        const ResolveLocals *ls = &r->flow_locals[i];
        fprintf(out, "  Flow @%d:%d %s\n",
                f->loc.line, f->loc.column, f->name);
        fputs("    Locals\n", out);
        for (j = 0; j < ls->n; j++) {
            fprintf(out, "      %s @%d:%d %s\n",
                    ls->is_binding[j] ? "Binding" : "Param",
                    ls->locs[j].line, ls->locs[j].column,
                    ls->names[j]);
        }
    }
}
