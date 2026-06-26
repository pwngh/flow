/*
 * main.c — command-line entry point for the Flow runtime.
 *
 * The standalone flowd binary exposes three subcommands: `run`
 * (execute a flow from a compiled IR against type-directed stub tools,
 * writing a full trace), `replay` (strict-compare two trace dirs and
 * exit non-zero on any divergence), and `diff` (report per-node
 * differences between two traces). Note that `replay` here only
 * compares existing traces; re-executing a recorded run against a new
 * model is done through the C API (flowd_replay), not this binary.
 * Production execution with real tool behavior happens through libflowd,
 * called from a host program or a language binding — the host owns the
 * tool implementations; `run`'s stubs make the flowc -> flowd chain
 * runnable for smoke testing.
 *
 * flowd also exposes two test-only flags that exercise the loader and
 * print an introspection summary of the loaded runtime: `--load-ir
 * <path>` (a human-readable lite dump) and `--canonical-dump <path>`
 * (a byte-deterministic JSON canonical form). The fixture-driven test
 * driver under tests/run.sh uses them; they are not part of the public
 * surface and may change between phases.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flowd.h"
#include "ir_load.h"
#include "value.h"
#include "trace.h"
#include "util.h"
#include "stub_host.h"
#include "cjson/cJSON.h"

#define FLOWD_VERSION "0.1.0-phase1"

static void
print_help(void)
{
    fputs(
        "usage: flowd <subcommand> [options]\n"
        "\n"
        "subcommands:\n"
        "  run           execute a flow from a compiled IR and write a trace\n"
        "  replay        strict-compare two trace dirs (exit 1 on any divergence)\n"
        "  diff          compare two traces and report per-node differences\n"
        "\n"
        "run options:\n"
        "  flowd run <ir.json> [--flow NAME] [--input JSON] [--trace-dir DIR]\n"
        "      Executes a flow against stub tool implementations (a default,\n"
        "      type-correct value per declared tool) and writes a full trace.\n"
        "      --flow      flow to run (default: the first flow in the IR)\n"
        "      --input     flow input as JSON (default: a synthesized default)\n"
        "      --trace-dir trace root (default: ./traces)\n"
        "      The flow's own logic runs for real; only the tools are stubbed.\n"
        "      For real tool behavior, link libflowd and register your own.\n"
        "\n"
        "replay / diff:\n"
        "  flowd replay <trace_a> <trace_b>   strict compare; exit 1 on divergence\n"
        "  flowd diff   <trace_a> <trace_b>   report per-node divergences as JSON\n"
        "      Both compare two existing trace dirs. Re-executing a recorded run\n"
        "      against a new model is done via the C API (flowd_replay), not here.\n"
        "\n"
        "options:\n"
        "  --help                  show this help and exit\n"
        "  --version               show version and exit\n"
        "  --load-ir <path>        (test-only) load an IR file and print a\n"
        "                          human-readable introspection summary\n"
        "  --canonical-dump <path> (test-only) load an IR file and print a\n"
        "                          byte-deterministic JSON canonical form\n"
        "                          of the loaded runtime state\n",
        stdout);
}

/* `run` subcommand — execute a flow end to end against type-directed
 * tool stubs (see the file header). Smoke/dry execution: the tools are
 * stand-ins, but the runtime path it drives (load, bind, eval,
 * gateway/model fallback, trace) is the real one. */

static int
cmd_run(int argc, char **argv)
{
    const char *ir_path   = NULL;
    const char *flow_name = NULL;
    const char *input_arg = NULL;
    const char *trace_dir = "traces";

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--flow") == 0 && i + 1 < argc) {
            flow_name = argv[++i];
        } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input_arg = argv[++i];
        } else if (strcmp(argv[i], "--trace-dir") == 0 && i + 1 < argc) {
            trace_dir = argv[++i];
        } else if (argv[i][0] != '-' && ir_path == NULL) {
            ir_path = argv[i];
        } else {
            fprintf(stderr, "flowd run: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (ir_path == NULL) {
        fputs("flowd: run <ir.json> [--flow NAME] [--input JSON] "
              "[--trace-dir DIR]\n", stderr);
        return 2;
    }

    DiagStream *diag = diag_create();
    diag_set_max_errors(diag, 0);
    diag_attach_stderr(diag);
    flowd_runtime *rt = flowd_load_ir_file(ir_path, diag);
    if (rt == NULL) { diag_destroy(diag); return 1; }

    /* Default to flow index 0, which is declaration order (see the
     * help text): the first flow declared in the IR. */
    const flow_t *flow = flow_name ? flowd_flow_by_name(rt, flow_name)
                                   : (flowd_flow_count(rt) > 0
                                      ? flowd_flow_at(rt, 0) : NULL);
    if (flow == NULL) {
        fprintf(stderr, "flowd run: %s\n",
                flow_name ? "no such flow in IR" : "IR declares no flows");
        flowd_destroy(rt); diag_destroy(diag); return 1;
    }

    /* Stub every tool so the flow runs with no host code (see stub_host). */
    flowd_install_stub_host(rt);

    char *input = input_arg ? strdup(input_arg)
                            : flowd_default_input_json(rt, flow);
    fprintf(stderr, "flowd: running flow '%s'  (input: %s)\n",
            flow->name, input);

    char *susp = NULL;
    char *out  = flowd_run_named(rt, flow->name, input, trace_dir, &susp);

    int rc;
    if (out != NULL) {
        printf("%s\n", out);
        fprintf(stderr, "flowd: trace written under %s/%s/\n",
                trace_dir, flow->name);
        free(out);
        rc = 0;
    } else if (susp != NULL) {
        fprintf(stderr, "flowd: flow suspended (token %s); resume via the "
                        "C API (flowd_resume)\n", susp);
        free(susp);
        rc = 0;
    } else {
        char *e = flowd_last_error_json(rt);
        fprintf(stderr, "flowd: run failed: %s\n", e ? e : "(unknown)");
        free(e);
        rc = 1;
    }

    free(input);
    flowd_destroy(rt);
    diag_destroy(diag);
    return rc;
}

