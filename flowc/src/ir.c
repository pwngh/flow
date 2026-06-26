/* src/ir.c
 *
 * JSON IR emitter.  Hand-rolled, no external library.
 *
 *
 * Format
 * ------
 *
 * Pretty-printed UTF-8 JSON, two-space indent, one member per line
 * inside objects, one element per line inside non-empty arrays.
 * Empty objects render as `{}`; empty arrays as `[]`.  No trailing
 * comma anywhere.  A single trailing newline after the closing `}`.
 *
 * Field order is fixed by this file -- see the section headings
 * below.  A reader can therefore byte-diff two outputs from the
 * same source and any difference is a real difference, not a
 * dictionary-iteration artifact.
 *
 *
 * Determinism
 * -----------
 *
 *   - LC_NUMERIC is assumed to be "C" (src/main.c's main() calls
 *     setlocale(LC_NUMERIC, "C"); no other category is pinned).
 *     Without it, the printf used by flowc_format_double picks up
 *     the user's LC_NUMERIC and emits ',' as the decimal separator in
 *     locales like de_DE — at which point the IR is no longer valid
 *     JSON.
 *
 *   - `compiled_at` reads SOURCE_DATE_EPOCH when set (treated as a
 *     decimal Unix timestamp), otherwise uses the current wall
 *     time.  The output is formatted as RFC 3339 UTC.  This matches
 *     the reproducible-builds convention used by GCC, Debian, etc.
 *
 *   - Floats (both IR literals and predicate strings) render through
 *     flowc_format_double, the shortest decimal that strtod reads back
 *     exactly — so a literal keeps the precision it was written with
 *     and a simple value stays short. Deterministic by construction.
 */

#include "ir.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ast.h"
#include "util.h"


/* --------------------------------------------------------------------
 * Decl lookup
 *
 * The IR's argument records require the parameter name in `field`
 * for shorthand (positional) arguments.  The parser records
 * field=NULL for shorthand; the type checker derives the mapping
 * by position.  The IR emitter mirrors that here: shorthand args
 * resolve to the k-th formal of the referenced tool / type.
 * -------------------------------------------------------------------- */

static const ToolDecl *find_tool_in_program(const Program *p, const char *name)
{
    size_t i;
    for (i = 0; i < p->n_decls; i++) {
        const Decl *d = p->decls[i];
        if (d->kind == DECL_TOOL && strcmp(d->as.tool_decl->name, name) == 0) {
            return d->as.tool_decl;
        }
    }
    return NULL;
}

static const TypeDecl *find_type_in_program(const Program *p, const char *name)
{
    size_t i;
    for (i = 0; i < p->n_decls; i++) {
        const Decl *d = p->decls[i];
        if (d->kind == DECL_TYPE && strcmp(d->as.type_decl->name, name) == 0) {
            return d->as.type_decl;
        }
    }
    return NULL;
}


/* --------------------------------------------------------------------
 * String helpers
 * -------------------------------------------------------------------- */

/* Write `s` as a JSON string literal, escaping " \ and control
 * characters per RFC 8259.  Flow identifiers never contain
 * characters that need escaping; the routine is defensive against
 * arbitrary source-file paths and against future feature
 * additions. */
static void emit_str(FILE *out, const char *s)
{
    fputc('"', out);
    flowc_json_escape(out, s);
    fputc('"', out);
}

static void emit_indent(FILE *out, int n)
{
    int i;
    for (i = 0; i < n; i++) fputs("  ", out);
}


/* --------------------------------------------------------------------
 * Timestamp
 * -------------------------------------------------------------------- */

/* Format `compiled_at` into `buf`.  If SOURCE_DATE_EPOCH is set and
 * parses as a non-negative decimal integer, use it; otherwise use
 * the current wall time.  Output format is RFC 3339 UTC. */
static void format_compiled_at(char *buf, size_t buflen)
{
    const char *sde = getenv("SOURCE_DATE_EPOCH");
    time_t t;

    if (sde != NULL && *sde != '\0') {
        char *end;
        errno = 0;
        long v = strtol(sde, &end, 10);
        if (*end == '\0' && errno != ERANGE && v >= 0) {
            t = (time_t)v;
        } else {
            t = time(NULL);  /* malformed or out of range; ignore */
        }
    } else {
        t = time(NULL);
    }

    struct tm utc;
    /* gmtime_r is POSIX but not C99; gmtime is C99 and returns a
     * pointer to a static buffer.  Single-threaded compiler, so
     * gmtime is fine here. */
    struct tm *r = gmtime(&t);
    if (r == NULL) {
        /* Defensive: gmtime can fail for out-of-range time_t.
         * Fall back to the epoch. */
        /* Only the fields the fixed format reads are set; %Y/%m/%d/%H/%M/%S
         * never touch tm_wday/tm_yday/tm_isdst.  Adding a weekday/day-of-year
         * specifier to the strftime format below would read those zeroed
         * fields and lie -- set them here too if you do. */
        memset(&utc, 0, sizeof utc);
        utc.tm_year = 70;
        utc.tm_mday = 1;
    } else {
        utc = *r;
    }
    strftime(buf, buflen, "%Y-%m-%dT%H:%M:%SZ", &utc);
}


