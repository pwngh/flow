/* tests/api/test_anthropic.c
 *
 * End-to-end test of the Anthropic adapter against the local
 * mock server. Covers:
 *
 *   1. Adapter dispatch — model id `claude-*` routes through the
 *      adapter and the adapter's outbound request reaches the
 *      mock server with the expected shape.
 *   2. Headers — the x-api-key and anthropic-version headers are
 *      present; the api key value is the one we registered.
 *   3. Response parsing — the adapter extracts the assistant text
 *      from the Messages response shape and returns it as a JSON
 *      string the runtime can map onto the tool's declared output.
 *   4. Trace bytes never contain the API key — the load-bearing
 *      secret-isolation invariant.
 */

#include "flowd.h"
#include "trace.h"
#include "util.h"
#include "mock_server.h"
#include "../../src/adapters/anthropic.h"

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
        snprintf(buf, sizeof buf,
                 "want=%s\n    got =%s", want, got ? got : "NULL");
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

/* IR with a tool `ask` of effect `model("claude-3-5-sonnet")` and a
 * one-line flow that calls it. */
static const char *IR =
    "{\"ir_version\":\"1.0\",\"types\":[],"
    " \"tools\":[{\"name\":\"ask\","
    "  \"input\":[{\"name\":\"prompt\",\"type\":\"string\"}],"
    "  \"output\":\"string\","
    "  \"effect\":{\"level\":\"model\","
    "              \"model\":\"claude-3-5-sonnet\"}}],"
    " \"flows\":[{\"name\":\"chat\","
    "  \"params\":[{\"name\":\"prompt\",\"type\":\"string\","
    "               \"implicit\":false}],"
    "  \"output\":\"string\",\"bindings\":[],"
    "  \"return\":{\"kind\":\"call\",\"tool\":\"ask\","
    "   \"args\":[{\"field\":\"prompt\","
    "     \"value\":{\"kind\":\"path\",\"segments\":[\"prompt\"]}}]}}]}";

/* A canned 200 response body shaped like Anthropic's Messages API.
 * Includes a `usage` block so the metrics extraction can be tested. */
static const char *CANNED_RESPONSE =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 209\r\n"
    "\r\n"
    "{\"id\":\"msg_xx\",\"type\":\"message\",\"role\":\"assistant\","
    "\"model\":\"claude-3-5-sonnet\","
    "\"content\":[{\"type\":\"text\","
    "\"text\":\"The capital is Paris.\"}],"
    "\"stop_reason\":\"end_turn\","
    "\"usage\":{\"input_tokens\":7,\"output_tokens\":12}}";

static void
test_dispatch_and_extract(void)
{
    mock_server_t *srv = mock_server_start(CANNED_RESPONSE);
    if (!srv) { fail("mock/start", "could not bind"); return; }
    ok("mock/start");

    char base_url[64];
    snprintf(base_url, sizeof base_url,
             "http://127.0.0.1:%u", mock_server_port(srv));

    anthropic_adapter_config_t cfg = {
        .api_key    = "sk-test-XXXXX-secret-do-not-leak",
        .base_url   = base_url,
        .max_tokens = 32,
        .timeout_ms = 5000,
    };
    flowd_provider_adapter_t adapter = anthropic_adapter(&cfg);

    flowd_runtime *rt = flowd_load_ir(IR);
    int rc = flowd_register_provider(rt, &adapter);
    if (rc != 0) {
        char b[64]; snprintf(b, sizeof b, "rc=%d", rc);
        fail("register/provider", b); mock_server_stop(srv);
        flowd_destroy(rt); return;
    }
    ok("register/provider");

    char *susp = NULL;
    char *out = flowd_run(rt, "{\"prompt\":\"capital of France?\"}",
                          NULL, &susp);
    assert_eq("dispatch/text-output", out, "\"The capital is Paris.\"");
    free(out);

    /* Inspect what the server received. */
    const mock_capture_t *cap = mock_server_captured(srv);
    if (cap && cap->request &&
        strstr(cap->request, "POST /v1/messages") != NULL) {
        ok("request/path-correct");
    } else {
        fail("request/path-correct",
             cap && cap->request ? cap->request : "no request captured");
    }
    if (cap && strstr(cap->request, "x-api-key: sk-test-") != NULL) {
        ok("request/api-key-header-present");
    } else {
        fail("request/api-key-header-present",
             "missing x-api-key header");
    }
    if (cap && strstr(cap->request, "anthropic-version:") != NULL) {
        ok("request/version-header-present");
    } else {
        fail("request/version-header-present",
             "missing anthropic-version header");
    }
    if (cap && cap->body && strstr(cap->body, "\"capital of France?\"") != NULL) {
        ok("request/prompt-in-body");
    } else {
        fail("request/prompt-in-body",
             cap && cap->body ? cap->body : "(no body)");
    }
    if (cap && cap->body &&
        strstr(cap->body, "\"model\":\"claude-3-5-sonnet\"") != NULL) {
        ok("request/model-id-in-body");
    } else {
        fail("request/model-id-in-body",
             cap && cap->body ? cap->body : "(no body)");
    }

    mock_server_stop(srv);
    flowd_destroy(rt);
}


