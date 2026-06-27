/* tests/api/test_gateway.c
 *
 * Model gateway — end-to-end.
 *
 * Verifies:
 *   1. flowd_register_provider accepts a valid adapter, rejects a
 *      malformed one.
 *   2. EFFECT_MODEL tool calls route through the gateway when an
 *      adapter claims the model.
 *   3. Gateway fall-through: if no adapter supports the model BUT a
 *      flowd_register_model fn is registered, dispatch goes there.
 *   4. The trace's model_call node carries the provider name set by
 *      the gateway adapter.
 *   5. Adapter ordering: adapters are tried in registration order;
 *      the first match wins.
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
    printf("not ok %s\n    %s\n", n, d ? d : "(no detail)");
    g_fail++;
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

/* Stub adapter implementations. */

static const char *stub_provider_a(void)         { return "stub-a"; }
static const char *stub_provider_b(void)         { return "stub-b"; }

static int support_gpt(const char *m, void *ctx) {
    (void)ctx; return strncmp(m, "gpt-", 4) == 0;
}
static int support_claude(const char *m, void *ctx) {
    (void)ctx; return strncmp(m, "claude-", 7) == 0;
}
static int support_any(const char *m, void *ctx) {
    (void)m; (void)ctx; return 1;
}

static char *
stub_invoke_echo_provider(const char *model, const char *req,
                          char **err, void *user_ctx)
{
    (void)req; (void)err;
    const char *provider = (const char *)user_ctx;
    /* Return a string containing both provider and model so the test
     * can verify which adapter served the call. */
    size_t n = strlen(provider) + strlen(model) + 16u;
    char *out = malloc(n);
    snprintf(out, n, "\"%s:%s\"", provider, model);
    return out;
}

static const flowd_provider_adapter_t adapter_openai = {
    .provider_name  = stub_provider_a,
    .supports_model = support_gpt,
    .invoke         = stub_invoke_echo_provider,
    .user_ctx       = (void *)"openai",
};

static const flowd_provider_adapter_t adapter_anthropic = {
    .provider_name  = stub_provider_b,
    .supports_model = support_claude,
    .invoke         = stub_invoke_echo_provider,
    .user_ctx       = (void *)"anthropic",
};

static const flowd_provider_adapter_t adapter_catchall = {
    .provider_name  = stub_provider_a,
    .supports_model = support_any,
    .invoke         = stub_invoke_echo_provider,
    .user_ctx       = (void *)"catchall",
};

static const flowd_provider_adapter_t adapter_malformed = {
    /* missing provider_name */
    .provider_name  = NULL,
    .supports_model = support_gpt,
    .invoke         = stub_invoke_echo_provider,
    .user_ctx       = NULL,
};

/* IR with two model tools: one for gpt, one for claude. */
static const char *IR =
    "{\"ir_version\":\"1.0\",\"types\":[],"
    " \"tools\":["
    "  {\"name\":\"ask_gpt\","
    "   \"input\":[{\"name\":\"q\",\"type\":\"string\"}],"
    "   \"output\":\"string\","
    "   \"effect\":{\"level\":\"model\",\"model\":\"gpt-4o-mini\"}},"
    "  {\"name\":\"ask_claude\","
    "   \"input\":[{\"name\":\"q\",\"type\":\"string\"}],"
    "   \"output\":\"string\","
    "   \"effect\":{\"level\":\"model\",\"model\":\"claude-opus-4\"}}],"
    " \"flows\":["
    "  {\"name\":\"gpt_flow\","
    "   \"params\":[{\"name\":\"q\",\"type\":\"string\",\"implicit\":false}],"
    "   \"output\":\"string\",\"bindings\":[],"
    "   \"return\":{\"kind\":\"call\",\"tool\":\"ask_gpt\","
    "    \"args\":[{\"field\":\"q\",\"value\":"
    "     {\"kind\":\"path\",\"segments\":[\"q\"]}}]}},"
    "  {\"name\":\"claude_flow\","
    "   \"params\":[{\"name\":\"q\",\"type\":\"string\",\"implicit\":false}],"
    "   \"output\":\"string\",\"bindings\":[],"
    "   \"return\":{\"kind\":\"call\",\"tool\":\"ask_claude\","
    "    \"args\":[{\"field\":\"q\",\"value\":"
    "     {\"kind\":\"path\",\"segments\":[\"q\"]}}]}}]}";