/* --------------------------------------------------------------------
 * Predicate-to-string rendering
 *
 * The IR's `where` stage record stores its predicate as a single
 * canonical string.  We render the predicate AST into a fixed-size
 * PRED_BUF buffer with no dynamic growth.  The buffer is large
 * enough for ordinary predicates, but it does NOT provably hold
 * every v0-legal predicate: a single identifier may be up to
 * FLOWC_MAX_IDENT (1024) bytes (see src/lex.l), so a comparison of
 * two long multi-segment paths, or a deep and/or chain, can exceed
 * PRED_BUF.  The StrBuf helpers detect that overflow (the
 * `truncated` flag) and the caller in emit_stage turns it into a
 * hard FLOWC_ICE rather than silently emitting a mangled predicate.
 * -------------------------------------------------------------------- */

#define PRED_BUF 4096

typedef struct {
    char  *buf;
    size_t cap;
    size_t pos;
    int    truncated;  /* set once any output had to be dropped */
} StrBuf;

static void sb_putc(StrBuf *b, char c)
{
    if (b->pos + 1 < b->cap) {
        b->buf[b->pos++] = c;
        b->buf[b->pos]   = '\0';
    } else {
        b->truncated = 1;
    }
}

static void sb_puts(StrBuf *b, const char *s)
{
    for (; *s; s++) sb_putc(b, *s);
}

static void sb_printf(StrBuf *b, const char *fmt, ...)
{
    /* Guard first: once the buffer is full, `cap - pos` would
     * underflow (both size_t), so bail before computing `remaining`. */
    if (b->pos >= b->cap) { b->truncated = 1; return; }
    size_t remaining = b->cap - b->pos;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->pos, remaining, fmt, ap);
    va_end(ap);
    if (n < 0) { b->truncated = 1; return; }
    if ((size_t)n >= remaining) {
        b->pos = b->cap - 1;  /* truncated */
        b->truncated = 1;
    } else {
        b->pos += (size_t)n;
    }
}

static const char *cmp_op_token(CmpOp op)
{
    switch (op) {
    case CMP_LE:  return "<=";
    case CMP_GE:  return ">=";
    case CMP_LT:  return "<";
    case CMP_GT:  return ">";
    case CMP_EQ:  return "==";
    case CMP_NEQ: return "!=";
    }
    return "?";
}

static void render_expr_inline(StrBuf *b, const Expr *e)
{
    if (e == NULL) {
        sb_puts(b, "<null>");
        return;
    }
    switch (e->kind) {
    case EXPR_LITERAL:
        switch (e->as.literal.kind) {
        case LIT_INT:   sb_printf(b, "%ld", e->as.literal.int_val); break;
        case LIT_FLOAT: {
            char fb[32];
            flowc_format_double(e->as.literal.float_val, fb, sizeof fb);
            sb_puts(b, fb);
            break;
        }
        case LIT_BOOL:
            sb_puts(b, e->as.literal.bool_val ? "true" : "false");
            break;
        }
        break;
    case EXPR_PATH: {
        size_t i;
        for (i = 0; i < e->as.path.n; i++) {
            if (i > 0) sb_putc(b, '.');
            sb_puts(b, e->as.path.segments[i]);
        }
        break;
    }
    default:
        /* Calls / constructs / pipelines do not appear in v0
         * predicates; the type checker will already have flagged
         * those.  We render a placeholder for resilience. */
        sb_puts(b, "<expr>");
        break;
    }
}

static void render_cmp(StrBuf *b, const CmpExpr *c)
{
    render_expr_inline(b, c->lhs);
    sb_putc(b, ' ');
    sb_puts(b, cmp_op_token(c->op));
    sb_putc(b, ' ');
    render_expr_inline(b, c->rhs);
}

static void render_predicate(StrBuf *b, const Predicate *p)
{
    if (p == NULL) {
        sb_puts(b, "<null>");
        return;
    }
    switch (p->kind) {
    case PRED_CMP:
        render_cmp(b, p->cmp);
        break;
    case PRED_AND:
        render_predicate(b, p->left);
        sb_puts(b, " and ");
        render_predicate(b, p->right);
        break;
    case PRED_OR:
        render_predicate(b, p->left);
        sb_puts(b, " or ");
        render_predicate(b, p->right);
        break;
    }
}


/* --------------------------------------------------------------------
 * Expression emission (recursive)
 * -------------------------------------------------------------------- */

static void emit_expr(FILE *out, Arena *a, const Program *p, int indent, const Expr *e);

static const char *literal_type_str(LiteralKind k)
{
    switch (k) {
    case LIT_INT:   return "int";
    case LIT_FLOAT: return "float";
    case LIT_BOOL:  return "bool";
    }
    return "?";
}

