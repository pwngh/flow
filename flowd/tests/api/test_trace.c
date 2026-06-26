/* tests/api/test_trace.c
 *
 * Trace writer + reader + replay/diff — end-to-end.
 *
 * Verifies the full chain:
 *   1. flowd_run with trace_dir creates a complete trace.
 *   2. Two runs of the same flow against the same inputs produce
 *      byte-identical trace directories (with SOURCE_DATE_EPOCH=0
 *      and FLOWD_EXECUTION_ID_SUFFIX pinned).
 *   3. trace_reader_open succeeds on a sealed trace, returns the
 *      expected manifest fields, and appends to audit.log.
 *   4. flowd_run_named picks the right flow from a multi-flow IR.
 *   5. trace_audit_append writes a JSON-Lines record.
 *
 * Each case assertion prints `ok` or `not ok` and the program exits
 * non-zero if any assertion fails.
 */

#include "flowd.h"
#include "trace.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cjson/cJSON.h"

static int g_pass = 0;
static int g_fail = 0;

static void ok(const char *n)   { printf("  ok %s\n", n);     g_pass++; }
static void fail(const char *n, const char *d) {
    printf("not ok %s\n    %s\n", n, d ? d : "(no detail)");  g_fail++;
}

static char *
tool_double(const char *args, char **err, void *ctx)
{
    (void)err; (void)ctx;
    const char *p = strstr(args, "\"x\":");
    long n = strtol(p + 4, NULL, 10);
    char *out = malloc(32);
    snprintf(out, 32, "%ld", n * 2);
    return out;
}

/* Recursively remove a directory. */
static void
rm_rf(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char child[1024];
        snprintf(child, sizeof child, "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode))
            rm_rf(child);
        else
            unlink(child);
    }
    closedir(d);
    rmdir(path);
}

/* Read whole file into malloc'd buffer. */
static char *
slurp(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    char *buf = malloc((size_t)sz + 1u);
    if (!buf) { fclose(fp); return NULL; }
    fread(buf, 1, (size_t)sz, fp);
    buf[sz] = '\0';
    fclose(fp);
    return buf;
}

/* Byte-compare two files. */
static bool
files_equal(const char *a, const char *b)
{
    char *ba = slurp(a);
    char *bb = slurp(b);
    bool eq = (ba && bb) && strcmp(ba, bb) == 0;
    free(ba); free(bb);
    return eq;
}


/* ---- Case 1: basic trace materializes ---- */

static const char *IR =
    "{\"ir_version\":\"1.0\",\"types\":[],"
    " \"tools\":[{\"name\":\"doubler\","
    "  \"input\":[{\"name\":\"x\",\"type\":\"int\"}],"
    "  \"output\":\"int\",\"effect\":{\"level\":\"pure\"}}],"
    " \"flows\":[{\"name\":\"twice\","
    "  \"params\":[{\"name\":\"it\",\"type\":\"int\",\"implicit\":true}],"
    "  \"output\":\"int\",\"bindings\":[],"
    "  \"return\":{\"kind\":\"call\",\"tool\":\"doubler\","
    "   \"args\":[{\"field\":\"x\",\"value\":"
    "    {\"kind\":\"path\",\"segments\":[\"it\"]}}]}}]}";

static void
test_trace_materializes(void)
{
    rm_rf("/tmp/flowd-test-traceA");
    setenv("FLOWD_EXECUTION_ID_SUFFIX", "abcdef", 1);
    setenv("SOURCE_DATE_EPOCH", "0", 1);

    flowd_runtime *rt = flowd_load_ir(IR);
    if (!rt) { fail("trace/load", flowd_last_error_json(NULL)); return; }
    flowd_register_tool(rt, "doubler", FLOWD_EFFECT_PURE,
                        "(int) -> int", tool_double, "v1", NULL);
    char *susp = NULL;
    char *out = flowd_run(rt, "21", "/tmp/flowd-test-traceA", &susp);
    if (!out) { fail("trace/run", flowd_last_error_json(NULL));
                flowd_destroy(rt); return; }
    free(out);
    flowd_destroy(rt);

    /* Verify the directory shape. */
    struct stat st;
    bool m_ok = stat("/tmp/flowd-test-traceA/twice/exec_1970_01_01_abcdef/"
                     "manifest.json", &st) == 0;
    bool n0   = stat("/tmp/flowd-test-traceA/twice/exec_1970_01_01_abcdef/"
                     "nodes/n0.json", &st) == 0;
    bool n1   = stat("/tmp/flowd-test-traceA/twice/exec_1970_01_01_abcdef/"
                     "nodes/n1.json", &st) == 0;
    bool n2   = stat("/tmp/flowd-test-traceA/twice/exec_1970_01_01_abcdef/"
                     "nodes/n2.json", &st) == 0;
    if (m_ok && n0 && n1 && n2) ok("trace/materializes");
    else fail("trace/materializes",
              "missing one of manifest.json, nodes/n0..n2.json");
}


