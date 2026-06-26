/* src/ast_dump.c
 *
 * Pretty-print the AST in a stable, golden-testable form.
 *
 *
 * Format
 * ------
 *
 * One node per line, depth-first, two-space indentation per level.
 * Each line is:
 *
 *     <indent><NodeKind>[ @line:col][ <inline payload>]
 *
 * Payloads:
 *
 *   Program       <source-file>
 *   TypeDecl      <name> (record | sum)
 *     Field       <name> : <type>         (record kind)
 *     Variant     <name>                  (sum kind)
 *       Field     <name> : <type>
 *   ToolDecl      <name>
 *     Param       <name> : <type>
 *     return-type: <type>
 *     effect: <level>[("<model>")][ (retry: ...)]   (when present)
 *   FlowDecl      <name> [(implicit it)]
 *     Param       <name> : <type>      (zero or more)
 *     return-type: <type>
 *     Binding     <name>
 *       <expr>
 *     return:
 *       <expr>
 *
 * Expressions — one label per AST expression kind.  Only a narrow slice
 * of this format is pinned by a golden fixture: parse_smoke.expected.out
 * is the sole test that compares ast_dump bytes, and it exercises just
 * Program / record TypeDecl / Field / ToolDecl (deterministic effect) /
 * FlowDecl (implicit it) / Param / Binding / Call / shorthand Arg / Path /
 * Pipeline / source / Stage-Where / Predicate-Cmp / Cmp / Literal-Int /
 * Stage-Rank / Stage-Terminal (pick).  Everything else below — ListLiteral,
 * Construct, Conditional, TryElse, Match, Binop, Unop, the other stages,
 * sum-type variants, and every effect retry sub-clause — has no golden
 * coverage, so those exact bytes are defined here, not asserted anywhere:
 *
 *   Literal-Int   <int>
 *   Literal-Float <float>
 *   Literal-Bool  true | false
 *   ListLiteral   n=<count>        element <expr>s follow as children
 *   Path          a.b.c
 *   Call          <name>
 *     Arg         field=<name> | (shorthand)
 *       <expr>
 *   Construct     <name>
 *     Arg ...
 *   Conditional                    Branch: (cond: / consequent:) per branch,
 *                                  then else:
 *   TryElse                        try: / else: children
 *   Match                          scrutinee:, then one Arm per case with a
 *                                  pattern suffix: _ | (bind <name>) |
 *                                  <variant> | <variant> <binder>
 *   Binop '<op>'                   lhs / rhs children follow
 *   Unop  '<op>'                   operand child follows
 *   Pipeline
 *     source:
 *       <expr>
 *     Stage-Where        <predicate>
 *     Stage-Select / Stage-DedupeBy / Stage-Concat   per-stage <expr>
 *     Stage-Rank         field=<name> asc | desc
 *     Stage-Take         count=<n>
 *     Stage-Terminal     top | pick using=<model> | count |
 *                        sum | max | min field=<name> | any | all
 *
 * Predicates:
 *
 *   Predicate-Cmp
 *     Cmp <op>
 *       <expr>          (lhs)
 *       <expr>          (rhs)
 *   Predicate-And / Predicate-Or
 *     <predicate>       (left)
 *     <predicate>       (right)
 *
 *
 * Determinism
 * -----------
 *
 * Locations come straight from the parser.  Types render via
 * ast_type_to_string().  Floats use flowc_format_double (shortest
 * round-trip, the same as the IR, so the dump and the IR never
 * disagree on a value).  Identifiers
 * are unquoted because Flow identifiers cannot contain whitespace
 * or punctuation that would conflict with the structure.
 */

#include "ast_dump.h"

#include <stdio.h>

#include "ast.h"
#include "util.h"


/* --------------------------------------------------------------------
 * Forward declarations (the AST is mutually recursive)
 * -------------------------------------------------------------------- */

static void dump_decl     (FILE *out, Arena *arena, int indent, const Decl      *d);
static void dump_type_decl(FILE *out, Arena *arena, int indent, const TypeDecl  *t);
static void dump_tool_decl(FILE *out, Arena *arena, int indent, const ToolDecl  *t);
static void dump_flow_decl(FILE *out, Arena *arena, int indent, const FlowDecl  *f);
static void dump_field    (FILE *out, Arena *arena, int indent, const Field     *f);
static void dump_param    (FILE *out, Arena *arena, int indent, const Param     *p);
static void dump_binding  (FILE *out, Arena *arena, int indent, const Binding   *b);
static void dump_arg      (FILE *out, Arena *arena, int indent, const Arg       *a);
static void dump_expr     (FILE *out, Arena *arena, int indent, const Expr      *e);
static void dump_stage    (FILE *out, Arena *arena, int indent, const Stage     *s);
static void dump_predicate(FILE *out, Arena *arena, int indent, const Predicate *p);
static void dump_cmp      (FILE *out, Arena *arena, int indent, const CmpExpr   *c);