static void emit_args_from_formals(FILE *out, Arena *a, const Program *p,
                                   int indent,
                                   Param *const *formals, size_t n_formals,
                                   Arg **args, size_t n, const char *key)
{
    emit_indent(out, indent);
    fprintf(out, "\"%s\": [", key);
    if (n == 0) {
        fputs("]", out);
        return;
    }
    fputc('\n', out);
    size_t i, shorthand_i = 0;
    for (i = 0; i < n; i++) {
        const char *field_name = args[i]->field;
        if (field_name == NULL) {
            if (formals != NULL && shorthand_i < n_formals) {
                field_name = formals[shorthand_i]->name;
            } else {
                field_name = "";  /* checker should have flagged */
            }
            shorthand_i++;
        }

        emit_indent(out, indent + 1);
        fputs("{\n", out);
        emit_indent(out, indent + 2);
        fputs("\"field\": ", out);
        emit_str(out, field_name);
        fputs(",\n", out);
        emit_indent(out, indent + 2);
        fputs("\"value\": ", out);
        emit_expr(out, a, p, indent + 2, args[i]->value);
        fputc('\n', out);
        emit_indent(out, indent + 1);
        fputc('}', out);
        if (i + 1 < n) fputc(',', out);
        fputc('\n', out);
    }
    emit_indent(out, indent);
    fputc(']', out);
}

static void emit_args(FILE *out, Arena *a, const Program *p, int indent,
                      const char *tool_name,
                      Arg **args, size_t n, const char *key)
{
    const ToolDecl *td = find_tool_in_program(p, tool_name);
    Param *const *formals = (td != NULL) ? td->params : NULL;
    size_t n_formals     = (td != NULL) ? td->n_params : 0;
    emit_args_from_formals(out, a, p, indent, formals, n_formals, args, n, key);
}

/* Emit the "fields": [...] array for a construct expression. The
 * `formals` array supplies the field names used to expand shorthand
 * positional args (`User { "alice", "a@b.c" }` → fields[0]="id",
 * fields[1]="email"). For record constructs the formals come from
 * the type decl; for variant constructs they come from the matched
 * variant. NULL formals (no decl found) falls back to empty names
 * for shorthand args, which downstream consumers can detect. */
static void emit_construct_fields_from_formals(
    FILE *out, Arena *a, const Program *p, int indent,
    Field *const *formals, size_t n_formals,
    Arg **args, size_t n)
{
    emit_indent(out, indent);
    fputs("\"fields\": [", out);
    if (n == 0) {
        fputs("]", out);
        return;
    }
    fputc('\n', out);
    size_t i, shorthand_i = 0;
    for (i = 0; i < n; i++) {
        const char *field_name = args[i]->field;
        if (field_name == NULL) {
            if (formals != NULL && shorthand_i < n_formals) {
                field_name = formals[shorthand_i]->name;
            } else {
                field_name = "";
            }
            shorthand_i++;
        }

        emit_indent(out, indent + 1);
        fputs("{\n", out);
        emit_indent(out, indent + 2);
        fputs("\"name\": ", out);
        emit_str(out, field_name);
        fputs(",\n", out);
        emit_indent(out, indent + 2);
        fputs("\"value\": ", out);
        emit_expr(out, a, p, indent + 2, args[i]->value);
        fputc('\n', out);
        emit_indent(out, indent + 1);
        fputc('}', out);
        if (i + 1 < n) fputc(',', out);
        fputc('\n', out);
    }
    emit_indent(out, indent);
    fputc(']', out);
}

static void emit_construct_fields(FILE *out, Arena *a, const Program *p, int indent,
                                  const char *type_name,
                                  Arg **args, size_t n)
{
    const TypeDecl *td = find_type_in_program(p, type_name);
    Field *const *formals = (td != NULL && td->kind == TYPE_DECL_RECORD)
        ? td->fields : NULL;
    size_t n_formals = (td != NULL && td->kind == TYPE_DECL_RECORD)
        ? td->n_fields : 0;
    emit_construct_fields_from_formals(out, a, p, indent,
                                       formals, n_formals, args, n);
}