/* ---- Case 2: byte-determinism across two runs ---- */

static void
test_byte_determinism(void)
{
    rm_rf("/tmp/flowd-test-traceB");
    rm_rf("/tmp/flowd-test-traceC");
    setenv("FLOWD_EXECUTION_ID_SUFFIX", "abcdef", 1);
    setenv("SOURCE_DATE_EPOCH", "0", 1);

    for (int i = 0; i < 2; i++) {
        flowd_runtime *rt = flowd_load_ir(IR);
        flowd_register_tool(rt, "doubler", FLOWD_EFFECT_PURE,
                            "(int) -> int", tool_double, "v1", NULL);
        char *susp = NULL;
        char *out = flowd_run(rt, "21",
            i == 0 ? "/tmp/flowd-test-traceB" : "/tmp/flowd-test-traceC",
            &susp);
        free(out);
        flowd_destroy(rt);
    }

    const char *b = "/tmp/flowd-test-traceB/twice/exec_1970_01_01_abcdef";
    const char *c = "/tmp/flowd-test-traceC/twice/exec_1970_01_01_abcdef";

    char p1[256], p2[256];
    bool all_eq = true;
    const char *files[] = {
        "manifest.json", "nodes/n0.json", "nodes/n1.json", "nodes/n2.json"
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        snprintf(p1, sizeof p1, "%s/%s", b, files[i]);
        snprintf(p2, sizeof p2, "%s/%s", c, files[i]);
        if (!files_equal(p1, p2)) {
            all_eq = false;
            char detail[256];
            snprintf(detail, sizeof detail, "%s differs", files[i]);
            fail("byte-determinism/files-match", detail);
            break;
        }
    }
    if (all_eq) ok("byte-determinism/files-match");
}


/* ---- Case 3: trace_reader + audit log ---- */

static void
test_reader_and_audit(void)
{
    /* Reuse traceA from case 1. */
    const char *dir = "/tmp/flowd-test-traceA/twice/exec_1970_01_01_abcdef";

    DiagStream *diag = diag_create();
    diag_record_only(diag);
    trace_reader_t *r = trace_reader_open(dir, "test-suite", diag);
    if (!r) { fail("reader/open", "trace_reader_open returned NULL");
              diag_destroy(diag); return; }
    ok("reader/open");

    const cJSON *m = trace_reader_manifest(r);
    cJSON *flow = cJSON_GetObjectItemCaseSensitive((cJSON *)m, "flow");
    if (cJSON_IsString(flow) &&
        strcmp(cJSON_GetStringValue((cJSON *)flow), "twice") == 0) {
        ok("reader/manifest-flow-name");
    } else {
        fail("reader/manifest-flow-name",
             "manifest.flow != \"twice\"");
    }

    size_t nc = trace_reader_node_count(r);
    if (nc == 3) ok("reader/node-count-3");
    else {
        char buf[64]; snprintf(buf, sizeof buf, "node_count=%zu", nc);
        fail("reader/node-count-3", buf);
    }

    cJSON *n1 = trace_reader_node(r, "n1");
    if (n1) {
        cJSON *tool = cJSON_GetObjectItemCaseSensitive(n1, "tool");
        if (cJSON_IsString(tool) &&
            strcmp(cJSON_GetStringValue(tool), "doubler") == 0) {
            ok("reader/node-tool-name");
        } else {
            fail("reader/node-tool-name", "n1.tool != \"doubler\"");
        }
        cJSON_Delete(n1);
    } else {
        fail("reader/node-fetch", "n1 not loadable");
    }
    trace_reader_close(r);
    diag_destroy(diag);

    /* Audit log: opening the reader should have appended a
     * `trace_read` entry. */
    char alog[256];
    snprintf(alog, sizeof alog, "%s/audit.log", dir);
    char *contents = slurp(alog);
    if (contents && strstr(contents, "\"event\":\"trace_read\"") &&
                    strstr(contents, "\"caller\":\"test-suite\"")) {
        ok("audit/trace-read-recorded");
    } else {
        fail("audit/trace-read-recorded",
             contents ? contents : "audit.log not found");
    }
    free(contents);
}


/* ---- Case 4: flowd_run_named ---- */