/* --------------------------------------------------------------------
 * Small helpers
 * -------------------------------------------------------------------- */

static void dump_indent(FILE *out, int indent)
{
    int i;
    for (i = 0; i < indent; i++) {
        fputs("  ", out);
    }
}

static void dump_loc(FILE *out, SrcLoc loc)
{
    fprintf(out, " @%d:%d", loc.line, loc.column);
}

static void dump_type_inline(FILE *out, Arena *arena, const Type *t)
{
    if (t == NULL) {
        fputs("<null-type>", out);
    } else {
        fputs(ast_type_to_string(arena, t), out);
    }
}


/* --------------------------------------------------------------------
 * Field / Param
 * -------------------------------------------------------------------- */

static void dump_field(FILE *out, Arena *arena, int indent, const Field *f)
{
    dump_indent(out, indent);
    fputs("Field", out);
    dump_loc(out, f->loc);
    fprintf(out, " %s : ", f->name);
    dump_type_inline(out, arena, f->type);
    fputc('\n', out);
}

static void dump_param(FILE *out, Arena *arena, int indent, const Param *p)
{
    dump_indent(out, indent);
    fputs("Param", out);
    dump_loc(out, p->loc);
    fprintf(out, " %s : ", p->name);
    dump_type_inline(out, arena, p->type);
    fputc('\n', out);
}


/* --------------------------------------------------------------------
 * Expressions (mutually recursive with Predicate via CmpExpr)
 * -------------------------------------------------------------------- */

static void dump_cmp(FILE *out, Arena *arena, int indent, const CmpExpr *c)
{
    dump_indent(out, indent);
    fputs("Cmp", out);
    dump_loc(out, c->loc);
    fprintf(out, " %s\n", ast_cmp_op_str(c->op));
    dump_expr(out, arena, indent + 1, c->lhs);
    dump_expr(out, arena, indent + 1, c->rhs);
}

static void dump_predicate(FILE *out, Arena *arena, int indent, const Predicate *p)
{
    dump_indent(out, indent);
    switch (p->kind) {
    case PRED_CMP:
        fputs("Predicate-Cmp", out);
        dump_loc(out, p->loc);
        fputc('\n', out);
        dump_cmp(out, arena, indent + 1, p->cmp);
        break;
    case PRED_AND:
        fputs("Predicate-And", out);
        dump_loc(out, p->loc);
        fputc('\n', out);
        dump_predicate(out, arena, indent + 1, p->left);
        dump_predicate(out, arena, indent + 1, p->right);
        break;
    case PRED_OR:
        fputs("Predicate-Or", out);
        dump_loc(out, p->loc);
        fputc('\n', out);
        dump_predicate(out, arena, indent + 1, p->left);
        dump_predicate(out, arena, indent + 1, p->right);
        break;
    }
}

