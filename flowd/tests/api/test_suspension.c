/* tests/api/test_suspension.c
 *
 * suspend + resume.
 *
 * Verifies:
 *   1. A flow that calls await_human_approval suspends; flowd_run
 *      returns NULL and *suspension_token points at the on-disk
 *      suspension file.
 *   2. The suspension's manifest is sealed with status: "suspended".
 *   3. The suspensions/<token>.json file contains the data needed
 *      to resume: flow name, input, suspended node id, args.
 *   4. flowd_resume with the token and a decision_json injects the
 *      decision and the flow completes.
 *   5. The resume trace has the resumed node tagged with
 *      replay_of.mode == "re_invoked".
 *   6. The resume trace's synthesized output node (live post-suspension
 *      execution) is tagged replay_of.mode == "re_invoked", not
 *      "restored_from_trace".
 */

#include "flowd.h"
#include "trace.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "cjson/cJSON.h"

/* These tests join short, controlled fixture paths with readdir() names. gcc
 * can't bound a dirent name, so it flags the snprintf path-builds below with
 * -Wformat-truncation though the buffers are amply sized for any real path.
 * Silence that one false alarm (gcc only; clang has no such warning). */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

static int g_pass = 0;
static int g_fail = 0;
static void ok(const char *n) { printf("  ok %s\n", n); g_pass++; }
static void fail(const char *n, const char *d) {
    printf("not ok %s\n    %s\n", n, d ? d : "(no detail)"); g_fail++;
}
static void assert_eq(const char *n, const char *got, const char *want) {
    if (got && strcmp(got, want) == 0) ok(n);
    else {
        char buf[1024];
        snprintf(buf, sizeof buf, "want=%s\n    got =%s",
                 want, got ? got : "NULL");
        fail(n, buf);
    }
}

static void
rm_rf(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char c[1024]; snprintf(c, sizeof c, "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(c, &st) == 0 && S_ISDIR(st.st_mode)) rm_rf(c);
        else unlink(c);
    }
    closedir(d); rmdir(path);
}

/* IR: a flow that asks for human approval and returns the result.
 *
 *   type Approval = | Approved {approver: string}
 *                   | Rejected {approver: string, reason: string}
 *
 *   tool await_human_approval(prompt: string)
 *     effect mutation -> Approval
 *
 *   flow gate(prompt: string) -> Approval =
 *     await_human_approval(prompt: prompt)
 *
 * The runtime recognizes the tool name and suspends; no host
 * registration of an impl is needed (or expected). The decision
 * comes back through flowd_resume.
 */
static const char *IR =
    "{\"ir_version\":\"1.0\","
    " \"types\":[{\"name\":\"Approval\",\"kind\":\"sum\","
    "  \"variants\":[{\"name\":\"Approved\","
    "                 \"fields\":[{\"name\":\"approver\","
    "                              \"type\":\"string\"}]},"
    "                {\"name\":\"Rejected\","
    "                 \"fields\":[{\"name\":\"approver\","
    "                              \"type\":\"string\"},"
    "                             {\"name\":\"reason\","
    "                              \"type\":\"string\"}]}]}],"
    " \"tools\":[{\"name\":\"await_human_approval\","
    "  \"input\":[{\"name\":\"prompt\",\"type\":\"string\"}],"
    "  \"output\":\"Approval\","
    "  \"effect\":{\"level\":\"mutation\"}}],"
    " \"flows\":[{\"name\":\"gate\","
    "  \"params\":[{\"name\":\"prompt\",\"type\":\"string\",\"implicit\":false}],"
    "  \"output\":\"Approval\",\"bindings\":[],"
    "  \"return\":{\"kind\":\"call\","
    "   \"tool\":\"await_human_approval\","
    "   \"args\":[{\"field\":\"prompt\","
    "     \"value\":{\"kind\":\"path\",\"segments\":[\"prompt\"]}}]}}]}";


/* ---- Case 1: flow suspends, returns a token ---- */

static char *g_token;
static char *g_trace_dir;   /* "_root/<flow>/<exec>" */

