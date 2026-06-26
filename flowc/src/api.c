/* src/api.c
 *
 * Implementation of the public C API declared in api.h.
 *
 * Two POSIX 2008 stdio extensions carry the weight of this file.
 * fmemopen wraps a caller-owned byte buffer in a FILE *, so the
 * lexer (which reads from a FILE * regardless) sees an in-memory
 * source as if it were on disk. open_memstream returns a FILE *
 * whose writes accumulate into a heap-allocated buffer the caller
 * claims after close(), so the emitters (which write to a FILE *
 * regardless) can hand IR or dump output back without touching disk.
 *
 * The internal phases — lex, parse, resolve, check, emit — are
 * unchanged. The only thing this layer adds is what the FILE *s at
 * the boundary are backed by.
 *
 * Failure semantics are documented per-function in api.h. The
 * load-bearing invariants:
 *
 *   - Every compile/dump entry clears the prior output buffer before
 *     it allocates a new one, so the contract "pointer is valid
 *     until the next call" never lies.
 *
 *   - Every failure returns NULL (or the error count for
 *     flowc_check); the diagnostic stream is the only mechanism for
 *     surfacing detail. flowc_error_count is authoritative.
 *
 *   - The context's arena is the lifetime root for AST/resolved/
 *     checked structures across a single compile; the output buffer
 *     is a separate malloc and outlives the arena only by one call.
 *     flowc_context_reset destroys and recreates both arena and
 *     diag stream while preserving the handle.
 */

#include "api.h"

#include "ast.h"
#include "ast_dump.h"
#include "check.h"
#include "ir.h"
#include "lex.h"
#include "parse.h"
#include "parse.tab.h"     /* token kind enums + yylval; internal-only */
#include "resolve.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ====================================================================
 * Context internals
 * ==================================================================== */

struct flowc_context {
    Arena      *arena;
    DiagStream *diag;

    /* One-slot output buffer. Owned by the context; freed and
     * replaced on every compile/dump call, cleared by reset, freed
     * by destroy. NUL-terminated by virtue of open_memstream's
     * fflush-on-close contract. */
    char  *out_buf;
    size_t out_len;
};


flowc_context *flowc_context_create(void)
{
    flowc_context *ctx = calloc(1, sizeof *ctx);
    if (ctx == NULL) {
        FLOWC_ICE("flowc_context_create: out of memory");
    }
    ctx->arena = arena_create(0);
    if (ctx->arena == NULL) {
        free(ctx);
        FLOWC_ICE("flowc_context_create: arena allocation failed");
    }
    ctx->diag    = diag_create();   /* record-only by default */
    ctx->out_buf = NULL;
    ctx->out_len = 0;
    return ctx;
}

void flowc_context_destroy(flowc_context *ctx)
{
    if (ctx == NULL) {
        return;
    }
    free(ctx->out_buf);
    diag_destroy(ctx->diag);
    arena_destroy(ctx->arena);
    free(ctx);
}

void flowc_context_reset(flowc_context *ctx)
{
    if (ctx == NULL) {
        return;
    }
    /* Per api.h, reset invalidates any output pointer the caller
     * previously held: the buffer is freed here, so that pointer now
     * dangles. */
    free(ctx->out_buf);
    ctx->out_buf = NULL;
    ctx->out_len = 0;

    diag_destroy(ctx->diag);
    arena_destroy(ctx->arena);

    ctx->arena = arena_create(0);
    if (ctx->arena == NULL) {
        FLOWC_ICE("flowc_context_reset: arena allocation failed");
    }
    ctx->diag = diag_create();
}


/* ====================================================================
 * Output-buffer plumbing
 * ==================================================================== */

static void ctx_clear_output(flowc_context *ctx)
{
    free(ctx->out_buf);
    ctx->out_buf = NULL;
    ctx->out_len = 0;
}

/* Installs (buf, len) as the new output, freeing any prior buffer
 * first so the one-slot invariant never leaks. Takes ownership of
 * buf. */
static void ctx_take_output(flowc_context *ctx, char *buf, size_t len)
{
    free(ctx->out_buf);
    ctx->out_buf = buf;
    ctx->out_len = len;
}