static void
test_run_named(void)
{
    /* Two flows: pick the second by name. */
    const char *ir =
        "{\"ir_version\":\"1.0\",\"types\":[],\"tools\":[],"
        " \"flows\":["
        "  {\"name\":\"a\","
        "   \"params\":[{\"name\":\"it\",\"type\":\"int\",\"implicit\":true}],"
        "   \"output\":\"int\",\"bindings\":[],"
        "   \"return\":{\"kind\":\"literal\",\"type\":\"int\",\"value\":111}},"
        "  {\"name\":\"b\","
        "   \"params\":[{\"name\":\"it\",\"type\":\"int\",\"implicit\":true}],"
        "   \"output\":\"int\",\"bindings\":[],"
        "   \"return\":{\"kind\":\"literal\",\"type\":\"int\",\"value\":222}}]}";

    flowd_runtime *rt = flowd_load_ir(ir);
    char *susp = NULL;
    char *out = flowd_run_named(rt, "b", "0", NULL, &susp);
    if (out && strcmp(out, "222") == 0) ok("run_named/picks-b");
    else fail("run_named/picks-b", out ? out : "NULL");
    free(out);

    out = flowd_run_named(rt, "nonexistent", "0", NULL, &susp);
    if (out == NULL) {
        char *err = flowd_last_error_json(NULL);
        if (err && strstr(err, "not found")) ok("run_named/unknown-flow-R155");
        else fail("run_named/unknown-flow-R155",
                  err ? err : "(no error)");
        free(err);
    } else {
        fail("run_named/unknown-flow-R155",
             "expected NULL for unknown flow");
        free(out);
    }
    flowd_destroy(rt);
}


/* ---- Case 5: end-to-end same-output replay ---- */

static void
test_replay_equality(void)
{
    rm_rf("/tmp/flowd-test-replayA");
    rm_rf("/tmp/flowd-test-replayB");
    setenv("FLOWD_EXECUTION_ID_SUFFIX", "abcdef", 1);
    setenv("SOURCE_DATE_EPOCH", "0", 1);

    /* Two independent runs against the same flow + input. */
    for (int i = 0; i < 2; i++) {
        flowd_runtime *rt = flowd_load_ir(IR);
        flowd_register_tool(rt, "doubler", FLOWD_EFFECT_PURE,
                            "(int) -> int", tool_double, "v1", NULL);
        char *susp = NULL;
        char *out = flowd_run(rt, "21",
            i == 0 ? "/tmp/flowd-test-replayA"
                   : "/tmp/flowd-test-replayB",
            &susp);
        free(out);
        flowd_destroy(rt);
    }

    const char *dirA = "/tmp/flowd-test-replayA/twice/exec_1970_01_01_abcdef";
    const char *dirB = "/tmp/flowd-test-replayB/twice/exec_1970_01_01_abcdef";

    DiagStream *diag = diag_create();
    diag_record_only(diag);
    trace_reader_t *ra = trace_reader_open(dirA, "replay-test", diag);
    trace_reader_t *rb = trace_reader_open(dirB, "replay-test", diag);
    if (!ra || !rb) {
        fail("replay/open-both", "one or both readers failed");
        if (ra) trace_reader_close(ra);
        if (rb) trace_reader_close(rb);
        diag_destroy(diag);
        return;
    }
    cJSON *oa = cJSON_GetObjectItemCaseSensitive(
        (cJSON *)trace_reader_manifest(ra), "output_hash");
    cJSON *ob = cJSON_GetObjectItemCaseSensitive(
        (cJSON *)trace_reader_manifest(rb), "output_hash");
    if (cJSON_IsString(oa) && cJSON_IsString(ob) &&
        strcmp(cJSON_GetStringValue(oa),
               cJSON_GetStringValue(ob)) == 0) {
        ok("replay/output-hashes-match");
    } else {
        fail("replay/output-hashes-match",
             "output_hash differs across runs");
    }
    trace_reader_close(ra);
    trace_reader_close(rb);
    diag_destroy(diag);
}


/* ---- Case 6: failed run produces "failed" manifest ---- */