static void dump_stage(FILE *out, Arena *arena, int indent, const Stage *s)
{
    dump_indent(out, indent);
    switch (s->kind) {
    case STAGE_WHERE:
        fputs("Stage-Where", out);
        dump_loc(out, s->loc);
        fputc('\n', out);
        dump_predicate(out, arena, indent + 1, s->predicate);
        break;
    case STAGE_RANK:
        fputs("Stage-Rank", out);
        dump_loc(out, s->loc);
        fprintf(out, " field=%s %s\n",
                s->sort_field, ast_sort_dir_str(s->sort_dir));
        break;
    case STAGE_SELECT:
        fputs("Stage-Select", out);
        dump_loc(out, s->loc);
        fputc('\n', out);
        dump_expr(out, arena, indent + 1, s->body);
        break;
    case STAGE_DEDUPE:
        fputs("Stage-DedupeBy", out);
        dump_loc(out, s->loc);
        fputc('\n', out);
        dump_expr(out, arena, indent + 1, s->body);
        break;
    case STAGE_CONCAT:
        fputs("Stage-Concat", out);
        dump_loc(out, s->loc);
        fputc('\n', out);
        dump_expr(out, arena, indent + 1, s->body);
        break;
    case STAGE_TAKE:
        fputs("Stage-Take", out);
        dump_loc(out, s->loc);
        fprintf(out, " count=%ld\n", s->take_count);
        break;
    case STAGE_TERMINAL:
        fputs("Stage-Terminal", out);
        dump_loc(out, s->loc);
        switch (s->terminal_kind) {
        case TERMINAL_TOP:
            fputs(" top\n", out);
            break;
        case TERMINAL_PICK:
            fprintf(out, " pick using=%s\n",
                    s->model ? s->model : "<null>");
            break;
        case TERMINAL_COUNT:
            fputs(" count\n", out);
            break;
        case TERMINAL_SUM:
            fprintf(out, " sum field=%s\n",
                    s->agg_field ? s->agg_field : "<null>");
            break;
        case TERMINAL_MAX:
            fprintf(out, " max field=%s\n",
                    s->agg_field ? s->agg_field : "<null>");
            break;
        case TERMINAL_MIN:
            fprintf(out, " min field=%s\n",
                    s->agg_field ? s->agg_field : "<null>");
            break;
        case TERMINAL_ANY:
            fputs(" any\n", out);
            dump_expr(out, arena, indent + 1, s->agg_pred);
            break;
        case TERMINAL_ALL:
            fputs(" all\n", out);
            dump_expr(out, arena, indent + 1, s->agg_pred);
            break;
        }
        break;
    }
}

static void dump_arg(FILE *out, Arena *arena, int indent, const Arg *a)
{
    dump_indent(out, indent);
    fputs("Arg", out);
    dump_loc(out, a->loc);
    if (a->field) {
        fprintf(out, " field=%s\n", a->field);
    } else {
        fputs(" (shorthand)\n", out);
    }
    dump_expr(out, arena, indent + 1, a->value);
}

