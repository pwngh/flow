/* src/main.c
 *
 * The `flowc` binary's argument parser, output dispatcher, and exit-
 * code logic. Every compiler pass lives in libflowc — this file
 * exists to translate argv into one of the library's entry points,
 * pipe the result to stdout or `-o <file>`, and finalize the
 * diagnostic stream.
 *
 * Anything that walks the AST, threads the diag stream through
 * passes, or formats a token belongs in api.c, not here. main.c
 * stays close to argv → library-call → stdout.
 *
 * Exit codes:
 *
 *     0  success
 *     1  at least one error diagnostic was issued
 *     2  invalid command-line invocation
 *     3  input file could not be opened or read
 *     4  internal compiler error
 *
 * Codes 2 and 3 are produced by the flowc_*_error helpers in util.c
 * (flowc_invocation_error and flowc_io_error), which exit directly.
 * Code 4 comes from flowc_ice_at via the FLOWC_ICE macro (also in
 * util.c), which is a separate helper, not part of that family.
 * Codes 0 and 1 are produced by this file's return from main().
 */

#include "api.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* The version string format is "flowc (Flow) M.N.P" where M.N.P
 * is a semantic version. */
#define FLOWC_VERSION "0.1.0"


/* ====================================================================
 * Options
 * ==================================================================== */

typedef enum {
    DUMP_NONE,
    DUMP_TOKENS,
    DUMP_AST,
    DUMP_RESOLVED,
    DUMP_CHECKED,
    DUMP_IR
} DumpStage;

typedef struct {
    const char *input_path;
    const char *output_path;
    DumpStage   dump_stage;
    bool        check_only;

    /* Diagnostic configuration captured from argv and applied to
     * the DiagStream in main() after the context is created. */
    DiagFormat  diag_format;
    bool        diag_color_off;
    int         max_errors;
    bool        werror;
} Options;


/* ====================================================================
 * --help and --version
 * ==================================================================== */

static void print_version(void)
{
    printf("flowc (Flow) %s\n", FLOWC_VERSION);
    printf("Reference implementation. See flow(7) for the language reference "
           "and flow-ir(5) for the IR specification.\n");
}

static void print_help(void)
{
    fputs(
"Usage: flowc [option...] file\n"
"\n"
"Compile a Flow (.flow) source file to Flow IR (JSON). Exactly one\n"
"input file is required. With no -o, IR is written to standard output.\n"
"\n"
"Overall options:\n"
"  -o <file>                 Write IR to <file>. '-' means stdout.\n"
"                            No effect with a non-IR --dump stage.\n"
"  -c                        Accepted for compatibility (no-op).\n"
"  -x flow                   Specify input language (only 'flow' accepted).\n"
"  --help                    Display this help and exit.\n"
"  --version                 Display compiler version and exit.\n"
"\n"
"Warning options:\n"
"  -Wall                     Accepted for compatibility; no effect.\n"
"  -Wextra                   Accepted for compatibility; no effect.\n"
"  -Wpedantic                Accepted for compatibility; no extra checks.\n"
"  -Werror                   Treat all warnings as errors.\n"
"  -Werror=<id>              Accepted for compatibility; no effect.\n"
"  -Wno-<id>                 Accepted for compatibility; no effect.\n"
"  -w                        Accepted for compatibility; no effect.\n"
"\n"
"Diagnostic options:\n"
"  -fdiagnostics-format=<f>  Diagnostic format: text (default) or json.\n"
"  -fno-diagnostics-color    Disable color in textual diagnostics.\n"
"  -fmax-errors=<n>          Stop after <n> errors (0 = unlimited; default 1).\n"
"\n"
"Language options:\n"
"  -std=flow-v0              Default language standard.\n"
"  -std=flow-v0-strict       Accepted for compatibility; strict mode is a no-op.\n"
"\n"
"Debug / internals:\n"
"  --dump=<stage>            Dump after <stage>: tokens, ast, resolved,\n"
"                            checked, or ir. Exits after the dump.\n"
"  --check-only              Skip IR emission; exit 0 if no errors.\n"
"\n"
"Environment variables:\n"
"  FLOWC_DIAGNOSTICS_COLOR   Override color: always, never, auto.\n"
"  NO_COLOR                  Disable color (any non-empty value).\n"
"  SOURCE_DATE_EPOCH         Override IR compiled_at timestamp.\n"
"\n"
"See flow(7) and flow-ir(5) for the language and IR reference.\n",
        stdout);
}