/* ====================================================================
 * Pipeline driver
 *
 * One internal routine drives the lex/parse/resolve/check/emit
 * pipeline for every entry point. The `stage` argument selects what
 * gets written to `out`:
 *
 *   STAGE_TOKENS   — dump_token_line per token (no parser involved)
 *   STAGE_AST      — ast_dump after a clean parse
 *   STAGE_RESOLVED — resolve_dump after a clean resolve
 *   STAGE_CHECKED  — check_dump after a clean check
 *   STAGE_IR       — ir_emit after a clean check
 *   STAGE_NONE     — no output; used by flowc_check
 *
 * Returns 0 if no error diagnostics were emitted, -1 otherwise.
 * Output is only written when every prior pass produced clean
 * diagnostics; otherwise the buffer ends up empty and the caller
 * sees NULL.
 * ==================================================================== */

typedef enum {
    STAGE_NONE,
    STAGE_TOKENS,
    STAGE_AST,
    STAGE_RESOLVED,
    STAGE_CHECKED,
    STAGE_IR
} pipeline_stage;


static const char *token_kind_name(int kind)
{
    switch (kind) {
    case IDENT:            return "IDENT";
    case INT_LIT:          return "INT_LIT";
    case FLOAT_LIT:        return "FLOAT_LIT";
    case STRING_LIT:       return "STRING_LIT";
    case KW_ALL:           return "KW_ALL";
    case KW_AND:           return "KW_AND";
    case KW_ANY:           return "KW_ANY";
    case KW_ASC:           return "KW_ASC";
    case KW_BACKOFF:       return "KW_BACKOFF";
    case KW_BOOL:          return "KW_BOOL";
    case KW_BY:            return "KW_BY";
    case KW_CONCAT:        return "KW_CONCAT";
    case KW_COUNT:         return "KW_COUNT";
    case KW_DEDUPE:        return "KW_DEDUPE";
    case KW_DESC:          return "KW_DESC";
    case KW_DETERMINISTIC: return "KW_DETERMINISTIC";
    case KW_EFFECT:        return "KW_EFFECT";
    case KW_ELSE:          return "KW_ELSE";
    case KW_FACTOR:        return "KW_FACTOR";
    case KW_FALSE:         return "KW_FALSE";
    case KW_FLOAT:         return "KW_FLOAT";
    case KW_FLOW:          return "KW_FLOW";
    case KW_FOREVER:       return "KW_FOREVER";
    case KW_IF:            return "KW_IF";
    case KW_INITIAL:       return "KW_INITIAL";
    case KW_INT:           return "KW_INT";
    case KW_IT:            return "KW_IT";
    case KW_MATCH:         return "KW_MATCH";
    case KW_MAX:           return "KW_MAX";
    case KW_MIN:           return "KW_MIN";
    case KW_MODEL:         return "KW_MODEL";
    case KW_MUTATION:      return "KW_MUTATION";
    case KW_NOT:           return "KW_NOT";
    case KW_OR:            return "KW_OR";
    case KW_PICK:          return "KW_PICK";
    case KW_PURE:          return "KW_PURE";
    case KW_RANK:          return "KW_RANK";
    case KW_RETRY:         return "KW_RETRY";
    case KW_ROW:           return "KW_ROW";
    case KW_SELECT:        return "KW_SELECT";
    case KW_STRING:        return "KW_STRING";
    case KW_SUM:           return "KW_SUM";
    case KW_TAKE:          return "KW_TAKE";
    case KW_THEN:          return "KW_THEN";
    case KW_TOOL:          return "KW_TOOL";
    case KW_TOP:           return "KW_TOP";
    case KW_TRUE:          return "KW_TRUE";
    case KW_TRY:           return "KW_TRY";
    case KW_TYPE:          return "KW_TYPE";
    case KW_USING:         return "KW_USING";
    case KW_WHERE:         return "KW_WHERE";
    case ARROW:            return "ARROW";
    case LE:               return "LE";
    case GE:               return "GE";
    case EQEQ:             return "EQEQ";
    case NEQ:              return "NEQ";
    default:               return NULL;
    }
}

/* Emit one token in the canonical golden-test format. Four kinds
 * carry a yylval payload from the lexer: IDENT and STRING_LIT set
 * yylval.str, INT_LIT sets yylval.int_val, FLOAT_LIT sets
 * yylval.float_val. This dumper only PRINTS three of them — IDENT,
 * INT_LIT, and FLOAT_LIT — while STRING_LIT falls through to the
 * default case and is emitted without its string value. For the
 * printed kinds the lexer's yylval (still holding the just-returned
 * token) is the source of truth, hence the per-kind switch rather
 * than a uniform format. */