static void dump_expr(FILE *out, Arena *arena, int indent, const Expr *e)
{
    if (e == NULL) {
        dump_indent(out, indent);
        fputs("<null-expr>\n", out);
        return;
    }
    dump_indent(out, indent);
    switch (e->kind) {
    case EXPR_LITERAL:
        switch (e->as.literal.kind) {
        case LIT_INT:
            fputs("Literal-Int", out);
            dump_loc(out, e->loc);
            fprintf(out, " %ld\n", e->as.literal.int_val);
            break;
        case LIT_FLOAT: {
            char fb[32];
            flowc_format_double(e->as.literal.float_val, fb, sizeof fb);
            fputs("Literal-Float", out);
            dump_loc(out, e->loc);
            fprintf(out, " %s\n", fb);
            break;
        }
        case LIT_BOOL:
            fputs("Literal-Bool", out);
            dump_loc(out, e->loc);
            fprintf(out, " %s\n",
                    e->as.literal.bool_val ? "true" : "false");
            break;
        }
        break;
    case EXPR_LIST_LITERAL: {
        size_t i;
        fputs("ListLiteral", out);
        dump_loc(out, e->loc);
        fprintf(out, " n=%zu\n", e->as.list_literal.n_elements);
        for (i = 0; i < e->as.list_literal.n_elements; i++) {
            dump_expr(out, arena, indent + 1, e->as.list_literal.elements[i]);
        }
        break;
    }
    case EXPR_PATH: {
        size_t i;
        fputs("Path", out);
        dump_loc(out, e->loc);
        fputc(' ', out);
        for (i = 0; i < e->as.path.n; i++) {
            if (i > 0) {
                fputc('.', out);
            }
            fputs(e->as.path.segments[i], out);
        }
        fputc('\n', out);
        break;
    }
    case EXPR_CALL: {
        size_t i;
        fputs("Call", out);
        dump_loc(out, e->loc);
        fprintf(out, " %s\n", e->as.apply.name);
        for (i = 0; i < e->as.apply.n_args; i++) {
            dump_arg(out, arena, indent + 1, e->as.apply.args[i]);
        }
        break;
    }
    case EXPR_CONSTRUCT: {
        size_t i;
        fputs("Construct", out);
        dump_loc(out, e->loc);
        fprintf(out, " %s\n", e->as.apply.name);
        for (i = 0; i < e->as.apply.n_args; i++) {
            dump_arg(out, arena, indent + 1, e->as.apply.args[i]);
        }
        break;
    }
    case EXPR_PIPELINE: {
        size_t i;
        fputs("Pipeline", out);
        dump_loc(out, e->loc);
        fputc('\n', out);
        dump_indent(out, indent + 1);
        fputs("source:\n", out);
        dump_expr(out, arena, indent + 2, e->as.pipeline.source);
        for (i = 0; i < e->as.pipeline.n_stages; i++) {
            dump_stage(out, arena, indent + 1, e->as.pipeline.stages[i]);
        }
        break;
    }
    case EXPR_CONDITIONAL: {
        size_t i;
        fputs("Conditional", out);
        dump_loc(out, e->loc);
        fputc('\n', out);
        for (i = 0; i < e->as.conditional.n_branches; i++) {
            dump_indent(out, indent + 1);
            fputs("Branch:\n", out);
            dump_indent(out, indent + 2);
            fputs("cond:\n", out);
            dump_expr(out, arena, indent + 3,
                      e->as.conditional.branches[i]->cond);
            dump_indent(out, indent + 2);
            fputs("consequent:\n", out);
            dump_expr(out, arena, indent + 3,
                      e->as.conditional.branches[i]->consequent);
        }
        dump_indent(out, indent + 1);
        fputs("else:\n", out);
        dump_expr(out, arena, indent + 2, e->as.conditional.else_expr);
        break;
    }
    case EXPR_TRY_ELSE: {
        fputs("TryElse", out);
        dump_loc(out, e->loc);
        fputc('\n', out);
        dump_indent(out, indent + 1);
        fputs("try:\n", out);
        dump_expr(out, arena, indent + 2, e->as.try_else.try_expr);
        dump_indent(out, indent + 1);
        fputs("else:\n", out);
        dump_expr(out, arena, indent + 2, e->as.try_else.else_expr);
        break;
    }
    case EXPR_BINOP: {
        fprintf(out, "Binop '%s'", ast_binop_str(e->as.binop.op));
        dump_loc(out, e->loc);
        fputc('\n', out);
        dump_expr(out, arena, indent + 1, e->as.binop.lhs);
        dump_expr(out, arena, indent + 1, e->as.binop.rhs);
        break;
    }
    case EXPR_UNOP: {
        fprintf(out, "Unop '%s'", ast_unop_str(e->as.unop.op));
        dump_loc(out, e->loc);
        fputc('\n', out);
        dump_expr(out, arena, indent + 1, e->as.unop.operand);
        break;
    }
    case EXPR_MATCH: {
        size_t i;
        fputs("Match", out);
        dump_loc(out, e->loc);
        fputc('\n', out);
        dump_indent(out, indent + 1);
        fputs("scrutinee:\n", out);
        dump_expr(out, arena, indent + 2, e->as.match.scrutinee);
        for (i = 0; i < e->as.match.n_arms; i++) {
            const MatchArm *arm = e->as.match.arms[i];
            dump_indent(out, indent + 1);
            fputs("Arm", out);
            dump_loc(out, arm->loc);
            switch (arm->pattern.kind) {
                case PAT_WILDCARD:
                    fputs(" _\n", out);
                    break;
                case PAT_BIND:
                    fprintf(out, " (bind %s)\n", arm->pattern.binder_name);
                    break;
                case PAT_VARIANT:
                    fprintf(out, " %s\n", arm->pattern.variant_name);
                    break;
                case PAT_VARIANT_BIND:
                    fprintf(out, " %s %s\n",
                            arm->pattern.variant_name,
                            arm->pattern.binder_name);
                    break;
            }
            dump_expr(out, arena, indent + 2, arm->body);
        }
        break;
    }
    }
}


/* --------------------------------------------------------------------
 * Bindings and top-level declarations
 * -------------------------------------------------------------------- */

static void dump_binding(FILE *out, Arena *arena, int indent, const Binding *b)
{
    dump_indent(out, indent);
    fputs("Binding", out);
    dump_loc(out, b->loc);
    fprintf(out, " %s\n", b->name);
    dump_expr(out, arena, indent + 1, b->value);
}

static void dump_variant(FILE *out, Arena *arena, int indent, const Variant *v)
{
    size_t i;
    dump_indent(out, indent);
    fputs("Variant", out);
    dump_loc(out, v->loc);
    fprintf(out, " %s\n", v->name);
    for (i = 0; i < v->n_fields; i++) {
        dump_field(out, arena, indent + 1, v->fields[i]);
    }
}