/* ====================================================================
 * Small helpers
 * ==================================================================== */

static bool starts_with(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

static bool has_flow_extension(const char *path)
{
    size_t len = strlen(path);
    if (len < 5) return false;
    return strcmp(path + len - 5, ".flow") == 0;
}

static long parse_nonneg_int(const char *option, const char *value)
{
    if (value[0] == '\0') {
        flowc_invocation_error("option '%s' requires a value", option);
    }
    char *end;
    errno = 0;
    long n = strtol(value, &end, 10);
    if (*end != '\0' || errno == ERANGE || n < 0 || n > INT_MAX) {
        flowc_invocation_error("invalid value for '%s': '%s'", option, value);
    }
    return n;
}

static DumpStage parse_dump_stage(const char *value)
{
    if (strcmp(value, "tokens")   == 0) return DUMP_TOKENS;
    if (strcmp(value, "ast")      == 0) return DUMP_AST;
    if (strcmp(value, "resolved") == 0) return DUMP_RESOLVED;
    if (strcmp(value, "checked")  == 0) return DUMP_CHECKED;
    if (strcmp(value, "ir")       == 0) return DUMP_IR;
    flowc_invocation_error(
        "invalid --dump stage: '%s' (expected one of: tokens, ast, "
        "resolved, checked, ir)", value);
    return DUMP_NONE;
}


/* ====================================================================
 * Argument parsing
 * ==================================================================== */

static void parse_args(int argc, char **argv, Options *o)
{
    o->input_path     = NULL;
    o->output_path    = NULL;
    o->dump_stage     = DUMP_NONE;
    o->check_only     = false;
    o->diag_format    = DIAG_FORMAT_TEXT;
    o->diag_color_off = false;
    o->max_errors     = 1;
    o->werror         = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--help")        == 0) { print_help();    exit(0); }
        if (strcmp(a, "--version")     == 0) { print_version(); exit(0); }
        if (strcmp(a, "--check-only")  == 0) { o->check_only  = true; continue; }

        if (starts_with(a, "--dump=")) {
            o->dump_stage = parse_dump_stage(a + 7);
            continue;
        }

        if (strcmp(a, "-o") == 0) {
            if (i + 1 >= argc) flowc_invocation_error("'-o' requires a file argument");
            o->output_path = argv[++i];
            continue;
        }
        if (strcmp(a, "-x") == 0) {
            if (i + 1 >= argc) flowc_invocation_error("'-x' requires a language argument");
            i++;
            if (strcmp(argv[i], "flow") != 0) {
                flowc_invocation_error(
                    "unknown -x language: '%s' (only 'flow' is supported)",
                    argv[i]);
            }
            continue;
        }

        if (strcmp(a, "-c")         == 0) continue;
        if (strcmp(a, "-w")         == 0) continue;
        if (strcmp(a, "-Wall")      == 0) continue;
        if (strcmp(a, "-Wextra")    == 0) continue;
        if (strcmp(a, "-Wpedantic") == 0) continue;
        if (strcmp(a, "-Werror")    == 0) { o->werror         = true; continue; }
        if (strcmp(a, "-fno-diagnostics-color") == 0) {
            o->diag_color_off = true; continue;
        }

        if (starts_with(a, "-Werror=")) continue;
        if (starts_with(a, "-Wno-"))    continue;

        if (starts_with(a, "-fdiagnostics-format=")) {
            const char *v = a + strlen("-fdiagnostics-format=");
            if      (strcmp(v, "text") == 0) o->diag_format = DIAG_FORMAT_TEXT;
            else if (strcmp(v, "json") == 0) o->diag_format = DIAG_FORMAT_JSON;
            else flowc_invocation_error(
                "invalid -fdiagnostics-format: '%s' (expected text or json)", v);
            continue;
        }
        if (starts_with(a, "-fmax-errors=")) {
            o->max_errors = (int)parse_nonneg_int(
                "-fmax-errors", a + strlen("-fmax-errors="));
            continue;
        }
        if (starts_with(a, "-std=")) {
            const char *v = a + strlen("-std=");
            if (strcmp(v, "flow-v0") != 0 && strcmp(v, "flow-v0-strict") != 0) {
                flowc_invocation_error(
                    "unknown -std: '%s' (expected flow-v0 or flow-v0-strict)", v);
            }
            /* Accepted for compatibility; strict mode is unimplemented. */
            continue;
        }

        if (strcmp(a, "-") == 0) {
            flowc_invocation_error(
                "reading source from standard input is not supported");
        }
        if (a[0] != '-') {
            if (o->input_path != NULL) {
                flowc_invocation_error(
                    "multiple input files not supported "
                    "(already have '%s', also given '%s')",
                    o->input_path, a);
            }
            o->input_path = a;
            continue;
        }

        flowc_invocation_error("unrecognized option: '%s' (try --help)", a);
    }

    if (o->input_path == NULL) {
        flowc_invocation_error("no input file (try --help)");
    }
}