static void dump_token_line(FILE *out, int kind, SrcLoc loc)
{
    const char *file = loc.file ? loc.file : "<unknown>";

    /* Single-char tokens are returned by the lexer as their ASCII
     * code (1..255), so print the literal character; >=256 are the
     * named multi-char kinds handled below. */
    if (kind > 0 && kind < 256) {
        fprintf(out, "%s:%d:%d: '%c'\n",
                file, loc.line, loc.column, (char)kind);
        return;
    }

    const char *name = token_kind_name(kind);
    if (name == NULL) {
        fprintf(out, "%s:%d:%d: <kind=%d>\n",
                file, loc.line, loc.column, kind);
        return;
    }

    switch (kind) {
    case IDENT:
        fprintf(out, "%s:%d:%d: %s '%s'\n",
                file, loc.line, loc.column, name, yylval.str);
        break;
    case INT_LIT:
        fprintf(out, "%s:%d:%d: %s %ld\n",
                file, loc.line, loc.column, name, yylval.int_val);
        break;
    case FLOAT_LIT:
        fprintf(out, "%s:%d:%d: %s %g\n",
                file, loc.line, loc.column, name, yylval.float_val);
        break;
    default:
        fprintf(out, "%s:%d:%d: %s\n",
                file, loc.line, loc.column, name);
        break;
    }
}


static int run_pipeline(flowc_context *ctx,
                        const char *src, size_t len,
                        const char *name,
                        pipeline_stage stage,
                        FILE *out)
{
    /* Copy `name` into the arena: SrcLoc.file must outlive every
     * SrcLoc derived from it (util.h), but the caller's `name` is only
     * borrowed for this call. The fallback is a static literal, so it
     * already outlives any SrcLoc and needs no copy. */
    const char *src_name = name != NULL
        ? arena_strdup(ctx->arena, name)
        : "<source>";

    /* fmemopen takes a non-const pointer for read mode despite never
     * writing; the cast is forced by the POSIX prototype, not by any
     * actual mutation. It also rejects a zero-length buffer (returns
     * NULL/EINVAL on glibc/macOS/BSD), so an empty source is fed as a
     * single space — lexically equivalent to a whitespace-only file
     * (no tokens) — rather than aborting the process with an ICE. */
    const char *in     = src;
    size_t      in_len = len;
    if (in_len == 0) { in = " "; in_len = 1; }
    FILE *fp = fmemopen((void *)in, in_len, "r");
    if (fp == NULL) {
        FLOWC_ICE("fmemopen failed: %s", strerror(errno));
    }

    lex_open(fp, src_name, ctx->arena, ctx->diag);

    if (stage == STAGE_TOKENS) {
        int kind;
        while ((kind = lex_token()) != 0) {
            dump_token_line(out, kind, lex_last_loc());
        }
        lex_close();
        fclose(fp);
        return diag_error_count(ctx->diag) > 0 ? -1 : 0;
    }

    parse_open(ctx->arena, src_name, ctx->diag);
    int prc = parse_run();
    Program *prog = parse_close();
    lex_close();
    fclose(fp);

    if (stage == STAGE_AST) {
        if (prc == 0 && diag_error_count(ctx->diag) == 0 && prog != NULL) {
            ast_dump(out, ctx->arena, prog);
        }
        return diag_error_count(ctx->diag) > 0 ? -1 : 0;
    }

    Resolved *r = NULL;
    Checked  *c = NULL;
    if (prc == 0 && diag_error_count(ctx->diag) == 0 && prog != NULL) {
        r = resolve_run(ctx->arena, ctx->diag, prog);
    }
    if (stage == STAGE_RESOLVED) {
        if (diag_error_count(ctx->diag) == 0 && r != NULL) {
            resolve_dump(out, r);
        }
        return diag_error_count(ctx->diag) > 0 ? -1 : 0;
    }

    if (diag_error_count(ctx->diag) == 0 && r != NULL) {
        c = check_run(ctx->arena, ctx->diag, r);
    }
    if (stage == STAGE_CHECKED) {
        if (diag_error_count(ctx->diag) == 0 && c != NULL) {
            check_dump(out, ctx->arena, c);
        }
        return diag_error_count(ctx->diag) > 0 ? -1 : 0;
    }

    if (stage == STAGE_NONE) {
        return diag_error_count(ctx->diag) > 0 ? -1 : 0;
    }

    /* STAGE_IR */
    if (diag_error_count(ctx->diag) == 0 && c != NULL) {
        ir_emit(out, ctx->arena, prog);
    }
    return diag_error_count(ctx->diag) > 0 ? -1 : 0;
}