static void
test_suspend(void)
{
    rm_rf("/tmp/flowd-suspend");
    setenv("FLOWD_EXECUTION_ID_SUFFIX", "abcdef", 1);
    setenv("FLOWD_SUSPENSION_TOKEN",   "ssss1234abcd", 1);
    setenv("SOURCE_DATE_EPOCH", "0", 1);

    flowd_runtime *rt = flowd_load_ir(IR);
    if (!rt) { fail("suspend/load", flowd_last_error_json(NULL)); return; }

    char *susp_token = NULL;
    char *out = flowd_run(rt, "{\"prompt\":\"approve transfer?\"}",
                          "/tmp/flowd-suspend", &susp_token);
    if (out != NULL) {
        fail("suspend/run-returns-null", out);
        free(out); flowd_destroy(rt); return;
    }
    ok("suspend/run-returns-null");
    if (susp_token == NULL) {
        fail("suspend/token-set", "suspension_token is NULL");
        flowd_destroy(rt); return;
    }
    ok("suspend/token-set");
    /* Token should be the path to the suspension file. */
    struct stat st;
    if (stat(susp_token, &st) == 0) ok("suspend/token-file-exists");
    else fail("suspend/token-file-exists", susp_token);

    g_token     = susp_token;
    g_trace_dir = strdup("/tmp/flowd-suspend/gate/exec_1970_01_01_abcdef");

    /* Verify the trace's manifest says suspended. */
    DiagStream *diag = diag_create();
    diag_record_only(diag);
    trace_reader_t *r = trace_reader_open(g_trace_dir, "t", diag);
    if (r) {
        const cJSON *m = trace_reader_manifest(r);
        cJSON *status = cJSON_GetObjectItemCaseSensitive(
            (cJSON *)m, "status");
        if (cJSON_IsString(status) &&
            strcmp(cJSON_GetStringValue(status), "suspended") == 0) {
            ok("suspend/manifest-status-suspended");
        } else {
            fail("suspend/manifest-status-suspended",
                 status && cJSON_IsString(status)
                   ? cJSON_GetStringValue(status) : "absent");
        }
        trace_reader_close(r);
    } else {
        fail("suspend/manifest-readable", "no manifest");
    }
    diag_destroy(diag);
    flowd_destroy(rt);
}


/* ---- Case 2: suspension file contains the expected fields ---- */

static void
test_suspension_file(void)
{
    if (!g_token) { fail("susp-file/no-token-from-prior", NULL); return; }
    FILE *fp = fopen(g_token, "rb");
    if (!fp) { fail("susp-file/open", g_token); return; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp); rewind(fp);
    char *buf = malloc((size_t)sz + 1u);
    size_t rd = fread(buf, 1, (size_t)sz, fp); buf[rd] = '\0'; fclose(fp);   /* fread is warn_unused_result on glibc */
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) { fail("susp-file/parses", "invalid JSON"); return; }
    cJSON *flow = cJSON_GetObjectItemCaseSensitive(j, "flow");
    cJSON *node = cJSON_GetObjectItemCaseSensitive(j, "node_id");
    cJSON *cond = cJSON_GetObjectItemCaseSensitive(j, "condition");
    cJSON *input = cJSON_GetObjectItemCaseSensitive(j, "input_json");
    if (cJSON_IsString(flow) &&
        strcmp(cJSON_GetStringValue(flow), "gate") == 0) {
        ok("susp-file/flow-recorded");
    } else fail("susp-file/flow-recorded", "wrong flow");
    if (cJSON_IsString(node) && cJSON_GetStringValue(node)[0] == 'n') {
        ok("susp-file/node-id-recorded");
    } else fail("susp-file/node-id-recorded", "node_id missing or wrong");
    if (cJSON_IsString(cond) &&
        strcmp(cJSON_GetStringValue(cond), "human_approval") == 0) {
        ok("susp-file/condition-human-approval");
    } else fail("susp-file/condition-human-approval", "cond mismatch");
    if (cJSON_IsString(input) &&
        strstr(cJSON_GetStringValue(input), "approve transfer") != NULL) {
        ok("susp-file/input-json-roundtrip");
    } else fail("susp-file/input-json-roundtrip", "input_json absent");
    cJSON_Delete(j);
}


/* ---- Case 3: resume with a decision completes the flow ---- */

static void
test_resume(void)
{
    if (!g_token) { fail("resume/no-token", NULL); return; }
    flowd_runtime *rt = flowd_load_ir(IR);
    char *susp = NULL;
    const char *decision =
        "{\"variant\":\"Approved\","
        " \"fields\":{\"approver\":\"alice@example.com\"}}";
    char *out = flowd_resume(rt, g_token, decision, &susp);
    if (!out) {
        fail("resume/runs", flowd_last_error_json(NULL));
        flowd_destroy(rt); return;
    }
    /* The flow returns the decision directly. Canonical form keeps
     * record fields lex-sorted and adds the variant tag. */
    assert_eq("resume/output-matches-decision", out,
              "{\"fields\":{\"approver\":\"alice@example.com\"},"
              "\"variant\":\"Approved\"}");
    free(out);
    flowd_destroy(rt);
}


/* ---- Case 4: resume trace records re_invoked at the suspended node */