/* ====================================================================
 * File slurp
 *
 * Reads the file at `path` into a malloc'd buffer (caller frees).
 * Exits via flowc_io_error on any failure so the CLI's exit-3
 * behavior matches v0: a missing input does not produce a
 * diagnostic-stream error, it ends the process with a one-line
 * message and exit code 3. *Inside* the library, the same I/O
 * failure surfaces as E001 + NULL — different contract, different
 * caller expectation. */
static char *slurp(const char *path, size_t *out_len)
{
    char *buf = NULL;
    switch (flowc_read_file(path, &buf, out_len, NULL)) {
    case FILE_READ_OK:        return buf;
    case FILE_READ_ERR_OPEN:  flowc_io_error(path, "cannot open");
    case FILE_READ_ERR_SEEK:  flowc_io_error(path, "cannot seek");
    case FILE_READ_ERR_TELL:  flowc_io_error(path, "cannot tell");
    case FILE_READ_ERR_READ:  flowc_io_error(path, "cannot read");
    }
    /* flowc_io_error does not return; unreachable. */
    return NULL;
}


/* ====================================================================
 * Dispatch
 *
 * One library call per --dump stage; the returned buffer (NULL on
 * error, NUL-terminated string on success) is written to stdout
 * or, for IR with -o, to the named output file.
 *
 * The exit code is derived from flowc_error_count(ctx) after every
 * dispatch path: zero errors => exit 0, otherwise exit 1.
 * ==================================================================== */