static void emit_stage(FILE *out, Arena *a, const Program *p, int indent, const Stage *s)
{
    emit_indent(out, indent);
    fputs("{\n", out);
    /* kind + op pair first, then operator-specific members */
    switch (s->kind) {
    case STAGE_WHERE: {
        StrBuf b;
        char  buf[PRED_BUF];
        b.buf = buf; b.cap = sizeof buf; b.pos = 0; b.truncated = 0;
        buf[0] = '\0';
        render_predicate(&b, s->predicate);
        if (b.truncated) {
            /* A legal predicate overflowed PRED_BUF. Emitting the
             * truncated string would write a predicate that parses as
             * a different (or invalid) predicate downstream while the
             * compiler still exited success. Fail loudly instead. */
            FLOWC_ICE("where-predicate exceeds %d-byte render buffer",
                      PRED_BUF);
        }

        emit_indent(out, indent + 1);
        fputs("\"kind\": \"filter\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"op\": \"where\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"predicate\": ", out);
        emit_str(out, buf);
        fputc('\n', out);
        break;
    }
    case STAGE_RANK:
        emit_indent(out, indent + 1);
        fputs("\"kind\": \"sort\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"op\": \"rank\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"by\": ", out);
        emit_str(out, s->sort_field ? s->sort_field : "");
        fputs(",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"direction\": ", out);
        emit_str(out, ast_sort_dir_str(s->sort_dir));
        fputc('\n', out);
        break;
    case STAGE_SELECT:
        emit_indent(out, indent + 1);
        fputs("\"kind\": \"select\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"op\": \"select\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"body\": ", out);
        emit_expr(out, a, p, indent + 1, s->body);
        fputc('\n', out);
        break;
    case STAGE_DEDUPE:
        emit_indent(out, indent + 1);
        fputs("\"kind\": \"dedupe\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"op\": \"dedupe_by\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"key\": ", out);
        emit_expr(out, a, p, indent + 1, s->body);
        fputc('\n', out);
        break;
    case STAGE_CONCAT:
        emit_indent(out, indent + 1);
        fputs("\"kind\": \"concat\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"op\": \"concat\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"other\": ", out);
        emit_expr(out, a, p, indent + 1, s->body);
        fputc('\n', out);
        break;
    case STAGE_TAKE:
        emit_indent(out, indent + 1);
        fputs("\"kind\": \"take\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"op\": \"take\",\n", out);
        emit_indent(out, indent + 1);
        fprintf(out, "\"count\": %ld\n", s->take_count);
        break;
    case STAGE_TERMINAL:
        emit_indent(out, indent + 1);
        fputs("\"kind\": \"terminal\",\n", out);
        switch (s->terminal_kind) {
        case TERMINAL_TOP:
            emit_indent(out, indent + 1);
            fputs("\"op\": \"top\"\n", out);
            break;
        case TERMINAL_PICK:
            emit_indent(out, indent + 1);
            fputs("\"op\": \"pick\",\n", out);
            emit_indent(out, indent + 1);
            fputs("\"model\": ", out);
            emit_str(out, s->model ? s->model : "");
            fputc('\n', out);
            break;
        case TERMINAL_COUNT:
            emit_indent(out, indent + 1);
            fputs("\"op\": \"count\"\n", out);
            break;
        case TERMINAL_SUM:
        case TERMINAL_MAX:
        case TERMINAL_MIN: {
            const char *op =
                (s->terminal_kind == TERMINAL_SUM) ? "sum" :
                (s->terminal_kind == TERMINAL_MAX) ? "max" : "min";
            emit_indent(out, indent + 1);
            fprintf(out, "\"op\": \"%s\",\n", op);
            emit_indent(out, indent + 1);
            fputs("\"field\": ", out);
            emit_str(out, s->agg_field ? s->agg_field : "");
            fputc('\n', out);
            break;
        }
        case TERMINAL_ANY:
        case TERMINAL_ALL: {
            const char *op =
                (s->terminal_kind == TERMINAL_ANY) ? "any" : "all";
            emit_indent(out, indent + 1);
            fprintf(out, "\"op\": \"%s\",\n", op);
            emit_indent(out, indent + 1);
            fputs("\"predicate\": ", out);
            emit_expr(out, a, p, indent + 1, s->agg_pred);
            fputc('\n', out);
            break;
        }
        }
        break;
    }
    emit_indent(out, indent);
    fputc('}', out);
}