static void
test_resume_cross_ref(void)
{
    /* The resumed trace is a normal sibling exec dir under the same
     * <root>/<flow>/ parent as the original, with a fresh execution id
     * tagged "_resumed" — NOT a double-nested <orig>_resumed/<flow>/...
     * path. Find the tagged exec dir directly. */
    DIR *d = opendir("/tmp/flowd-suspend/gate");
    if (!d) { fail("resume/find-trace", "gate dir missing"); return; }
    struct dirent *de;
    char resumed[1024] = {0};
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "exec_", 5) == 0 &&
            strstr(de->d_name, "_resumed") != NULL) {
            snprintf(resumed, sizeof resumed,
                     "/tmp/flowd-suspend/gate/%s", de->d_name);
            break;
        }
    }
    closedir(d);
    if (!resumed[0]) {
        fail("resume/find-trace", "no _resumed exec dir");
        return;
    }
    ok("resume/trace-dir-exists");

    /* Lock the no-double-nesting fix: the resumed exec dir must hold a
     * manifest.json directly, and must NOT contain a nested <flow>/
     * subdirectory. */
    {
        char nested[1100];
        snprintf(nested, sizeof nested, "%s/gate", resumed);
        DIR *dn = opendir(nested);
        if (dn) {
            closedir(dn);
            fail("resume/no-double-nesting", nested);
        } else {
            ok("resume/no-double-nesting");
        }
    }

    DiagStream *diag = diag_create();
    diag_record_only(diag);
    trace_reader_t *r = trace_reader_open(resumed, "t", diag);
    if (!r) {
        fail("resume/open-trace", "open failed");
        diag_destroy(diag); return;
    }
    /* n1 is the await_human_approval node in the gate flow. */
    cJSON *n1 = trace_reader_node(r, "n1");
    if (!n1) {
        fail("resume/n1-exists", "n1 missing in resume trace");
        trace_reader_close(r); diag_destroy(diag); return;
    }
    cJSON *ro   = cJSON_GetObjectItemCaseSensitive(n1, "replay_of");
    cJSON *mode = ro ? cJSON_GetObjectItemCaseSensitive(ro, "mode") : NULL;
    if (cJSON_IsString(mode) &&
        strcmp(cJSON_GetStringValue(mode), "re_invoked") == 0) {
        ok("resume/cross-ref-re-invoked");
    } else {
        fail("resume/cross-ref-re-invoked",
             mode && cJSON_IsString(mode)
               ? cJSON_GetStringValue(mode) : "absent");
    }
    cJSON_Delete(n1);

    /* The synthesized output node (n2) of a RESUME is produced by live
     * post-suspension execution, not restored from the original trace,
     * so its provenance mode must be "re_invoked" — never
     * "restored_from_trace". Guards the resume output-node mislabel. */
    cJSON *n2 = trace_reader_node(r, "n2");
    if (!n2) {
        fail("resume/output-node-exists", "n2 missing in resume trace");
    } else {
        cJSON *kind = cJSON_GetObjectItemCaseSensitive(n2, "node_kind");
        cJSON *ro2  = cJSON_GetObjectItemCaseSensitive(n2, "replay_of");
        cJSON *m2   = ro2 ? cJSON_GetObjectItemCaseSensitive(ro2, "mode")
                          : NULL;
        if (cJSON_IsString(kind) &&
            strcmp(cJSON_GetStringValue(kind), "output") == 0 &&
            cJSON_IsString(m2) &&
            strcmp(cJSON_GetStringValue(m2), "re_invoked") == 0) {
            ok("resume/output-node-re-invoked");
        } else {
            fail("resume/output-node-re-invoked",
                 m2 && cJSON_IsString(m2) ? cJSON_GetStringValue(m2)
                                          : "absent");
        }
        cJSON_Delete(n2);
    }

    const cJSON *m = trace_reader_manifest(r);
    cJSON *st = cJSON_GetObjectItemCaseSensitive((cJSON *)m, "status");
    if (cJSON_IsString(st) &&
        strcmp(cJSON_GetStringValue(st), "complete") == 0) {
        ok("resume/manifest-status-complete");
    } else {
        fail("resume/manifest-status-complete",
             st && cJSON_IsString(st)
               ? cJSON_GetStringValue(st) : "absent");
    }

    /* Lineage is recorded in the manifest, pointing back at the
     * original suspended trace dir. */
    cJSON *rf = cJSON_GetObjectItemCaseSensitive((cJSON *)m, "resumed_from");
    if (cJSON_IsString(rf) &&
        strstr(cJSON_GetStringValue(rf), "/gate/exec_") != NULL &&
        strstr(cJSON_GetStringValue(rf), "_resumed") == NULL) {
        ok("resume/manifest-resumed-from");
    } else {
        fail("resume/manifest-resumed-from",
             rf && cJSON_IsString(rf)
               ? cJSON_GetStringValue(rf) : "absent");
    }
    trace_reader_close(r);
    diag_destroy(diag);
}

int
main(void)
{
    test_suspend();
    test_suspension_file();
    test_resume();
    test_resume_cross_ref();
    free(g_token);
    free(g_trace_dir);
    printf("\nPASS %d  FAIL %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