/* ---- Case 1: registration accepts/rejects correctly ---- */

static void
test_register(void)
{
    flowd_runtime *rt = flowd_load_ir(IR);
    if (!rt) { fail("register/load", flowd_last_error_json(NULL)); return; }
    int rc = flowd_register_provider(rt, &adapter_openai);
    if (rc == 0) ok("register/valid-adapter-ok");
    else {
        char buf[64]; snprintf(buf, sizeof buf, "rc=%d", rc);
        fail("register/valid-adapter-ok", buf);
    }
    rc = flowd_register_provider(rt, &adapter_malformed);
    if (rc == 150) ok("register/malformed-rejected");
    else {
        char buf[64]; snprintf(buf, sizeof buf, "expected 150 got %d", rc);
        fail("register/malformed-rejected", buf);
    }
    flowd_destroy(rt);
}


/* ---- Case 2: gateway routes by model id ---- */

static void
test_dispatch_by_model(void)
{
    flowd_runtime *rt = flowd_load_ir(IR);
    flowd_register_provider(rt, &adapter_openai);
    flowd_register_provider(rt, &adapter_anthropic);

    char *susp = NULL;
    char *out = flowd_run_named(rt, "gpt_flow", "\"hello\"", NULL, &susp);
    assert_eq("dispatch/openai-claims-gpt-4o-mini",
              out, "\"openai:gpt-4o-mini\"");
    free(out);

    out = flowd_run_named(rt, "claude_flow", "\"hello\"", NULL, &susp);
    assert_eq("dispatch/anthropic-claims-claude-opus-4",
              out, "\"anthropic:claude-opus-4\"");
    free(out);

    flowd_destroy(rt);
}


/* ---- Case 3: adapter order — first match wins ---- */

static void
test_first_match_wins(void)
{
    flowd_runtime *rt = flowd_load_ir(IR);
    /* Register the catchall FIRST; even though openai also matches
     * gpt-4o-mini, the catchall is checked first. */
    flowd_register_provider(rt, &adapter_catchall);
    flowd_register_provider(rt, &adapter_openai);

    char *susp = NULL;
    char *out = flowd_run_named(rt, "gpt_flow", "\"hi\"", NULL, &susp);
    assert_eq("order/catchall-wins-when-registered-first",
              out, "\"catchall:gpt-4o-mini\"");
    free(out);
    flowd_destroy(rt);
}


/* ---- Case 4: fall through to flowd_register_model ---- */

static char *
direct_model_fn(const char *args, char **err, void *ctx)
{
    (void)args; (void)err; (void)ctx;
    return strdup("\"direct\"");
}

static void
test_gateway_falls_through_to_direct(void)
{
    flowd_runtime *rt = flowd_load_ir(IR);
    /* Register only the claude adapter; the openai-flavored call has
     * no gateway adapter. flowd_register_model fills the gap. */
    flowd_register_provider(rt, &adapter_anthropic);
    int rc = flowd_register_model(rt, "ask_gpt",
                                  "(string) -> string",
                                  direct_model_fn, "v1", NULL);
    if (rc != 0) {
        char buf[64]; snprintf(buf, sizeof buf, "register_model rc=%d", rc);
        fail("fall-through/register-model", buf); flowd_destroy(rt); return;
    }
    char *susp = NULL;
    char *out = flowd_run_named(rt, "gpt_flow", "\"x\"", NULL, &susp);
    assert_eq("fall-through/direct-impl-fires", out, "\"direct\"");
    free(out);
    flowd_destroy(rt);
}


/* ---- Case 5: trace records the provider name ---- */