/* Compare two traces and emit JSON divergences to stdout. Returns
 * the number of divergences (0 = identical). */
static int
diff_traces(const char *path_a, const char *path_b, bool verbose)
{
    DiagStream *diag = diag_create();
    diag_attach_stderr(diag);

    trace_reader_t *ra = trace_reader_open(path_a, "flowd-cli", diag);
    trace_reader_t *rb = trace_reader_open(path_b, "flowd-cli", diag);
    if (!ra || !rb) {
        if (ra) trace_reader_close(ra);
        if (rb) trace_reader_close(rb);
        diag_destroy(diag);
        return -1;
    }

    const cJSON *ma = trace_reader_manifest(ra);
    const cJSON *mb = trace_reader_manifest(rb);

    int divergences = 0;
    fputs("{\n  \"divergences\": [\n", stdout);
    bool first = true;

    /* Compare one manifest field by its compact-printed JSON: both
     * sides went through the same canonical writer, so a byte strcmp
     * is a structural compare. A missing field reads as the literal
     * "null" so absent-vs-present still registers as a divergence. */
    #define DIFF_FIELD(key) do {                                       \
        cJSON *va = cJSON_GetObjectItemCaseSensitive(ma, key); \
        cJSON *vb = cJSON_GetObjectItemCaseSensitive(mb, key); \
        char *sa = va ? cJSON_PrintUnformatted(va) : strdup("null");   \
        char *sb = vb ? cJSON_PrintUnformatted(vb) : strdup("null");   \
        if (sa && sb && strcmp(sa, sb) != 0) {                          \
            if (!first) fputs(",\n", stdout);                           \
            first = false;                                              \
            printf("    {\"path\": \"manifest.%s\", \"a\": %s, "       \
                   "\"b\": %s}", key, sa, sb);                          \
            divergences++;                                              \
        }                                                               \
        free(sa); free(sb);                                             \
    } while (0)

    DIFF_FIELD("flow");
    DIFF_FIELD("ir_hash");
    DIFF_FIELD("input_hash");
    DIFF_FIELD("output_hash");
    DIFF_FIELD("status");
    DIFF_FIELD("node_count");
    #undef DIFF_FIELD

    /* Walk nodes pairwise, up to max(na, nb). Both-absent is not a
     * divergence (it just means we've gone past both sides' node
     * counts); one-present-one-absent is a divergence. */
    size_t na = trace_reader_node_count(ra);
    size_t nb = trace_reader_node_count(rb);
    size_t nmax = na > nb ? na : nb;
    for (size_t i = 0; i < nmax; i++) {
        char nid[24]; snprintf(nid, sizeof nid, "n%zu", i);   /* n + up to 20 size_t digits + NUL */
        cJSON *na_j = trace_reader_node(ra, nid);
        cJSON *nb_j = trace_reader_node(rb, nid);
        if (!na_j && !nb_j) continue;
        if (!na_j || !nb_j) {
            if (!first) fputs(",\n", stdout);
            first = false;
            printf("    {\"path\": \"nodes.%s\", "
                   "\"a_present\": %s, \"b_present\": %s}",
                   nid,
                   na_j ? "true" : "false",
                   nb_j ? "true" : "false");
            divergences++;
            cJSON_Delete(na_j); cJSON_Delete(nb_j);
            continue;
        }
        /* Divergence between two runs shows up in the recorded
         * invocations (the inputs/output JSON of each tool call), so
         * compare that array via the same compact-print-then-strcmp
         * canonicalization as the manifest fields. If a node has no
         * invocations array, fall back to comparing the whole node. */
        cJSON *inva = cJSON_GetObjectItemCaseSensitive(na_j, "invocations");
        cJSON *invb = cJSON_GetObjectItemCaseSensitive(nb_j, "invocations");
        char *sa = cJSON_PrintUnformatted(inva ? inva : na_j);
        char *sb = cJSON_PrintUnformatted(invb ? invb : nb_j);
        if (sa && sb && strcmp(sa, sb) != 0) {
            if (!first) fputs(",\n", stdout);
            first = false;
            printf("    {\"path\": \"nodes.%s.invocations\", "
                   "\"a\": %s, \"b\": %s}", nid, sa, sb);
            divergences++;
        }
        free(sa); free(sb);
        cJSON_Delete(na_j); cJSON_Delete(nb_j);
    }
    if (!first) fputc('\n', stdout);
    fputs("  ],\n", stdout);
    printf("  \"divergence_count\": %d\n", divergences);
    fputs("}\n", stdout);

    trace_reader_close(ra);
    trace_reader_close(rb);

    if (verbose && divergences == 0) {
        fprintf(stderr, "flowd: no divergences "
                        "(manifest fields and recorded invocations match)\n");
    }
    diag_destroy(diag);
    return divergences;
}