static int dispatch(const Options *o, flowc_context *ctx)
{
    /* W101: emitted before the file is even read, so there is no real
     * source position to point at; anchor it at the synthetic (1,1).
     * W102/W103 share the same synthetic anchor for the same reason. */
    SrcLoc top = { o->input_path, 1, 1 };
    DiagStream *ds = flowc_diag_stream(ctx);

    if (!has_flow_extension(o->input_path)) {
        diag_emit(ds, top, DIAG_WARNING, "W101",
                  "input filename '%s' does not end in '.flow'",
                  o->input_path);
    }

    /* -o redirects only the final IR; for a non-IR --dump stage the
     * output always goes to stdout, so -o has no effect. Warn rather
     * than silently discard the path the user asked to write to. */
    bool ir_stage = (o->dump_stage == DUMP_NONE || o->dump_stage == DUMP_IR);
    if (o->output_path != NULL && !ir_stage) {
        diag_emit(ds, top, DIAG_WARNING, "W102",
                  "-o '%s' has no effect with --dump=%s; dump output "
                  "goes to stdout", o->output_path,
                  o->dump_stage == DUMP_TOKENS   ? "tokens"   :
                  o->dump_stage == DUMP_AST      ? "ast"      :
                  o->dump_stage == DUMP_RESOLVED ? "resolved" : "checked");
    }

    /* --check-only is honored only on the IR path; a non-IR --dump
     * stage runs its dump and ignores --check-only. Warn so the
     * contradictory pair is not silently resolved in favor of --dump. */
    if (o->check_only && !ir_stage) {
        diag_emit(ds, top, DIAG_WARNING, "W103",
                  "--check-only has no effect with a non-IR --dump stage; "
                  "the dump is produced anyway");
    }

    /* Slurp once; every dump-stage delegate takes (src,len,name).
     * Reading here rather than via flowc_compile_file keeps a single
     * site that exits through flowc_io_error on a missing file,
     * preserving the CLI's exit-3 contract. */
    size_t src_len = 0;
    char  *src     = slurp(o->input_path, &src_len);

    const char *out_buf = NULL;
    switch (o->dump_stage) {
    case DUMP_TOKENS:
        out_buf = flowc_dump_tokens(ctx, src, src_len, o->input_path);
        break;
    case DUMP_AST:
        out_buf = flowc_dump_ast(ctx, src, src_len, o->input_path);
        break;
    case DUMP_RESOLVED:
        out_buf = flowc_dump_resolved(ctx, src, src_len, o->input_path);
        break;
    case DUMP_CHECKED:
        out_buf = flowc_dump_checked(ctx, src, src_len, o->input_path);
        break;
    case DUMP_NONE:
    case DUMP_IR:
        if (o->check_only) {
            (void)flowc_check(ctx, src, src_len, o->input_path);
            out_buf = NULL;
        } else {
            out_buf = flowc_compile_source(ctx, src, src_len, o->input_path);
        }
        break;
    }

    free(src);

    /* -o redirects only the final IR (DUMP_NONE/DUMP_IR); --dump stages
     * are debug output and always go to stdout (W102 warned above when
     * -o was supplied for one). '-' forces stdout too. */
    if (out_buf != NULL) {
        bool to_file = ir_stage
                        && o->output_path != NULL
                        && strcmp(o->output_path, "-") != 0;
        if (to_file) {
            FILE *out = fopen(o->output_path, "w");
            if (out == NULL) {
                flowc_io_error(o->output_path, "cannot open for writing");
            }
            fputs(out_buf, out);
            fclose(out);
        } else {
            fputs(out_buf, stdout);
        }
    }

    return flowc_error_count(ctx) > 0 ? 1 : 0;
}


/* ====================================================================
 * main
 * ==================================================================== */

int main(int argc, char **argv)
{
    /* Determinism contract: pin LC_NUMERIC to "C" so "%g" uses '.'
     * regardless of inherited locale. The IR emitter relies on this;
     * see src/ir.c for the full contract. */
    setlocale(LC_NUMERIC, "C");

    Options opts;
    parse_args(argc, argv, &opts);

    flowc_context *ctx  = flowc_context_create();
    DiagStream    *diag = flowc_diag_stream(ctx);

    diag_set_format    (diag, opts.diag_format);
    diag_set_max_errors(diag, opts.max_errors);
    diag_set_werror    (diag, opts.werror);
    diag_attach_stderr (diag,
                        opts.diag_color_off ? DIAG_COLOR_NEVER : DIAG_COLOR_AUTO);

    int rc = dispatch(&opts, ctx);

    diag_finalize(diag);
    flowc_context_destroy(ctx);
    return rc;
}