/* Run the pipeline, capture output via open_memstream, install in
 * the context's one-slot buffer, return the buffer pointer or NULL.
 * STAGE_NONE callers (flowc_check) take a different path that
 * doesn't allocate a memstream — see flowc_check below. */
static const char *compile_into_buffer(flowc_context *ctx,
                                       const char *src, size_t len,
                                       const char *name,
                                       pipeline_stage stage)
{
    /* Free the previous buffer up front so a caller's prior return
     * pointer is invalidated even if this compile later fails — the
     * "valid only until the next call" contract must hold regardless
     * of outcome. */
    ctx_clear_output(ctx);

    char  *buf     = NULL;
    size_t buf_len = 0;
    FILE  *out     = open_memstream(&buf, &buf_len);
    if (out == NULL) {
        FLOWC_ICE("open_memstream failed: %s", strerror(errno));
    }

    int rc = run_pipeline(ctx, src, len, name, stage, out);

    /* fclose() flushes the stream and finalizes buf/buf_len. After
     * this point, buf is a NUL-terminated malloc'd string. */
    fclose(out);

    if (rc != 0) {
        free(buf);
        return NULL;
    }
    ctx_take_output(ctx, buf, buf_len);
    return ctx->out_buf;
}


/* ====================================================================
 * Public entry points
 * ==================================================================== */

const char *flowc_compile_source(flowc_context *ctx,
                                 const char *src, size_t len,
                                 const char *name)
{
    return compile_into_buffer(ctx, src, len, name, STAGE_IR);
}

const char *flowc_dump_tokens(flowc_context *ctx,
                              const char *src, size_t len,
                              const char *name)
{
    return compile_into_buffer(ctx, src, len, name, STAGE_TOKENS);
}

const char *flowc_dump_ast(flowc_context *ctx,
                           const char *src, size_t len,
                           const char *name)
{
    return compile_into_buffer(ctx, src, len, name, STAGE_AST);
}

const char *flowc_dump_resolved(flowc_context *ctx,
                                const char *src, size_t len,
                                const char *name)
{
    return compile_into_buffer(ctx, src, len, name, STAGE_RESOLVED);
}

const char *flowc_dump_checked(flowc_context *ctx,
                               const char *src, size_t len,
                               const char *name)
{
    return compile_into_buffer(ctx, src, len, name, STAGE_CHECKED);
}

int flowc_check(flowc_context *ctx,
                const char *src, size_t len,
                const char *name)
{
    /* No output expected; clear the slot and skip the memstream. */
    ctx_clear_output(ctx);
    (void)run_pipeline(ctx, src, len, name, STAGE_NONE, NULL);
    return diag_error_count(ctx->diag);
}

const char *flowc_compile_file(flowc_context *ctx, const char *path)
{
    /* Slurp the file into memory and delegate. flowc_read_file opens
     * in binary mode so reading bytes is exact; the lexer is byte-
     * oriented (SrcLoc columns are byte offsets, per the SrcLoc
     * convention in util.h). */
    char  *buf = NULL;
    size_t n   = 0;
    int    err = 0;
    const char *what;
    switch (flowc_read_file(path, &buf, &n, &err)) {
    case FILE_READ_OK:        what = NULL;          break;
    case FILE_READ_ERR_OPEN:  what = "cannot open"; break;
    case FILE_READ_ERR_SEEK:  what = "cannot seek"; break;
    case FILE_READ_ERR_TELL:  what = "cannot tell"; break;
    case FILE_READ_ERR_READ:  what = "cannot read"; break;
    default:                  what = "cannot read"; break;
    }
    if (what != NULL) {
        diag_emit(ctx->diag, SRCLOC_NONE, DIAG_ERROR, "E001",
                  "%s '%s': %s", what, path, strerror(err));
        ctx_clear_output(ctx);
        return NULL;
    }

    const char *result = flowc_compile_source(ctx, buf, n, path);
    free(buf);
    return result;
}


/* ====================================================================
 * Diagnostic queries
 * ==================================================================== */

int flowc_error_count(const flowc_context *ctx)
{
    return diag_error_count(ctx->diag);
}

DiagStream *flowc_diag_stream(flowc_context *ctx)
{
    return ctx->diag;
}