/* replay <trace_a> <trace_b>: strict equality. Exit 0 only if no
 * divergences. The "re-execute against live IR" framing from the
 * brief is implemented via the C API tests; the CLI compares the
 * resulting traces. */
static int
cmd_replay(int argc, char **argv)
{
    if (argc < 2) {
        fputs("flowd: replay <original_trace_dir> <new_trace_dir>\n",
              stderr);
        return 2;
    }
    int d = diff_traces(argv[0], argv[1], true);
    if (d < 0) return 2;
    return d == 0 ? 0 : 1;
}

static int
cmd_diff(int argc, char **argv)
{
    if (argc < 2) {
        fputs("flowd: diff <trace_dir_a> <trace_dir_b>\n", stderr);
        return 2;
    }
    int d = diff_traces(argv[0], argv[1], false);
    if (d < 0) return 2;
    return 0;     /* diff always exits 0 unless I/O failed */
}

/* Shared implementation for --load-ir and --canonical-dump (the
 * test-driver hooks for the fixture suite); the only difference is
 * which dump function runs on success. Diagnostics route to stderr via
 * an attached DiagStream so the fixture driver can diff the
 * human-readable error text; dump output goes to stdout. Returns 1 on
 * load failure (R150, missing fields, malformed JSON). A genuinely
 * fatal condition (file I/O, OOM) exits via the util fatal-exit
 * helpers with their documented codes. */
typedef void (*dump_fn)(const flowd_runtime *, FILE *);

static int
cmd_load_and_dump(const char *path, dump_fn dump)
{
    DiagStream *diag = diag_create();
    diag_set_max_errors(diag, 0);     /* unlimited */
    diag_attach_stderr(diag);

    flowd_runtime *rt = flowd_load_ir_file(path, diag);
    int rc = 0;
    if (!rt) {
        rc = 1;
    } else {
        dump(rt, stdout);
        flowd_destroy(rt);
    }
    diag_destroy(diag);
    return rc;
}

static int
cmd_load_ir(const char *path)
{
    return cmd_load_and_dump(path, flowd_canonical_dump_lite);
}

static int
cmd_canonical_dump(const char *path)
{
    return cmd_load_and_dump(path, flowd_canonical_dump_json);
}

int
main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "--help") == 0
                 || strcmp(argv[1], "-h")     == 0) {
        print_help();
        return argc < 2 ? 1 : 0;
    }
    if (strcmp(argv[1], "--version") == 0) {
        printf("flowd %s\n", FLOWD_VERSION);
        return 0;
    }
    if (strcmp(argv[1], "--load-ir") == 0) {
        if (argc < 3) {
            fputs("flowd: --load-ir requires a path argument\n", stderr);
            return 2;
        }
        return cmd_load_ir(argv[2]);
    }
    if (strcmp(argv[1], "--canonical-dump") == 0) {
        if (argc < 3) {
            fputs("flowd: --canonical-dump requires a path argument\n",
                  stderr);
            return 2;
        }
        return cmd_canonical_dump(argv[2]);
    }
    if (strcmp(argv[1], "run") == 0) {
        return cmd_run(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "replay") == 0) {
        return cmd_replay(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "diff") == 0) {
        return cmd_diff(argc - 2, argv + 2);
    }
    fprintf(stderr, "flowd: unknown subcommand: %s\n", argv[1]);
    print_help();
    return 1;
}