/* ---- API key never appears in trace bytes ---- */

static void
test_no_key_in_traces(void)
{
    rm_rf("/tmp/flowd-anthropic-trace");
    setenv("FLOWD_EXECUTION_ID_SUFFIX", "abcdef", 1);
    setenv("SOURCE_DATE_EPOCH", "0", 1);

    mock_server_t *srv = mock_server_start(CANNED_RESPONSE);
    char base_url[64];
    snprintf(base_url, sizeof base_url,
             "http://127.0.0.1:%u", mock_server_port(srv));

    /* A sentinel value we can grep the trace dir for. */
    const char *KEY = "sk-LEAK-CANARY-1234567890";
    anthropic_adapter_config_t cfg = {
        .api_key    = KEY,
        .base_url   = base_url,
        .max_tokens = 32,
        .timeout_ms = 5000,
    };
    flowd_provider_adapter_t adapter = anthropic_adapter(&cfg);

    flowd_runtime *rt = flowd_load_ir(IR);
    flowd_register_provider(rt, &adapter);
    char *susp = NULL;
    char *out = flowd_run(rt, "{\"prompt\":\"x\"}",
                          "/tmp/flowd-anthropic-trace", &susp);
    free(out);
    flowd_destroy(rt);
    mock_server_stop(srv);

    /* Walk every file in the trace dir and verify the key sentinel
     * appears in none of them. */
    int found = 0;
    char cmd[256];
    snprintf(cmd, sizeof cmd,
             "grep -rq '%s' /tmp/flowd-anthropic-trace && echo FOUND || echo CLEAN",
             KEY);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        char line[32] = {0};
        if (fgets(line, sizeof line, fp)) {
            if (strstr(line, "FOUND") != NULL) found = 1;
        }
        pclose(fp);
    }
    if (!found) ok("no-key/trace-bytes-clean");
    else fail("no-key/trace-bytes-clean",
              "API key sentinel leaked into a trace file");
}


/* ---- token metrics flow through the v2 invoke path ---- */

static void
test_metrics_extracted(void)
{
    mock_server_t *srv = mock_server_start(CANNED_RESPONSE);
    if (!srv) { fail("metrics/mock-start", "could not bind"); return; }

    char base_url[64];
    snprintf(base_url, sizeof base_url,
             "http://127.0.0.1:%u", mock_server_port(srv));

    anthropic_adapter_config_t cfg = {
        .api_key    = "sk-test-XXXXX-secret-do-not-leak",
        .base_url   = base_url,
        .max_tokens = 32,
        .timeout_ms = 5000,
    };
    flowd_provider_adapter_t adapter = anthropic_adapter(&cfg);

    if (adapter.invoke_with_metrics == NULL) {
        fail("metrics/v2-entry-set", "invoke_with_metrics is NULL");
        mock_server_stop(srv); return;
    }
    ok("metrics/v2-entry-set");

    flowd_adapter_response_t r = {0};
    adapter.invoke_with_metrics("claude-3-5-sonnet",
                                "{\"prompt\":\"capital?\"}",
                                &r, adapter.user_ctx);
    if (r.response_json == NULL) {
        fail("metrics/invoke", r.err_msg ? r.err_msg : "(no error)");
        free(r.err_msg); mock_server_stop(srv); return;
    }
    ok("metrics/invoke");

    if (r.tokens_in == 7u)  ok("metrics/tokens_in=7");
    else {
        char b[64]; snprintf(b, sizeof b, "got %llu",
                             (unsigned long long)r.tokens_in);
        fail("metrics/tokens_in=7", b);
    }
    if (r.tokens_out == 12u) ok("metrics/tokens_out=12");
    else {
        char b[64]; snprintf(b, sizeof b, "got %llu",
                             (unsigned long long)r.tokens_out);
        fail("metrics/tokens_out=12", b);
    }

    free(r.response_json);
    free(r.err_msg);
    mock_server_stop(srv);
}