static void emit_expr(FILE *out, Arena *a, const Program *p, int indent, const Expr *e)
{
    if (e == NULL) {
        fputs("null", out);
        return;
    }
    fputs("{\n", out);
    switch (e->kind) {

    case EXPR_LITERAL:
        emit_indent(out, indent + 1);
        fputs("\"kind\": \"literal\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"value\": ", out);
        switch (e->as.literal.kind) {
        case LIT_INT:   fprintf(out, "%ld", e->as.literal.int_val);  break;
        case LIT_FLOAT: {
            char fb[32];
            flowc_format_double(e->as.literal.float_val, fb, sizeof fb);
            fputs(fb, out);
            break;
        }
        case LIT_BOOL:
            fputs(e->as.literal.bool_val ? "true" : "false", out);
            break;
        }
        fputs(",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"type\": ", out);
        emit_str(out, literal_type_str(e->as.literal.kind));
        fputc('\n', out);
        break;

    case EXPR_PATH: {
        emit_indent(out, indent + 1);
        fputs("\"kind\": \"path\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"segments\": [", out);
        size_t i;
        for (i = 0; i < e->as.path.n; i++) {
            if (i > 0) fputs(", ", out);
            emit_str(out, e->as.path.segments[i]);
        }
        fputs("]\n", out);
        break;
    }

    case EXPR_CALL:
        if (e->as.apply.resolved_flow != NULL) {
            const FlowDecl *flow = e->as.apply.resolved_flow;
            emit_indent(out, indent + 1);
            fputs("\"kind\": \"subflow_call\",\n", out);
            emit_indent(out, indent + 1);
            fputs("\"flow\": ", out);
            emit_str(out, e->as.apply.name);
            fputs(",\n", out);
            emit_args_from_formals(out, a, p, indent + 1,
                                   flow->params, flow->n_params,
                                   e->as.apply.args, e->as.apply.n_args,
                                   "args");
            fputc('\n', out);
        } else {
            emit_indent(out, indent + 1);
            fputs("\"kind\": \"call\",\n", out);
            emit_indent(out, indent + 1);
            fputs("\"tool\": ", out);
            emit_str(out, e->as.apply.name);
            fputs(",\n", out);
            emit_args(out, a, p, indent + 1, e->as.apply.name,
                      e->as.apply.args, e->as.apply.n_args, "args");
            fputc('\n', out);
        }
        break;

    case EXPR_CONSTRUCT:
        if (e->as.apply.resolved_variant != NULL) {
            const TypeDecl *sum = e->as.apply.resolved_sum;
            const Variant  *var = e->as.apply.resolved_variant;
            emit_indent(out, indent + 1);
            fputs("\"kind\": \"construct_variant\",\n", out);
            emit_indent(out, indent + 1);
            fputs("\"type\": ", out);
            emit_str(out, sum->name);
            fputs(",\n", out);
            emit_indent(out, indent + 1);
            fputs("\"variant\": ", out);
            emit_str(out, var->name);
            fputs(",\n", out);
            emit_construct_fields_from_formals(
                out, a, p, indent + 1,
                var->fields, var->n_fields,
                e->as.apply.args, e->as.apply.n_args);
            fputc('\n', out);
        } else {
            emit_indent(out, indent + 1);
            fputs("\"kind\": \"construct\",\n", out);
            emit_indent(out, indent + 1);
            fputs("\"type\": ", out);
            emit_str(out, e->as.apply.name);
            fputs(",\n", out);
            emit_construct_fields(out, a, p, indent + 1, e->as.apply.name,
                                  e->as.apply.args, e->as.apply.n_args);
            fputc('\n', out);
        }
        break;

    case EXPR_PIPELINE: {
        emit_indent(out, indent + 1);
        fputs("\"kind\": \"pipeline\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"source\": ", out);
        emit_expr(out, a, p, indent + 1, e->as.pipeline.source);
        fputs(",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"stages\": [", out);
        if (e->as.pipeline.n_stages == 0) {
            fputs("]\n", out);
        } else {
            fputc('\n', out);
            size_t i;
            for (i = 0; i < e->as.pipeline.n_stages; i++) {
                emit_stage(out, a, p, indent + 2, e->as.pipeline.stages[i]);
                if (i + 1 < e->as.pipeline.n_stages) fputc(',', out);
                fputc('\n', out);
            }
            emit_indent(out, indent + 1);
            fputs("]\n", out);
        }
        break;
    }

    case EXPR_BINOP:
        emit_indent(out, indent + 1);
        fputs("\"kind\": \"binop\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"op\": ", out);
        emit_str(out, ast_binop_str(e->as.binop.op));
        fputs(",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"left\": ", out);
        emit_expr(out, a, p, indent + 1, e->as.binop.lhs);
        fputs(",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"right\": ", out);
        emit_expr(out, a, p, indent + 1, e->as.binop.rhs);
        fputc('\n', out);
        break;

    case EXPR_UNOP:
        emit_indent(out, indent + 1);
        fputs("\"kind\": \"unop\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"op\": ", out);
        emit_str(out, ast_unop_str(e->as.unop.op));
        fputs(",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"operand\": ", out);
        emit_expr(out, a, p, indent + 1, e->as.unop.operand);
        fputc('\n', out);
        break;

    case EXPR_LIST_LITERAL: {
        emit_indent(out, indent + 1);
        fputs("\"kind\": \"list_literal\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"element_type\": ", out);
        if (e->as.list_literal.element_type != NULL) {
            emit_str(out, ast_type_to_string(a, e->as.list_literal.element_type));
        } else {
            emit_str(out, "");
        }
        fputs(",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"elements\": [", out);
        if (e->as.list_literal.n_elements == 0) {
            fputs("]\n", out);
        } else {
            fputc('\n', out);
            size_t i;
            for (i = 0; i < e->as.list_literal.n_elements; i++) {
                emit_indent(out, indent + 2);
                emit_expr(out, a, p, indent + 2, e->as.list_literal.elements[i]);
                if (i + 1 < e->as.list_literal.n_elements) fputc(',', out);
                fputc('\n', out);
            }
            emit_indent(out, indent + 1);
            fputs("]\n", out);
        }
        break;
    }

    case EXPR_CONDITIONAL: {
        emit_indent(out, indent + 1);
        fputs("\"kind\": \"conditional\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"branches\": [", out);
        if (e->as.conditional.n_branches == 0) {
            fputs("],\n", out);
        } else {
            fputc('\n', out);
            size_t i;
            for (i = 0; i < e->as.conditional.n_branches; i++) {
                emit_indent(out, indent + 2);
                fputs("{\n", out);
                emit_indent(out, indent + 3);
                fputs("\"cond\": ", out);
                emit_expr(out, a, p, indent + 3,
                          e->as.conditional.branches[i]->cond);
                fputs(",\n", out);
                emit_indent(out, indent + 3);
                fputs("\"consequent\": ", out);
                emit_expr(out, a, p, indent + 3,
                          e->as.conditional.branches[i]->consequent);
                fputc('\n', out);
                emit_indent(out, indent + 2);
                fputc('}', out);
                if (i + 1 < e->as.conditional.n_branches) fputc(',', out);
                fputc('\n', out);
            }
            emit_indent(out, indent + 1);
            fputs("],\n", out);
        }
        emit_indent(out, indent + 1);
        fputs("\"else\": ", out);
        emit_expr(out, a, p, indent + 1, e->as.conditional.else_expr);
        fputc('\n', out);
        break;
    }

    case EXPR_TRY_ELSE:
        emit_indent(out, indent + 1);
        fputs("\"kind\": \"try_else\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"try\": ", out);
        emit_expr(out, a, p, indent + 1, e->as.try_else.try_expr);
        fputs(",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"else\": ", out);
        emit_expr(out, a, p, indent + 1, e->as.try_else.else_expr);
        fputc('\n', out);
        break;

    case EXPR_MATCH: {
        emit_indent(out, indent + 1);
        fputs("\"kind\": \"match\",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"scrutinee\": ", out);
        emit_expr(out, a, p, indent + 1, e->as.match.scrutinee);
        fputs(",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"scrutinee_type\": ", out);
        if (e->as.match.resolved_sum != NULL) {
            emit_str(out, e->as.match.resolved_sum->name);
        } else {
            emit_str(out, "");
        }
        fputs(",\n", out);
        emit_indent(out, indent + 1);
        fputs("\"arms\": [", out);
        if (e->as.match.n_arms == 0) {
            fputs("]\n", out);
        } else {
            fputc('\n', out);
            size_t i;
            for (i = 0; i < e->as.match.n_arms; i++) {
                const MatchArm *arm = e->as.match.arms[i];
                emit_indent(out, indent + 2);
                fputs("{\n", out);
                emit_indent(out, indent + 3);
                fputs("\"pattern\": ", out);
                switch (arm->pattern.kind) {
                    case PAT_WILDCARD:
                        fputs("{\"kind\": \"wildcard\"}", out);
                        break;
                    case PAT_BIND:
                        /* Catch-all binder. Encoded as a
                         * wildcard kind with an attached binder, so
                         * runtime match dispatch handles both paths
                         * uniformly. */
                        fputs("{\"kind\": \"wildcard\", \"binder\": ", out);
                        emit_str(out, arm->pattern.binder_name);
                        fputc('}', out);
                        break;
                    case PAT_VARIANT:
                        fputs("{\"kind\": \"variant\", \"variant\": ", out);
                        emit_str(out, arm->pattern.variant_name);
                        fputc('}', out);
                        break;
                    case PAT_VARIANT_BIND:
                        fputs("{\"kind\": \"variant\", \"variant\": ", out);
                        emit_str(out, arm->pattern.variant_name);
                        fputs(", \"binder\": ", out);
                        emit_str(out, arm->pattern.binder_name);
                        fputc('}', out);
                        break;
                }
                fputs(",\n", out);
                emit_indent(out, indent + 3);
                fputs("\"body\": ", out);
                emit_expr(out, a, p, indent + 3, arm->body);
                fputc('\n', out);
                emit_indent(out, indent + 2);
                fputc('}', out);
                if (i + 1 < e->as.match.n_arms) fputc(',', out);
                fputc('\n', out);
            }
            emit_indent(out, indent + 1);
            fputs("]\n", out);
        }
        break;
    }

    }
    emit_indent(out, indent);
    fputc('}', out);
}