static void dump_type_decl(FILE *out, Arena *arena, int indent, const TypeDecl *t)
{
    size_t i;
    dump_indent(out, indent);
    fputs("TypeDecl", out);
    dump_loc(out, t->loc);
    fprintf(out, " %s (%s)\n",
            t->name, t->kind == TYPE_DECL_SUM ? "sum" : "record");
    if (t->kind == TYPE_DECL_SUM) {
        for (i = 0; i < t->n_variants; i++) {
            dump_variant(out, arena, indent + 1, t->variants[i]);
        }
    } else {
        for (i = 0; i < t->n_fields; i++) {
            dump_field(out, arena, indent + 1, t->fields[i]);
        }
    }
}

static const char *effect_level_name(EffectLevel level)
{
    switch (level) {
        case EFFECT_PURE:          return "pure";
        case EFFECT_DETERMINISTIC: return "deterministic";
        case EFFECT_MODEL:         return "model";
        case EFFECT_MUTATION:      return "mutation";
    }
    return "?";
}

static void dump_tool_decl(FILE *out, Arena *arena, int indent, const ToolDecl *t)
{
    size_t i;
    dump_indent(out, indent);
    fputs("ToolDecl", out);
    dump_loc(out, t->loc);
    fprintf(out, " %s\n", t->name);
    for (i = 0; i < t->n_params; i++) {
        dump_param(out, arena, indent + 1, t->params[i]);
    }
    dump_indent(out, indent + 1);
    fputs("return-type: ", out);
    dump_type_inline(out, arena, t->return_type);
    fputc('\n', out);
    if (t->effect != NULL) {
        dump_indent(out, indent + 1);
        fputs("effect: ", out);
        fputs(effect_level_name(t->effect->level), out);
        if (t->effect->level == EFFECT_MODEL && t->effect->model_name) {
            fprintf(out, "(\"%s\")", t->effect->model_name);
        }
        switch (t->effect->retry.kind) {
            case RETRY_DEFAULT:
                break;
            case RETRY_FOREVER:
                fputs(" (retry: forever)", out);
                break;
            case RETRY_COUNT:
                fprintf(out, " (retry: %ld)", t->effect->retry.count);
                break;
            case RETRY_BACKOFF:
                fprintf(out,
                        " (retry: backoff(initial: %ld, max: %ld, factor: %ld))",
                        t->effect->retry.backoff_initial,
                        t->effect->retry.backoff_max,
                        t->effect->retry.backoff_factor);
                break;
        }
        fputc('\n', out);
    }
}

static void dump_flow_decl(FILE *out, Arena *arena, int indent, const FlowDecl *f)
{
    size_t i;
    dump_indent(out, indent);
    fputs("FlowDecl", out);
    dump_loc(out, f->loc);
    fprintf(out, " %s%s\n",
            f->name, f->implicit_it ? " (implicit it)" : "");
    for (i = 0; i < f->n_params; i++) {
        dump_param(out, arena, indent + 1, f->params[i]);
    }
    dump_indent(out, indent + 1);
    fputs("return-type: ", out);
    dump_type_inline(out, arena, f->return_type);
    fputc('\n', out);
    for (i = 0; i < f->n_bindings; i++) {
        dump_binding(out, arena, indent + 1, f->bindings[i]);
    }
    dump_indent(out, indent + 1);
    fputs("return:\n", out);
    dump_expr(out, arena, indent + 2, f->return_expr);
}

static void dump_decl(FILE *out, Arena *arena, int indent, const Decl *d)
{
    switch (d->kind) {
    case DECL_TYPE: dump_type_decl(out, arena, indent, d->as.type_decl); break;
    case DECL_TOOL: dump_tool_decl(out, arena, indent, d->as.tool_decl); break;
    case DECL_FLOW: dump_flow_decl(out, arena, indent, d->as.flow_decl); break;
    }
}


/* --------------------------------------------------------------------
 * Public entry point — the only non-static symbol here; every dump_*
 * helper above is file-local and reached only through this function.
 * -------------------------------------------------------------------- */

void ast_dump(FILE *out, Arena *arena, const Program *program)
{
    size_t i;
    if (program == NULL) {
        fputs("<null-program>\n", out);
        return;
    }
    fprintf(out, "Program %s\n",
            program->source_file ? program->source_file : "<unknown>");
    for (i = 0; i < program->n_decls; i++) {
        dump_decl(out, arena, 1, program->decls[i]);
    }
}