static void
test_failed_run_status(void)
{
    rm_rf("/tmp/flowd-test-failed");
    setenv("FLOWD_EXECUTION_ID_SUFFIX", "abcdef", 1);
    setenv("SOURCE_DATE_EPOCH", "0", 1);

    /* IR references a tool that isn't registered → R152 fail. */
    flowd_runtime *rt = flowd_load_ir(IR);
    /* No register_tool call. */
    char *susp = NULL;
    char *out = flowd_run(rt, "21", "/tmp/flowd-test-failed", &susp);
    if (out != NULL) { fail("failed/run-should-fail", out); free(out); }
    else ok("failed/run-fails");

    DiagStream *diag = diag_create();
    diag_record_only(diag);
    trace_reader_t *r = trace_reader_open(
        "/tmp/flowd-test-failed/twice/exec_1970_01_01_abcdef",
        "test", diag);
    if (!r) {
        fail("failed/manifest-readable",
             "no manifest after failed run");
    } else {
        cJSON *st = cJSON_GetObjectItemCaseSensitive(
            (cJSON *)trace_reader_manifest(r), "status");
        if (cJSON_IsString(st) &&
            strcmp(cJSON_GetStringValue(st), "failed") == 0) {
            ok("failed/manifest-status-failed");
        } else {
            fail("failed/manifest-status-failed",
                 st ? cJSON_GetStringValue(st) : "(absent)");
        }
        trace_reader_close(r);
    }
    diag_destroy(diag);
    flowd_destroy(rt);
}


/* ---- Case 7: value_read audit event ---- */

static void
test_value_read_audit(void)
{
    /* Open a trace and request a value by hash via the new
     * trace_reader_value API. Even with a missing hash, the audit
     * log should grow a `value_read` entry. */
    DiagStream *diag = diag_create();
    diag_record_only(diag);
    const char *dir = "/tmp/flowd-test-traceA/twice/exec_1970_01_01_abcdef";
    trace_reader_t *r = trace_reader_open(dir, "vr-test", diag);
    if (!r) { fail("value_read/open", "reader open failed");
              diag_destroy(diag); return; }

    /* Fetch a hash that definitely isn't in the store (value was
     * inlined; nothing should be under values/). The fetch returns
     * NULL but the audit entry fires. */
    cJSON *v = trace_reader_value(r,
        "sha256:0000000000000000000000000000000000000000"
        "000000000000000000000000");
    if (v) { fail("value_read/missing-hash", "should have returned NULL");
             cJSON_Delete(v); }

    trace_reader_close(r);
    diag_destroy(diag);

    char alog[256];
    snprintf(alog, sizeof alog, "%s/audit.log", dir);
    char *contents = slurp(alog);
    if (contents && strstr(contents, "\"event\":\"value_read\"") &&
                    strstr(contents, "\"caller\":\"vr-test\"")) {
        ok("value_read/audit-entry-written");
    } else {
        fail("value_read/audit-entry-written",
             contents ? contents : "audit.log absent");
    }
    free(contents);
}


/* ---- Case 8: trace-dir create failure → R301 ---- */

static void
test_trace_dir_create_fails_R301(void)
{
    /* Plant a regular FILE where a trace-root parent directory would
     * need to be, then ask the writer to nest a trace UNDER it. The
     * writer derives trace_dir = "<root>/<slug>/<execution_id>" and
     * calls ensure_dir on it; mkdir of the path component that is the
     * regular file fails with ENOTDIR, so ensure_dir returns non-zero
     * and trace_writer_open emits R301 before returning NULL. */
    const char *blocker = "/tmp/flowd-test-r301-blocker";
    unlink(blocker);
    FILE *fp = fopen(blocker, "wb");
    if (!fp) { fail("r301/setup", "could not create blocker file"); return; }
    fputs("not a directory\n", fp);
    fclose(fp);

    /* trace_root sits one level under the regular file, so creating
     * any directory beneath it is impossible. */
    char root[256];
    snprintf(root, sizeof root, "%s/nested", blocker);

    DiagStream *diag = diag_create();
    diag_record_only(diag);

    trace_writer_t *w = trace_writer_open(root, "twice", IR, 0, diag, NULL);
    if (w != NULL) {
        fail("r301/open-should-fail",
             "trace_writer_open succeeded under a regular file");
        trace_writer_close(w);
        diag_destroy(diag);
        unlink(blocker);
        return;
    }

    bool saw_r301 = false;
    for (size_t i = 0; i < diag_count(diag); i++) {
        const Diagnostic *d = diag_at(diag, i);
        if (d && d->id && strcmp(d->id, "R301") == 0) { saw_r301 = true; break; }
    }
    if (saw_r301) {
        ok("r301/trace-dir-create-failed");
    } else {
        const char *detail = diag_count(diag)
            ? diag_at(diag, 0)->id : "(no diagnostics recorded)";
        fail("r301/trace-dir-create-failed", detail);
    }

    diag_destroy(diag);
    unlink(blocker);
}


int
main(void)
{
    test_trace_materializes();
    test_byte_determinism();
    test_reader_and_audit();
    test_run_named();
    test_replay_equality();
    test_failed_run_status();
    test_value_read_audit();
    test_trace_dir_create_fails_R301();
    printf("\nPASS %d  FAIL %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