/* --------------------------------------------------------------------
 * Type / field / param emission
 * -------------------------------------------------------------------- */

/* Emit a `fields` array body (the inside of the brackets). Shared
 * between record-type decls and sum-variant decls — both carry a
 * Field[]. Caller is responsible for the array brackets and indent
 * of the surrounding container. */
static void emit_fields_array(FILE *out, Arena *a, int indent,
                              Field *const *fields, size_t n)
{
    if (n == 0) {
        fputs("[]", out);
        return;
    }
    fputs("[\n", out);
    size_t i;
    for (i = 0; i < n; i++) {
        const Field *f = fields[i];
        emit_indent(out, indent + 1);
        fputs("{\"name\": ", out);
        emit_str(out, f->name);
        fputs(", \"type\": ", out);
        emit_str(out, ast_type_to_string(a, f->type));
        fputc('}', out);
        if (i + 1 < n) fputc(',', out);
        fputc('\n', out);
    }
    emit_indent(out, indent);
    fputc(']', out);
}

static void emit_record_type_decl(FILE *out, Arena *a, int indent, const TypeDecl *t)
{
    emit_indent(out, indent);
    fputs("{\n", out);
    emit_indent(out, indent + 1);
    fputs("\"name\": ", out);
    emit_str(out, t->name);
    fputs(",\n", out);
    emit_indent(out, indent + 1);
    fputs("\"kind\": \"record\",\n", out);
    emit_indent(out, indent + 1);
    fputs("\"fields\": ", out);
    emit_fields_array(out, a, indent + 1, t->fields, t->n_fields);
    fputc('\n', out);
    emit_indent(out, indent);
    fputc('}', out);
}