static void
rm_rf(const char *path) {
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

static void
test_trace_records_provider(void)
{
    rm_rf("/tmp/flowd-test-gateway");
    setenv("FLOWD_EXECUTION_ID_SUFFIX", "abcdef", 1);
    setenv("SOURCE_DATE_EPOCH", "0", 1);

    flowd_runtime *rt = flowd_load_ir(IR);
    flowd_register_provider(rt, &adapter_openai);

    char *susp = NULL;
    char *out = flowd_run_named(rt, "gpt_flow", "\"q\"",
                                "/tmp/flowd-test-gateway", &susp);
    free(out);
    flowd_destroy(rt);

    DiagStream *diag = diag_create();
    diag_record_only(diag);
    trace_reader_t *r = trace_reader_open(
        "/tmp/flowd-test-gateway/gpt_flow/exec_1970_01_01_abcdef",
        "test", diag);
    if (!r) {
        fail("trace/open", "reader open failed");
        diag_destroy(diag);
        return;
    }
    /* The model_call node is n1 (after the input node n0). */
    cJSON *n1 = trace_reader_node(r, "n1");
    if (!n1) {
        fail("trace/n1-missing", "expected n1 to exist");
        trace_reader_close(r); diag_destroy(diag);
        return;
    }
    cJSON *kind = cJSON_GetObjectItemCaseSensitive(n1, "node_kind");
    cJSON *prov = cJSON_GetObjectItemCaseSensitive(n1, "provider");
    cJSON *mdl  = cJSON_GetObjectItemCaseSensitive(n1, "model");
    if (cJSON_IsString(kind) &&
        strcmp(cJSON_GetStringValue(kind), "model_call") == 0) {
        ok("trace/node-kind-is-model_call");
    } else {
        fail("trace/node-kind-is-model_call", "wrong kind");
    }
    if (cJSON_IsString(prov) &&
        strcmp(cJSON_GetStringValue(prov), "stub-a") == 0) {
        ok("trace/provider-recorded");
    } else {
        fail("trace/provider-recorded",
             prov && cJSON_IsString(prov)
               ? cJSON_GetStringValue(prov) : "(absent or non-string)");
    }
    if (cJSON_IsString(mdl) &&
        strcmp(cJSON_GetStringValue(mdl), "gpt-4o-mini") == 0) {
        ok("trace/model-id-recorded");
    } else {
        fail("trace/model-id-recorded", "model id mismatch");
    }
    cJSON_Delete(n1);
    trace_reader_close(r);
    diag_destroy(diag);
}


/* ---- Case 6: no adapter + no fallback impl → R101 ---- */

static void
test_no_adapter_no_impl(void)
{
    flowd_runtime *rt = flowd_load_ir(IR);
    /* Register an adapter for claude only; call gpt_flow which needs
     * an unregistered model. No direct register_model either. */
    flowd_register_provider(rt, &adapter_anthropic);
    char *susp = NULL;
    char *out = flowd_run_named(rt, "gpt_flow", "\"x\"", NULL, &susp);
    if (out != NULL) {
        fail("missing/should-fail", out);
        free(out);
    } else {
        char *err = flowd_last_error_json(NULL);
        if (err && strstr(err, "R101")) {
            ok("missing/reports-R101");
        } else {
            fail("missing/error-message",
                 err ? err : "(no error message)");
        }
        free(err);
    }
    flowd_destroy(rt);
}


/* ---- Case 7: cache hit returns previously-recorded output ---- */

/* Adapter counter — increments on every invoke. The cache test
 * calls twice with identical inputs and asserts the counter only
 * incremented once. */
static int g_invoke_count = 0;

static char *
stub_invoke_counting(const char *model, const char *req,
                     char **err, void *ctx)
{
    (void)req; (void)err; (void)ctx;
    g_invoke_count++;
    size_t n = strlen(model) + 32u;
    char *out = malloc(n);
    snprintf(out, n, "\"called:%s\"", model);
    return out;
}

static const flowd_provider_adapter_t adapter_counting = {
    .provider_name  = stub_provider_a,
    .supports_model = support_any,
    .invoke         = stub_invoke_counting,
    .user_ctx       = NULL,
};

static void
test_cache_hit(void)
{
    g_invoke_count = 0;
    flowd_runtime *rt = flowd_load_ir(IR);
    flowd_register_provider(rt, &adapter_counting);

    char *susp = NULL;
    /* First call: cache miss, adapter invoked. */
    char *out1 = flowd_run_named(rt, "gpt_flow", "\"x\"", NULL, &susp);
    /* Second call with identical input: cache hit, adapter NOT
     * invoked again. */
    char *out2 = flowd_run_named(rt, "gpt_flow", "\"x\"", NULL, &susp);

    if (out1 && out2 && strcmp(out1, out2) == 0) {
        ok("cache/same-input-same-output");
    } else {
        fail("cache/same-input-same-output", "outputs differ");
    }
    if (g_invoke_count == 1) {
        ok("cache/second-call-was-hit");
    } else {
        char buf[64];
        snprintf(buf, sizeof buf,
                 "expected 1 adapter invoke, got %d", g_invoke_count);
        fail("cache/second-call-was-hit", buf);
    }
    free(out1); free(out2);
    flowd_destroy(rt);
}


/* ---- Case 8: retry policy — adapter that fails twice then succeeds */

static int g_retry_attempts = 0;

static char *
stub_invoke_flaky(const char *model, const char *req,
                  char **err, void *ctx)
{
    (void)model; (void)req; (void)ctx;
    g_retry_attempts++;
    if (g_retry_attempts < 3) {
        *err = strdup("transient network glitch");
        return NULL;
    }
    return strdup("\"recovered\"");
}

static const flowd_provider_adapter_t adapter_flaky = {
    .provider_name  = stub_provider_a,
    .supports_model = support_any,
    .invoke         = stub_invoke_flaky,
    .user_ctx       = NULL,
};

static void
test_retry_succeeds_eventually(void)
{
    /* IR with retry: count=3 on the model tool. The flaky adapter
     * fails twice then succeeds; the wrapper should retry and
     * surface the third-attempt success. */
    const char *ir =
        "{\"ir_version\":\"1.0\",\"types\":[],"
        " \"tools\":[{\"name\":\"flaky\","
        "  \"input\":[{\"name\":\"x\",\"type\":\"string\"}],"
        "  \"output\":\"string\","
        "  \"effect\":{\"level\":\"model\",\"model\":\"any\","
        "              \"retry\":{\"kind\":\"count\",\"value\":3}}}],"
        " \"flows\":[{\"name\":\"f\","
        "  \"params\":[{\"name\":\"x\",\"type\":\"string\",\"implicit\":true}],"
        "  \"output\":\"string\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"call\",\"tool\":\"flaky\","
        "   \"args\":[{\"field\":\"x\",\"value\":"
        "    {\"kind\":\"path\",\"segments\":[\"x\"]}}]}}]}";

    g_retry_attempts = 0;
    setenv("FLOWD_BACKOFF_OFF", "1", 1);
    flowd_runtime *rt = flowd_load_ir(ir);
    flowd_register_provider(rt, &adapter_flaky);
    char *susp = NULL;
    char *out = flowd_run(rt, "\"hi\"", NULL, &susp);
    assert_eq("retry/recovers-on-3rd-attempt", out, "\"recovered\"");
    if (g_retry_attempts == 3) ok("retry/attempt-count");
    else {
        char buf[64];
        snprintf(buf, sizeof buf,
                 "expected 3 attempts got %d", g_retry_attempts);
        fail("retry/attempt-count", buf);
    }
    free(out);
    flowd_destroy(rt);
}


/* ---- Case 9: retry exhaustion → R130 ---- */

static char *
stub_invoke_always_fails(const char *m, const char *r, char **err, void *c)
{
    (void)m; (void)r; (void)c;
    *err = strdup("network unreachable");
    return NULL;
}

static const flowd_provider_adapter_t adapter_always_fails = {
    .provider_name  = stub_provider_a,
    .supports_model = support_any,
    .invoke         = stub_invoke_always_fails,
    .user_ctx       = NULL,
};

static void
test_retry_exhaustion(void)
{
    const char *ir =
        "{\"ir_version\":\"1.0\",\"types\":[],"
        " \"tools\":[{\"name\":\"dead\","
        "  \"input\":[{\"name\":\"x\",\"type\":\"string\"}],"
        "  \"output\":\"string\","
        "  \"effect\":{\"level\":\"model\",\"model\":\"any\","
        "              \"retry\":{\"kind\":\"count\",\"value\":2}}}],"
        " \"flows\":[{\"name\":\"f\","
        "  \"params\":[{\"name\":\"x\",\"type\":\"string\",\"implicit\":true}],"
        "  \"output\":\"string\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"call\",\"tool\":\"dead\","
        "   \"args\":[{\"field\":\"x\",\"value\":"
        "    {\"kind\":\"path\",\"segments\":[\"x\"]}}]}}]}";

    setenv("FLOWD_BACKOFF_OFF", "1", 1);
    flowd_runtime *rt = flowd_load_ir(ir);
    flowd_register_provider(rt, &adapter_always_fails);
    char *susp = NULL;
    char *out = flowd_run(rt, "\"x\"", NULL, &susp);
    if (out != NULL) {
        fail("retry/exhaustion-fails", out);
        free(out);
    } else {
        char *err = flowd_last_error_json(NULL);
        if (err && strstr(err, "R130")) ok("retry/exhaustion-R130");
        else fail("retry/exhaustion-R130",
                  err ? err : "(no error)");
        free(err);
    }
    flowd_destroy(rt);
}


/* ---- Case 10: noretry: prefix skips retry, surfaces as R101 ---- */

static char *
stub_invoke_noretry(const char *m, const char *r, char **err, void *c)
{
    (void)m; (void)r; (void)c;
    *err = strdup("noretry: HTTP 422 schema mismatch");
    return NULL;
}

static const flowd_provider_adapter_t adapter_noretry = {
    .provider_name  = stub_provider_a,
    .supports_model = support_any,
    .invoke         = stub_invoke_noretry,
    .user_ctx       = NULL,
};

static void
test_application_error_no_retry(void)
{
    const char *ir =
        "{\"ir_version\":\"1.0\",\"types\":[],"
        " \"tools\":[{\"name\":\"app\","
        "  \"input\":[{\"name\":\"x\",\"type\":\"string\"}],"
        "  \"output\":\"string\","
        "  \"effect\":{\"level\":\"model\",\"model\":\"any\","
        "              \"retry\":{\"kind\":\"count\",\"value\":99}}}],"
        " \"flows\":[{\"name\":\"f\","
        "  \"params\":[{\"name\":\"x\",\"type\":\"string\",\"implicit\":true}],"
        "  \"output\":\"string\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"call\",\"tool\":\"app\","
        "   \"args\":[{\"field\":\"x\",\"value\":"
        "    {\"kind\":\"path\",\"segments\":[\"x\"]}}]}}]}";

    flowd_runtime *rt = flowd_load_ir(ir);
    flowd_register_provider(rt, &adapter_noretry);
    char *susp = NULL;
    char *out = flowd_run(rt, "\"x\"", NULL, &susp);
    if (out != NULL) {
        fail("noretry/should-fail", out);
        free(out);
    } else {
        char *err = flowd_last_error_json(NULL);
        /* App errors surface as R101 even with retry: count=99 set;
         * the noretry: prefix bypasses retry. */
        if (err && strstr(err, "R101")) ok("noretry/R101-not-R130");
        else fail("noretry/R101-not-R130",
                  err ? err : "(no error)");
        free(err);
    }
    flowd_destroy(rt);
}


/* ---- Case 10b: cooperative cancel aborts a RETRY_FOREVER loop ---- */

static flowd_runtime *g_cancel_rt = NULL;
static int g_cancel_calls = 0;

static char *
stub_invoke_cancel_then_fail(const char *m, const char *r, char **err, void *c)
{
    (void)m; (void)r; (void)c;
    g_cancel_calls++;
    if (g_cancel_calls == 2) {
        /* Mid-retry, request cancellation. The gateway's next loop check
         * (and its sliced backoff sleep) must observe it and abort the
         * otherwise-infinite RETRY_FOREVER loop. */
        flowd_cancel(g_cancel_rt);
    }
    if (g_cancel_calls >= 3) {
        /* Safety net: if cancellation were ignored the loop would spin
         * forever; a noretry: error terminates it so the test fails
         * cleanly (wrong code) rather than hanging. */
        *err = strdup("noretry: cancel was not honored");
        return NULL;
    }
    *err = strdup("network unreachable");   /* retryable */
    return NULL;
}

static const flowd_provider_adapter_t adapter_cancel_then_fail = {
    .provider_name  = stub_provider_a,
    .supports_model = support_any,
    .invoke         = stub_invoke_cancel_then_fail,
    .user_ctx       = NULL,
};

static void
test_retry_cancelled(void)
{
    const char *ir =
        "{\"ir_version\":\"1.0\",\"types\":[],"
        " \"tools\":[{\"name\":\"dead\","
        "  \"input\":[{\"name\":\"x\",\"type\":\"string\"}],"
        "  \"output\":\"string\","
        "  \"effect\":{\"level\":\"model\",\"model\":\"any\","
        "              \"retry\":{\"kind\":\"forever\"}}}],"
        " \"flows\":[{\"name\":\"f\","
        "  \"params\":[{\"name\":\"x\",\"type\":\"string\",\"implicit\":true}],"
        "  \"output\":\"string\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"call\",\"tool\":\"dead\","
        "   \"args\":[{\"field\":\"x\",\"value\":"
        "    {\"kind\":\"path\",\"segments\":[\"x\"]}}]}}]}";

    setenv("FLOWD_BACKOFF_OFF", "1", 1);
    g_cancel_calls = 0;
    flowd_runtime *rt = flowd_load_ir(ir);
    if (!rt) { fail("retry/cancelled-load", flowd_last_error_json(NULL)); return; }
    g_cancel_rt = rt;
    flowd_register_provider(rt, &adapter_cancel_then_fail);
    char *susp = NULL;
    char *out = flowd_run(rt, "\"x\"", NULL, &susp);
    if (out != NULL) {
        fail("retry/cancelled-aborts", out);
        free(out);
    } else {
        char *err = flowd_last_error_json(NULL);
        if (err && strstr(err, "R160")) ok("retry/cancelled-R160");
        else fail("retry/cancelled-R160", err ? err : "(no error)");
        free(err);
    }
    g_cancel_rt = NULL;
    flowd_destroy(rt);
}


/* ---- Case 11: pick using <model> ---- */

static char *
stub_pick_first(const char *m, const char *r, char **err, void *c)
{
    (void)m; (void)r; (void)err; (void)c;
    return strdup("{\"index\":0}");
}
static char *
stub_pick_second(const char *m, const char *r, char **err, void *c)
{
    (void)m; (void)r; (void)err; (void)c;
    return strdup("{\"index\":1}");
}

static const flowd_provider_adapter_t adapter_pick_first = {
    .provider_name  = stub_provider_a,
    .supports_model = support_any,
    .invoke         = stub_pick_first,
    .user_ctx       = NULL,
};

static const flowd_provider_adapter_t adapter_pick_second = {
    .provider_name  = stub_provider_a,
    .supports_model = support_any,
    .invoke         = stub_pick_second,
    .user_ctx       = NULL,
};

static char *
fetch_three(const char *args, char **err, void *ctx)
{
    (void)args; (void)err; (void)ctx;
    return strdup("[\"alice\",\"bob\",\"carol\"]");
}

static void
test_pick_using(void)
{
    /* IR with: fetch() returning [string], pipeline = fetch() | pick using primary */
    const char *ir =
        "{\"ir_version\":\"1.0\",\"types\":[],"
        " \"tools\":[{\"name\":\"fetch\","
        "  \"input\":[{\"name\":\"x\",\"type\":\"int\"}],"
        "  \"output\":\"[string]\","
        "  \"effect\":{\"level\":\"pure\"}}],"
        " \"flows\":[{\"name\":\"choose\","
        "  \"params\":[{\"name\":\"x\",\"type\":\"int\",\"implicit\":false}],"
        "  \"output\":\"string\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"pipeline\","
        "   \"source\":{\"kind\":\"call\",\"tool\":\"fetch\","
        "    \"args\":[{\"field\":\"x\","
        "     \"value\":{\"kind\":\"path\",\"segments\":[\"x\"]}}]},"
        "   \"stages\":[{\"kind\":\"terminal\",\"op\":\"pick\","
        "                \"model\":\"primary\"}]}}]}";

    /* First registration: model picks index 0 → "alice" */
    flowd_runtime *rt = flowd_load_ir(ir);
    flowd_register_tool(rt, "fetch", FLOWD_EFFECT_PURE,
                        "(int) -> [string]", fetch_three, "v1", NULL);
    flowd_register_provider(rt, &adapter_pick_first);
    char *susp = NULL;
    char *out = flowd_run(rt, "{\"x\":0}", NULL, &susp);
    assert_eq("pick/index-0-alice", out, "\"alice\"");
    free(out);
    flowd_destroy(rt);

    /* Second registration: model picks index 1 → "bob" */
    rt = flowd_load_ir(ir);
    flowd_register_tool(rt, "fetch", FLOWD_EFFECT_PURE,
                        "(int) -> [string]", fetch_three, "v1", NULL);
    flowd_register_provider(rt, &adapter_pick_second);
    out = flowd_run(rt, "{\"x\":0}", NULL, &susp);
    assert_eq("pick/index-1-bob", out, "\"bob\"");
    free(out);
    flowd_destroy(rt);
}


/* ---- same-model replay ---- */

static int g_replay_adapter_calls = 0;
static char *
stub_invoke_counts_and_echoes(const char *model, const char *req,
                              char **err, void *ctx)
{
    (void)req; (void)err; (void)ctx;
    g_replay_adapter_calls++;
    size_t n = strlen(model) + 32u;
    char *out = malloc(n);
    snprintf(out, n, "\"original:%s\"", model);
    return out;
}
static const flowd_provider_adapter_t adapter_replay_count = {
    .provider_name  = stub_provider_a,
    .supports_model = support_any,
    .invoke         = stub_invoke_counts_and_echoes,
    .user_ctx       = NULL,
};

#include <dirent.h>
static void rm_rf2(const char *path) {
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char c[1024]; snprintf(c, sizeof c, "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(c, &st) == 0 && S_ISDIR(st.st_mode)) rm_rf2(c);
        else unlink(c);
    }
    closedir(d); rmdir(path);
}

static void
test_same_model_replay(void)
{
    /* Run the flow once writing a trace. Then replay against the
     * trace; assert the adapter was NOT invoked the second time. */
    rm_rf2("/tmp/flowd-replay-orig");
    rm_rf2("/tmp/flowd-replay-new");
    setenv("FLOWD_EXECUTION_ID_SUFFIX", "abcdef", 1);
    setenv("SOURCE_DATE_EPOCH", "0", 1);

    g_replay_adapter_calls = 0;
    flowd_runtime *rt = flowd_load_ir(IR);
    flowd_register_provider(rt, &adapter_replay_count);

    /* Original run. */
    char *susp = NULL;
    char *out = flowd_run_named(rt, "gpt_flow", "\"hi\"",
                                "/tmp/flowd-replay-orig", &susp);
    if (!out) {
        fail("replay/original-run", flowd_last_error_json(NULL));
        flowd_destroy(rt); return;
    }
    int adapter_calls_after_original = g_replay_adapter_calls;
    free(out);
    flowd_destroy(rt);

    /* Replay. */
    g_replay_adapter_calls = 0;
    rt = flowd_load_ir(IR);
    flowd_register_provider(rt, &adapter_replay_count);
    char *replayed = flowd_replay(rt, "gpt_flow",
        "/tmp/flowd-replay-orig/gpt_flow/exec_1970_01_01_abcdef",
        "/tmp/flowd-replay-new", NULL);
    if (!replayed) {
        fail("replay/same-model-runs",
             flowd_last_error_json(NULL));
        flowd_destroy(rt); return;
    }
    /* Same-model replay should produce identical output. */
    /* The original output was "\"original:gpt-4o-mini\"" — the value
     * restored from the trace. */
    assert_eq("replay/same-model-output",
              replayed, "\"original:gpt-4o-mini\"");
    free(replayed);
    /* And the adapter was NOT invoked. */
    if (g_replay_adapter_calls == 0) {
        ok("replay/no-provider-invocation");
    } else {
        char buf[64];
        snprintf(buf, sizeof buf,
                 "expected 0 adapter calls during replay, got %d",
                 g_replay_adapter_calls);
        fail("replay/no-provider-invocation", buf);
    }

    /* Verify the replay trace has replay_of cross-references. */
    DiagStream *diag = diag_create();
    diag_record_only(diag);
    trace_reader_t *r = trace_reader_open(
        "/tmp/flowd-replay-new/gpt_flow/exec_1970_01_01_abcdef",
        "test", diag);
    if (r) {
        cJSON *n1 = trace_reader_node(r, "n1");
        cJSON *ro = cJSON_GetObjectItemCaseSensitive(n1, "replay_of");
        cJSON *mode = ro
            ? cJSON_GetObjectItemCaseSensitive(ro, "mode") : NULL;
        if (cJSON_IsString(mode) &&
            strcmp(cJSON_GetStringValue(mode),
                   "restored_from_trace") == 0) {
            ok("replay/cross-ref-restored");
        } else {
            fail("replay/cross-ref-restored",
                 "n1.replay_of.mode missing or wrong");
        }
        cJSON_Delete(n1);
        trace_reader_close(r);
    } else {
        fail("replay/trace-readable", "new trace not openable");
    }
    diag_destroy(diag);

    (void)adapter_calls_after_original;
    flowd_destroy(rt);
}


/* ---- model-versioned replay ---- */

static char *
stub_invoke_new_model(const char *model, const char *req,
                      char **err, void *ctx)
{
    (void)req; (void)err; (void)ctx;
    size_t n = strlen(model) + 32u;
    char *out = malloc(n);
    snprintf(out, n, "\"new:%s\"", model);
    return out;
}

static int
support_only_gpt5(const char *m, void *ctx) {
    (void)ctx; return strcmp(m, "gpt-5") == 0;
}

static const flowd_provider_adapter_t adapter_gpt5 = {
    .provider_name  = stub_provider_a,
    .supports_model = support_only_gpt5,
    .invoke         = stub_invoke_new_model,
    .user_ctx       = NULL,
};

static void
test_model_versioned_replay(void)
{
    rm_rf2("/tmp/flowd-replayV-orig");
    rm_rf2("/tmp/flowd-replayV-new");
    setenv("FLOWD_EXECUTION_ID_SUFFIX", "abcdef", 1);
    setenv("SOURCE_DATE_EPOCH", "0", 1);

    /* Original: gpt-4o-mini via the count adapter. */
    flowd_runtime *rt = flowd_load_ir(IR);
    flowd_register_provider(rt, &adapter_replay_count);
    char *susp = NULL;
    char *orig = flowd_run_named(rt, "gpt_flow", "\"hi\"",
                                 "/tmp/flowd-replayV-orig", &susp);
    free(orig);
    flowd_destroy(rt);

    /* Replay against new model "gpt-5" via a different adapter. */
    rt = flowd_load_ir(IR);
    flowd_register_provider(rt, &adapter_gpt5);
    char *replayed = flowd_replay(rt, "gpt_flow",
        "/tmp/flowd-replayV-orig/gpt_flow/exec_1970_01_01_abcdef",
        "/tmp/flowd-replayV-new", "gpt-5");
    if (!replayed) {
        fail("model-versioned/runs", flowd_last_error_json(NULL));
        flowd_destroy(rt); return;
    }
    assert_eq("model-versioned/new-output", replayed, "\"new:gpt-5\"");
    free(replayed);

    /* Verify the replay trace's n1 has re_invoked mode. */
    DiagStream *diag = diag_create();
    diag_record_only(diag);
    trace_reader_t *r = trace_reader_open(
        "/tmp/flowd-replayV-new/gpt_flow/exec_1970_01_01_abcdef",
        "test", diag);
    if (r) {
        cJSON *n1 = trace_reader_node(r, "n1");
        cJSON *ro = cJSON_GetObjectItemCaseSensitive(n1, "replay_of");
        cJSON *mode = ro
            ? cJSON_GetObjectItemCaseSensitive(ro, "mode") : NULL;
        if (cJSON_IsString(mode) &&
            strcmp(cJSON_GetStringValue(mode),
                   "re_invoked") == 0) {
            ok("model-versioned/cross-ref-re-invoked");
        } else {
            fail("model-versioned/cross-ref-re-invoked",
                 mode && cJSON_IsString(mode)
                   ? cJSON_GetStringValue(mode) : "absent");
        }
        cJSON *model = cJSON_GetObjectItemCaseSensitive(n1, "model");
        if (cJSON_IsString(model) &&
            strcmp(cJSON_GetStringValue(model), "gpt-5") == 0) {
            ok("model-versioned/recorded-new-model");
        } else {
            fail("model-versioned/recorded-new-model",
                 model && cJSON_IsString(model)
                   ? cJSON_GetStringValue(model) : "absent");
        }
        cJSON_Delete(n1);
        trace_reader_close(r);
    }
    diag_destroy(diag);
    flowd_destroy(rt);
}

int
main(void)
{
    test_register();
    test_dispatch_by_model();
    test_first_match_wins();
    test_gateway_falls_through_to_direct();
    test_trace_records_provider();
    test_no_adapter_no_impl();

    /* cache + retry */
    test_cache_hit();
    test_retry_succeeds_eventually();
    test_retry_exhaustion();
    test_application_error_no_retry();
    test_retry_cancelled();
    test_pick_using();

    /* replay */
    test_same_model_replay();
    test_model_versioned_replay();

    printf("\nPASS %d  FAIL %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