/* ---- redactor wired through flowd_run end-to-end ---- */

static char *
sentinel_redactor(const char *bytes, size_t len, size_t *out_len,
                  void *ctx)
{
    (void)ctx;
    const char *needle  = "REDACT_ME";
    const char *replace = "*********";
    size_t nlen = strlen(needle);
    size_t rlen = strlen(replace);
    if (nlen != rlen) return NULL;
    char *out = malloc(len + 1u);
    if (!out) return NULL;
    memcpy(out, bytes, len);
    out[len] = '\0';
    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(out + i, needle, nlen) == 0) {
            memcpy(out + i, replace, rlen);
        }
    }
    *out_len = len;
    return out;
}

/* IR with a passthrough deterministic tool: input string in, same
 * string out. Lets us drop a sentinel through the trace pipeline
 * without involving a model adapter. */
static const char *PASSTHRU_IR =
    "{\"ir_version\":\"1.0\",\"types\":[],"
    " \"tools\":[{\"name\":\"echo\","
    "  \"input\":[{\"name\":\"s\",\"type\":\"string\"}],"
    "  \"output\":\"string\","
    "  \"effect\":{\"level\":\"deterministic\"}}],"
    " \"flows\":[{\"name\":\"main\","
    "  \"params\":[{\"name\":\"s\",\"type\":\"string\","
    "               \"implicit\":false}],"
    "  \"output\":\"string\",\"bindings\":[],"
    "  \"return\":{\"kind\":\"call\",\"tool\":\"echo\","
    "   \"args\":[{\"field\":\"s\","
    "     \"value\":{\"kind\":\"path\",\"segments\":[\"s\"]}}]}}]}";

static char *
echo_tool(const char *args_json, char **err_json, void *user_ctx)
{
    (void)err_json; (void)user_ctx;
    cJSON *args = cJSON_Parse(args_json ? args_json : "{}");
    cJSON *sv = cJSON_GetObjectItemCaseSensitive(args, "s");
    const char *s = cJSON_IsString(sv) ? cJSON_GetStringValue(sv) : "";
    cJSON *wrap = cJSON_CreateString(s);
    char *out = cJSON_PrintUnformatted(wrap);
    cJSON_Delete(wrap); cJSON_Delete(args);
    return out;
}

static int
file_contains(const char *path, const char *needle)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char buf[8192]; size_t n = fread(buf, 1u, sizeof buf - 1u, fp);
    buf[n] = '\0'; fclose(fp);
    return strstr(buf, needle) != NULL;
}

static void
test_redactor_end_to_end(void)
{
    rm_rf("/tmp/flowd-redactor-e2e");
    setenv("FLOWD_EXECUTION_ID_SUFFIX", "redact", 1);
    setenv("SOURCE_DATE_EPOCH", "0", 1);

    flowd_runtime *rt = flowd_load_ir(PASSTHRU_IR);
    flowd_register_tool(rt, "echo", FLOWD_EFFECT_DETERMINISTIC,
                        "(string) -> string", echo_tool, "v0", NULL);
    flowd_set_redactor(rt, sentinel_redactor, NULL);

    char *susp = NULL;
    char *out = flowd_run(rt, "{\"s\":\"prefix REDACT_ME suffix\"}",
                          "/tmp/flowd-redactor-e2e", &susp);
    free(out);
    flowd_destroy(rt);

    const char *trace_dir =
        "/tmp/flowd-redactor-e2e/main/exec_1970_01_01_redact";
    const char *n0 = "/tmp/flowd-redactor-e2e/main/exec_1970_01_01_redact/nodes/n0.json";
    const char *n1 = "/tmp/flowd-redactor-e2e/main/exec_1970_01_01_redact/nodes/n1.json";

    /* The sentinel should appear in NO trace files; the redactor
     * replaced it with ********. */
    if (!file_contains(n0, "REDACT_ME") &&
        !file_contains(n1, "REDACT_ME")) {
        ok("redactor/sentinel-absent-from-trace");
    } else {
        fail("redactor/sentinel-absent-from-trace",
             "REDACT_ME leaked into trace");
    }
    /* The replacement should appear instead. */
    if (file_contains(n0, "*********") || file_contains(n1, "*********")) {
        ok("redactor/replacement-present");
    } else {
        fail("redactor/replacement-present",
             "asterisks not found in any node");
    }
    (void)trace_dir;
}

int
main(void)
{
    test_dispatch_and_extract();
    test_no_key_in_traces();
    test_metrics_extracted();
    test_redactor_end_to_end();
    printf("\nPASS %d  FAIL %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