static void emit_sum_type_decl(FILE *out, Arena *a, int indent, const TypeDecl *t)
{
    emit_indent(out, indent);
    fputs("{\n", out);
    emit_indent(out, indent + 1);
    fputs("\"name\": ", out);
    emit_str(out, t->name);
    fputs(",\n", out);
    emit_indent(out, indent + 1);
    fputs("\"kind\": \"sum\",\n", out);
    emit_indent(out, indent + 1);
    fputs("\"variants\": [", out);
    if (t->n_variants == 0) {
        fputs("]\n", out);
    } else {
        fputc('\n', out);
        size_t i;
        for (i = 0; i < t->n_variants; i++) {
            const Variant *v = t->variants[i];
            emit_indent(out, indent + 2);
            fputs("{\n", out);
            emit_indent(out, indent + 3);
            fputs("\"name\": ", out);
            emit_str(out, v->name);
            fputs(",\n", out);
            emit_indent(out, indent + 3);
            fputs("\"fields\": ", out);
            emit_fields_array(out, a, indent + 3, v->fields, v->n_fields);
            fputc('\n', out);
            emit_indent(out, indent + 2);
            fputc('}', out);
            if (i + 1 < t->n_variants) fputc(',', out);
            fputc('\n', out);
        }
        emit_indent(out, indent + 1);
        fputs("]\n", out);
    }
    emit_indent(out, indent);
    fputc('}', out);
}

static void emit_type_decl(FILE *out, Arena *a, int indent, const TypeDecl *t)
{
    if (t->kind == TYPE_DECL_SUM) {
        emit_sum_type_decl(out, a, indent, t);
    } else {
        emit_record_type_decl(out, a, indent, t);
    }
}

static void emit_tool_decl(FILE *out, Arena *a, int indent, const ToolDecl *t)
{
    emit_indent(out, indent);
    fputs("{\n", out);
    emit_indent(out, indent + 1);
    fputs("\"name\": ", out);
    emit_str(out, t->name);
    fputs(",\n", out);
    emit_indent(out, indent + 1);
    fputs("\"input\": [", out);
    if (t->n_params == 0) {
        fputs("],\n", out);
    } else {
        fputc('\n', out);
        size_t i;
        for (i = 0; i < t->n_params; i++) {
            const Param *p = t->params[i];
            emit_indent(out, indent + 2);
            fputs("{\"name\": ", out);
            emit_str(out, p->name);
            fputs(", \"type\": ", out);
            emit_str(out, ast_type_to_string(a, p->type));
            fputc('}', out);
            if (i + 1 < t->n_params) fputc(',', out);
            fputc('\n', out);
        }
        emit_indent(out, indent + 1);
        fputs("],\n", out);
    }
    emit_indent(out, indent + 1);
    fputs("\"output\": ", out);
    emit_str(out, ast_type_to_string(a, t->return_type));
    fputs(",\n", out);

    /* effect — mandatory in v1. Resolver E198 guards the
     * NULL case so we never get here without a clause; if a stray
     * malformed AST reaches us with no effect, fall back to emitting
     * an empty "{}" placeholder. */
    emit_indent(out, indent + 1);
    fputs("\"effect\": {", out);
    if (t->effect == NULL) {
        fputs("}\n", out);
    } else {
        fputc('\n', out);
        const char *level_str = "pure";
        switch (t->effect->level) {
            case EFFECT_PURE:          level_str = "pure";          break;
            case EFFECT_DETERMINISTIC: level_str = "deterministic"; break;
            case EFFECT_MODEL:         level_str = "model";         break;
            case EFFECT_MUTATION:      level_str = "mutation";      break;
        }
        emit_indent(out, indent + 2);
        fputs("\"level\": ", out);
        emit_str(out, level_str);
        if (t->effect->level == EFFECT_MODEL && t->effect->model_name) {
            fputs(",\n", out);
            emit_indent(out, indent + 2);
            fputs("\"model\": ", out);
            emit_str(out, t->effect->model_name);
        }
        if (t->effect->retry.kind != RETRY_DEFAULT) {
            fputs(",\n", out);
            emit_indent(out, indent + 2);
            fputs("\"retry\": ", out);
            switch (t->effect->retry.kind) {
                case RETRY_FOREVER:
                    fputs("{\"kind\": \"forever\"}", out);
                    break;
                case RETRY_COUNT:
                    fprintf(out, "{\"kind\": \"count\", \"value\": %ld}",
                            t->effect->retry.count);
                    break;
                case RETRY_BACKOFF:
                    fprintf(out,
                            "{\"kind\": \"backoff\", \"initial\": %ld, "
                            "\"max\": %ld, \"factor\": %ld}",
                            t->effect->retry.backoff_initial,
                            t->effect->retry.backoff_max,
                            t->effect->retry.backoff_factor);
                    break;
                case RETRY_DEFAULT:
                    break;  /* unreachable; guarded by outer if */
            }
        }
        fputc('\n', out);
        emit_indent(out, indent + 1);
        fputc('}', out);
        fputc('\n', out);
    }
    emit_indent(out, indent);
    fputc('}', out);
}

static void emit_flow_decl(FILE *out, Arena *a, const Program *p, int indent, const FlowDecl *f)
{
    emit_indent(out, indent);
    fputs("{\n", out);

    emit_indent(out, indent + 1);
    fputs("\"name\": ", out);
    emit_str(out, f->name);
    fputs(",\n", out);

    emit_indent(out, indent + 1);
    fputs("\"params\": [", out);
    if (f->n_params == 0) {
        fputs("],\n", out);
    } else {
        fputc('\n', out);
        size_t i;
        for (i = 0; i < f->n_params; i++) {
            const Param *param = f->params[i];
            int implicit = (f->implicit_it && i == 0);
            emit_indent(out, indent + 2);
            fputs("{\"name\": ", out);
            emit_str(out, param->name);
            fputs(", \"type\": ", out);
            emit_str(out, ast_type_to_string(a, param->type));
            fputs(", \"implicit\": ", out);
            fputs(implicit ? "true" : "false", out);
            fputc('}', out);
            if (i + 1 < f->n_params) fputc(',', out);
            fputc('\n', out);
        }
        emit_indent(out, indent + 1);
        fputs("],\n", out);
    }

    emit_indent(out, indent + 1);
    fputs("\"output\": ", out);
    emit_str(out, ast_type_to_string(a, f->return_type));
    fputs(",\n", out);

    emit_indent(out, indent + 1);
    fputs("\"bindings\": [", out);
    if (f->n_bindings == 0) {
        fputs("],\n", out);
    } else {
        fputc('\n', out);
        size_t i;
        for (i = 0; i < f->n_bindings; i++) {
            const Binding *b = f->bindings[i];
            emit_indent(out, indent + 2);
            fputs("{\n", out);
            emit_indent(out, indent + 3);
            fputs("\"name\": ", out);
            emit_str(out, b->name);
            fputs(",\n", out);
            emit_indent(out, indent + 3);
            fputs("\"expr\": ", out);
            emit_expr(out, a, p, indent + 3, b->value);
            fputc('\n', out);
            emit_indent(out, indent + 2);
            fputc('}', out);
            if (i + 1 < f->n_bindings) fputc(',', out);
            fputc('\n', out);
        }
        emit_indent(out, indent + 1);
        fputs("],\n", out);
    }

    emit_indent(out, indent + 1);
    fputs("\"return\": ", out);
    emit_expr(out, a, p, indent + 1, f->return_expr);
    fputc('\n', out);

    emit_indent(out, indent);
    fputc('}', out);
}


/* --------------------------------------------------------------------
 * Top-level emission
 * -------------------------------------------------------------------- */

void ir_emit(FILE *out, Arena *a, const Program *p)
{
    if (p == NULL) {
        fputs("null\n", out);
        return;
    }

    char ts[32];
    format_compiled_at(ts, sizeof ts);

    fputs("{\n", out);

    fputs("  \"ir_version\": \"1.0\",\n", out);

    fputs("  \"compiled_at\": ", out);
    emit_str(out, ts);
    fputs(",\n", out);

    fputs("  \"source_file\": ", out);
    emit_str(out, p->source_file ? p->source_file : "");
    fputs(",\n", out);

    fputs("  \"types\": [", out);
    {
        size_t i, first = 1;
        for (i = 0; i < p->n_decls; i++) {
            const Decl *d = p->decls[i];
            if (d->kind != DECL_TYPE) continue;
            if (first) { fputc('\n', out); first = 0; }
            emit_type_decl(out, a, 2, d->as.type_decl);
            /* Types interleave with tools/flows in p->decls, so the
             * next array element is not simply decls[i+1]; scan ahead
             * for the next DECL_TYPE to know whether a separating comma
             * is needed (the format forbids a trailing comma). */
            size_t j;
            int last = 1;
            for (j = i + 1; j < p->n_decls; j++) {
                if (p->decls[j]->kind == DECL_TYPE) { last = 0; break; }
            }
            if (!last) fputc(',', out);
            fputc('\n', out);
        }
        if (!first) fputs("  ", out);
        fputs("],\n", out);
    }

    fputs("  \"tools\": [", out);
    {
        size_t i, first = 1;
        for (i = 0; i < p->n_decls; i++) {
            const Decl *d = p->decls[i];
            if (d->kind != DECL_TOOL) continue;
            if (first) { fputc('\n', out); first = 0; }
            emit_tool_decl(out, a, 2, d->as.tool_decl);
            size_t j;
            int last = 1;
            for (j = i + 1; j < p->n_decls; j++) {
                if (p->decls[j]->kind == DECL_TOOL) { last = 0; break; }
            }
            if (!last) fputc(',', out);
            fputc('\n', out);
        }
        if (!first) fputs("  ", out);
        fputs("],\n", out);
    }

    fputs("  \"flows\": [", out);
    {
        size_t i, first = 1;
        for (i = 0; i < p->n_decls; i++) {
            const Decl *d = p->decls[i];
            if (d->kind != DECL_FLOW) continue;
            if (first) { fputc('\n', out); first = 0; }
            emit_flow_decl(out, a, p, 2, d->as.flow_decl);
            size_t j;
            int last = 1;
            for (j = i + 1; j < p->n_decls; j++) {
                if (p->decls[j]->kind == DECL_FLOW) { last = 0; break; }
            }
            if (!last) fputc(',', out);
            fputc('\n', out);
        }
        if (!first) fputs("  ", out);
        fputs("]\n", out);
    }

    fputs("}\n", out);
}
