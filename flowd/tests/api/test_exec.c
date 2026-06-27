/* tests/api/test_exec.c
 *
 * Executor API test.
 *
 * Drives flowd_load_ir + flowd_run against three small in-memory IRs
 * that exercise the literal-and-path subset:
 *
 *   1. identity_string — flow(it: string) -> string = it
 *      Verifies: input parsing as a single-value-direct string,
 *      path-lookup of an implicit param, canonical string output.
 *
 *   2. record_field_pluck — flow(it: User) -> string = it.email
 *      Verifies: record JSON input parsing, multi-segment path,
 *      field access through a record value.
 *
 *   3. literal_only — flow(unused: int) -> string = "hello"
 *      Verifies: literal expression evaluation, the input is
 *      parsed (validating shape) but not consulted.
 *
 * Each case asserts that flowd_run returns a non-NULL JSON string
 * and that the bytes match the expected canonical form.
 *
 * On any failure the program exits non-zero and the run.sh driver
 * surfaces stderr.
 */

#include "flowd.h"
#include "util.h"
#include "ir_load.h"
#include "stub_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

static void
ok(const char *case_name)
{
    printf("  ok %s\n", case_name);
    g_pass++;
}

static void
fail(const char *case_name, const char *detail)
{
    printf("not ok %s\n    %s\n", case_name, detail ? detail : "(no detail)");
    g_fail++;
}

static void
assert_eq(const char *case_name, const char *got, const char *want)
{
    if (got == NULL) {
        fail(case_name, "got NULL output");
        return;
    }
    if (strcmp(got, want) == 0) {
        ok(case_name);
    } else {
        char buf[1024];
        snprintf(buf, sizeof buf, "want=%s\n    got =%s", want, got);
        fail(case_name, buf);
    }
}


/* ---- Case 1: identity ---- */

static void
test_identity_string(void)
{
    const char *ir =
        "{\n"
        "  \"ir_version\": \"1.0\",\n"
        "  \"types\": [],\n"
        "  \"tools\": [],\n"
        "  \"flows\": [{\n"
        "    \"name\": \"identity\",\n"
        "    \"params\": [{\"name\":\"it\",\"type\":\"string\",\"implicit\":true}],\n"
        "    \"output\": \"string\",\n"
        "    \"bindings\": [],\n"
        "    \"return\": {\"kind\":\"path\",\"segments\":[\"it\"]}\n"
        "  }]\n"
        "}\n";

    flowd_runtime *rt = flowd_load_ir(ir);
    if (rt == NULL) {
        char *err = flowd_last_error_json(NULL);
        fail("identity/load", err ? err : "(no error)");
        free(err);
        return;
    }

    /* Value-direct form: input is just the string. */
    char *susp = NULL;
    char *out  = flowd_run(rt, "\"hello\"", NULL, &susp);
    assert_eq("identity/value-direct", out, "\"hello\"");
    free(out);

    /* Object-keyed form: {"it": "world"}. */
    out = flowd_run(rt, "{\"it\":\"world\"}", NULL, &susp);
    assert_eq("identity/keyed", out, "\"world\"");
    free(out);

    flowd_destroy(rt);
}


/* ---- Case 2: record field pluck ---- */

static void
test_record_field_pluck(void)
{
    const char *ir =
        "{\n"
        "  \"ir_version\": \"1.0\",\n"
        "  \"types\": [\n"
        "    {\"name\":\"User\",\"kind\":\"record\",\n"
        "     \"fields\":[\n"
        "       {\"name\":\"email\",\"type\":\"string\"},\n"
        "       {\"name\":\"id\",\"type\":\"string\"}\n"
        "     ]}\n"
        "  ],\n"
        "  \"tools\": [],\n"
        "  \"flows\": [{\n"
        "    \"name\": \"email_of\",\n"
        "    \"params\": [{\"name\":\"it\",\"type\":\"User\",\"implicit\":true}],\n"
        "    \"output\": \"string\",\n"
        "    \"bindings\": [],\n"
        "    \"return\": {\"kind\":\"path\",\"segments\":[\"it\",\"email\"]}\n"
        "  }]\n"
        "}\n";

    flowd_runtime *rt = flowd_load_ir(ir);
    if (rt == NULL) {
        char *err = flowd_last_error_json(NULL);
        fail("record-pluck/load", err ? err : "(no error)");
        free(err);
        return;
    }

    char *susp = NULL;
    /* Value-direct: the record is the input. */
    char *out = flowd_run(rt,
        "{\"email\":\"a@b\",\"id\":\"u1\"}", NULL, &susp);
    assert_eq("record-pluck/value-direct", out, "\"a@b\"");
    free(out);

    /* Keyed: {"it": {...}}. */
    out = flowd_run(rt,
        "{\"it\":{\"email\":\"c@d\",\"id\":\"u2\"}}", NULL, &susp);
    assert_eq("record-pluck/keyed", out, "\"c@d\"");
    free(out);

    flowd_destroy(rt);
}


/* ---- Case 3: literal-only return ---- */

static void
test_literal_only(void)
{
    const char *ir =
        "{\n"
        "  \"ir_version\": \"1.0\",\n"
        "  \"types\": [],\n"
        "  \"tools\": [],\n"
        "  \"flows\": [{\n"
        "    \"name\": \"hello\",\n"
        "    \"params\": [{\"name\":\"unused\",\"type\":\"int\",\"implicit\":false}],\n"
        "    \"output\": \"string\",\n"
        "    \"bindings\": [],\n"
        "    \"return\": {\"kind\":\"literal\",\"type\":\"string\",\"value\":\"hello\"}\n"
        "  }]\n"
        "}\n";

    flowd_runtime *rt = flowd_load_ir(ir);
    if (rt == NULL) {
        char *err = flowd_last_error_json(NULL);
        fail("literal/load", err ? err : "(no error)");
        free(err);
        return;
    }

    char *susp = NULL;
    /* The input is consumed and validated even though the return
     * ignores it — this exercises value_from_json on an int. */
    char *out = flowd_run(rt, "{\"unused\":42}", NULL, &susp);
    assert_eq("literal/return-string", out, "\"hello\"");
    free(out);

    /* int literal */
    flowd_destroy(rt);

    const char *ir2 =
        "{\n"
        "  \"ir_version\": \"1.0\",\n"
        "  \"types\": [],\n"
        "  \"tools\": [],\n"
        "  \"flows\": [{\n"
        "    \"name\": \"the_answer\",\n"
        "    \"params\": [{\"name\":\"unused\",\"type\":\"int\",\"implicit\":false}],\n"
        "    \"output\": \"int\",\n"
        "    \"bindings\": [],\n"
        "    \"return\": {\"kind\":\"literal\",\"type\":\"int\",\"value\":42}\n"
        "  }]\n"
        "}\n";
    rt = flowd_load_ir(ir2);
    if (rt == NULL) {
        char *err = flowd_last_error_json(NULL);
        fail("literal-int/load", err ? err : "(no error)");
        free(err);
        return;
    }
    out = flowd_run(rt, "{\"unused\":0}", NULL, &susp);
    assert_eq("literal/return-int", out, "42");
    free(out);

    flowd_destroy(rt);
}


/* ---- Negative case: unimplemented expression kind ---- */

static void
test_nyi_call_reports(void)
{
    /* A flow whose return is a `call` — the literal-and-path subset
     * doesn't implement tool dispatch. flowd_run should fail cleanly
     * with a diagnostic. */
    const char *ir =
        "{\n"
        "  \"ir_version\": \"1.0\",\n"
        "  \"types\": [],\n"
        "  \"tools\": [{\n"
        "    \"name\":\"noop\",\n"
        "    \"input\":[{\"name\":\"x\",\"type\":\"int\"}],\n"
        "    \"output\":\"int\",\n"
        "    \"effect\":{\"level\":\"pure\"}\n"
        "  }],\n"
        "  \"flows\": [{\n"
        "    \"name\": \"call_one\",\n"
        "    \"params\": [{\"name\":\"it\",\"type\":\"int\",\"implicit\":true}],\n"
        "    \"output\": \"int\",\n"
        "    \"bindings\": [],\n"
        "    \"return\": {\"kind\":\"call\",\"tool\":\"noop\",\n"
        "                \"args\":[{\"field\":\"x\",\n"
        "                          \"value\":{\"kind\":\"path\",\n"
        "                                     \"segments\":[\"it\"]}}]}\n"
        "  }]\n"
        "}\n";

    flowd_runtime *rt = flowd_load_ir(ir);
    if (rt == NULL) {
        char *err = flowd_last_error_json(NULL);
        fail("nyi-call/load", err ? err : "(no error)");
        free(err);
        return;
    }

    char *susp = NULL;
    /* Without a registered impl for `noop`, the call dispatcher
     * reports R101 ("tool call failed after exhausted retries" —
     * with zero retries available, the failure is one-shot).
     * try/else would catch this; without one, the flow returns
     * NULL and the error blob carries the R101 code. */
    char *out  = flowd_run(rt, "1", NULL, &susp);
    if (out != NULL) {
        fail("unregistered-call/should-fail",
             "flowd_run returned non-NULL");
        free(out);
    } else {
        char *err = flowd_last_error_json(NULL);
        if (err != NULL && strstr(err, "R101") != NULL) {
            ok("unregistered-call/reports-R101");
        } else {
            fail("unregistered-call/error-message",
                 err ? err : "(no diagnostic recorded)");
        }
        free(err);
    }
    flowd_destroy(rt);
}


/* ---- construct + binop + conditional ---- */

static void
test_record_construct(void)
{
    const char *ir =
        "{\"ir_version\":\"1.0\","
        " \"types\":[{\"name\":\"Pair\",\"kind\":\"record\","
        "  \"fields\":[{\"name\":\"a\",\"type\":\"int\"},"
        "             {\"name\":\"b\",\"type\":\"int\"}]}],"
        " \"tools\":[],"
        " \"flows\":[{\"name\":\"mk\","
        "  \"params\":[{\"name\":\"x\",\"type\":\"int\",\"implicit\":false}],"
        "  \"output\":\"Pair\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"construct\",\"type\":\"Pair\","
        "   \"fields\":[{\"name\":\"a\",\"value\":{\"kind\":\"path\",\"segments\":[\"x\"]}},"
        "              {\"name\":\"b\",\"value\":{\"kind\":\"literal\",\"type\":\"int\",\"value\":99}}]}}]}";

    flowd_runtime *rt = flowd_load_ir(ir);
    if (!rt) { fail("construct/load", flowd_last_error_json(NULL)); return; }
    char *susp = NULL;
    char *out = flowd_run(rt, "{\"x\":7}", NULL, &susp);
    assert_eq("construct/record", out, "{\"a\":7,\"b\":99}");
    free(out);
    flowd_destroy(rt);
}

static void
test_binops_int(void)
{
    const char *ir_tmpl =
        "{\"ir_version\":\"1.0\",\"types\":[],\"tools\":[],"
        " \"flows\":[{\"name\":\"f\","
        "  \"params\":[{\"name\":\"a\",\"type\":\"int\",\"implicit\":false},"
        "             {\"name\":\"b\",\"type\":\"int\",\"implicit\":false}],"
        "  \"output\":\"int\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"binop\",\"op\":\"%s\","
        "   \"left\":{\"kind\":\"path\",\"segments\":[\"a\"]},"
        "   \"right\":{\"kind\":\"path\",\"segments\":[\"b\"]}}}]}";

    struct { const char *op; const char *in; const char *want; } cases[] = {
        {"+", "{\"a\":3,\"b\":4}", "7"},
        {"-", "{\"a\":10,\"b\":4}", "6"},
        {"*", "{\"a\":6,\"b\":7}", "42"},
        {"/", "{\"a\":20,\"b\":4}", "5"},
        {"%", "{\"a\":17,\"b\":5}", "2"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char buf[512];
        snprintf(buf, sizeof buf, ir_tmpl, cases[i].op);
        flowd_runtime *rt = flowd_load_ir(buf);
        if (!rt) { fail("binop-int/load", flowd_last_error_json(NULL)); continue; }
        char *susp = NULL;
        char *out = flowd_run(rt, cases[i].in, NULL, &susp);
        char name[64]; snprintf(name, sizeof name, "binop-int/%s", cases[i].op);
        assert_eq(name, out, cases[i].want);
        free(out);
        flowd_destroy(rt);
    }
}

static void
test_binops_cmp(void)
{
    /* a < b → true for a=2,b=3 */
    const char *ir =
        "{\"ir_version\":\"1.0\",\"types\":[],\"tools\":[],"
        " \"flows\":[{\"name\":\"f\","
        "  \"params\":[{\"name\":\"a\",\"type\":\"int\",\"implicit\":false},"
        "             {\"name\":\"b\",\"type\":\"int\",\"implicit\":false}],"
        "  \"output\":\"bool\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"binop\",\"op\":\"<\","
        "   \"left\":{\"kind\":\"path\",\"segments\":[\"a\"]},"
        "   \"right\":{\"kind\":\"path\",\"segments\":[\"b\"]}}}]}";

    flowd_runtime *rt = flowd_load_ir(ir);
    if (!rt) { fail("binop-cmp/load", flowd_last_error_json(NULL)); return; }
    char *susp = NULL;
    char *out = flowd_run(rt, "{\"a\":2,\"b\":3}", NULL, &susp);
    assert_eq("binop-cmp/lt-true", out, "true");
    free(out);
    out = flowd_run(rt, "{\"a\":5,\"b\":3}", NULL, &susp);
    assert_eq("binop-cmp/lt-false", out, "false");
    free(out);
    flowd_destroy(rt);
}

static void
test_short_circuit_and(void)
{
    /* `a > 0 and 10 / b > 1` would divide by zero if b==0, but the
     * short-circuit avoids that when a <= 0. */
    const char *ir =
        "{\"ir_version\":\"1.0\",\"types\":[],\"tools\":[],"
        " \"flows\":[{\"name\":\"f\","
        "  \"params\":[{\"name\":\"a\",\"type\":\"int\",\"implicit\":false},"
        "             {\"name\":\"b\",\"type\":\"int\",\"implicit\":false}],"
        "  \"output\":\"bool\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"binop\",\"op\":\"and\","
        "   \"left\":{\"kind\":\"binop\",\"op\":\">\","
        "    \"left\":{\"kind\":\"path\",\"segments\":[\"a\"]},"
        "    \"right\":{\"kind\":\"literal\",\"type\":\"int\",\"value\":0}},"
        "   \"right\":{\"kind\":\"binop\",\"op\":\">\","
        "    \"left\":{\"kind\":\"binop\",\"op\":\"/\","
        "     \"left\":{\"kind\":\"literal\",\"type\":\"int\",\"value\":10},"
        "     \"right\":{\"kind\":\"path\",\"segments\":[\"b\"]}},"
        "    \"right\":{\"kind\":\"literal\",\"type\":\"int\",\"value\":1}}}}]}";

    flowd_runtime *rt = flowd_load_ir(ir);
    if (!rt) { fail("short-circuit/load", flowd_last_error_json(NULL)); return; }
    char *susp = NULL;
    /* a=-1, b=0 → false from first branch; right not evaluated; no div-by-zero. */
    char *out = flowd_run(rt, "{\"a\":-1,\"b\":0}", NULL, &susp);
    assert_eq("short-circuit/and-skips-rhs", out, "false");
    free(out);
    flowd_destroy(rt);
}

static void
test_conditional(void)
{
    const char *ir =
        "{\"ir_version\":\"1.0\",\"types\":[],\"tools\":[],"
        " \"flows\":[{\"name\":\"sign\","
        "  \"params\":[{\"name\":\"it\",\"type\":\"int\",\"implicit\":true}],"
        "  \"output\":\"int\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"conditional\","
        "   \"branches\":[{\"cond\":{\"kind\":\"binop\",\"op\":\">\","
        "    \"left\":{\"kind\":\"path\",\"segments\":[\"it\"]},"
        "    \"right\":{\"kind\":\"literal\",\"type\":\"int\",\"value\":0}},"
        "    \"consequent\":{\"kind\":\"literal\",\"type\":\"int\",\"value\":1}},"
        "    {\"cond\":{\"kind\":\"binop\",\"op\":\"<\","
        "    \"left\":{\"kind\":\"path\",\"segments\":[\"it\"]},"
        "    \"right\":{\"kind\":\"literal\",\"type\":\"int\",\"value\":0}},"
        "    \"consequent\":{\"kind\":\"literal\",\"type\":\"int\",\"value\":-1}}],"
        "   \"else\":{\"kind\":\"literal\",\"type\":\"int\",\"value\":0}}}]}";

    flowd_runtime *rt = flowd_load_ir(ir);
    if (!rt) { fail("cond/load", flowd_last_error_json(NULL)); return; }
    char *susp = NULL;
    char *out;
    out = flowd_run(rt, "5", NULL, &susp);   assert_eq("cond/positive",  out, "1");  free(out);
    out = flowd_run(rt, "-3", NULL, &susp);  assert_eq("cond/negative",  out, "-1"); free(out);
    out = flowd_run(rt, "0", NULL, &susp);   assert_eq("cond/zero",      out, "0");  free(out);
    flowd_destroy(rt);
}

static void
test_list_literal(void)
{
    const char *ir =
        "{\"ir_version\":\"1.0\",\"types\":[],\"tools\":[],"
        " \"flows\":[{\"name\":\"three\","
        "  \"params\":[{\"name\":\"_\",\"type\":\"int\",\"implicit\":false}],"
        "  \"output\":\"[int]\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"list_literal\",\"element_type\":\"int\","
        "   \"elements\":[{\"kind\":\"literal\",\"type\":\"int\",\"value\":1},"
        "                 {\"kind\":\"literal\",\"type\":\"int\",\"value\":2},"
        "                 {\"kind\":\"literal\",\"type\":\"int\",\"value\":3}]}}]}";

    flowd_runtime *rt = flowd_load_ir(ir);
    if (!rt) { fail("list-lit/load", flowd_last_error_json(NULL)); return; }
    char *susp = NULL;
    char *out = flowd_run(rt, "{\"_\":0}", NULL, &susp);
    assert_eq("list-lit/inline", out, "[1,2,3]");
    free(out);
    flowd_destroy(rt);
}


/* ---- tool registration + call dispatch ---- */

static char *
tool_double(const char *args_json, char **err_json, void *user_ctx)
{
    (void)err_json; (void)user_ctx;
    /* Parse {"x": <int>}, return 2*x as a JSON int. Quick-and-cheap
     * substring scan; production tools would parse JSON properly. */
    const char *p = strstr(args_json, "\"x\":");
    long n = strtol(p + 4, NULL, 10);
    char *out = malloc(32);
    snprintf(out, 32, "%ld", n * 2);
    return out;
}

static void
test_tool_call(void)
{
    const char *ir =
        "{\"ir_version\":\"1.0\",\"types\":[],"
        " \"tools\":[{\"name\":\"doubler\","
        "  \"input\":[{\"name\":\"x\",\"type\":\"int\"}],"
        "  \"output\":\"int\",\"effect\":{\"level\":\"pure\"}}],"
        " \"flows\":[{\"name\":\"f\","
        "  \"params\":[{\"name\":\"it\",\"type\":\"int\",\"implicit\":true}],"
        "  \"output\":\"int\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"call\",\"tool\":\"doubler\","
        "   \"args\":[{\"field\":\"x\",\"value\":"
        "    {\"kind\":\"path\",\"segments\":[\"it\"]}}]}}]}";

    flowd_runtime *rt = flowd_load_ir(ir);
    if (!rt) { fail("call/load", flowd_last_error_json(NULL)); return; }

    int rc = flowd_register_tool(rt, "doubler", FLOWD_EFFECT_PURE,
                                 "(int) -> int", tool_double,
                                 "v1", NULL);
    if (rc != 0) {
        char buf[64]; snprintf(buf, sizeof buf, "register rc=%d", rc);
        fail("call/register", buf);
        flowd_destroy(rt);
        return;
    }
    ok("call/register");

    char *susp = NULL;
    char *out = flowd_run(rt, "21", NULL, &susp);
    assert_eq("call/dispatch", out, "42");
    free(out);
    flowd_destroy(rt);
}

static void
test_register_R151_level_mismatch(void)
{
    const char *ir =
        "{\"ir_version\":\"1.0\",\"types\":[],"
        " \"tools\":[{\"name\":\"t\","
        "  \"input\":[{\"name\":\"x\",\"type\":\"int\"}],"
        "  \"output\":\"int\",\"effect\":{\"level\":\"pure\"}}],"
        " \"flows\":[{\"name\":\"f\","
        "  \"params\":[{\"name\":\"x\",\"type\":\"int\",\"implicit\":false}],"
        "  \"output\":\"int\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"path\",\"segments\":[\"x\"]}}]}";
    flowd_runtime *rt = flowd_load_ir(ir);
    if (!rt) { fail("R151/load", flowd_last_error_json(NULL)); return; }
    /* Register with the wrong level: IR says pure, host claims model. */
    int rc = flowd_register_tool(rt, "t", FLOWD_EFFECT_MODEL,
                                 "(int) -> int", tool_double, "v1", NULL);
    if (rc == 151) ok("R151/effect-level-mismatch");
    else {
        char buf[32]; snprintf(buf, sizeof buf, "expected 151, got %d", rc);
        fail("R151/expected-151", buf);
    }
    flowd_destroy(rt);
}


/* ---- subflow + match ---- */

static void
test_subflow(void)
{
    /* helper(it) -> it + 1
     * main(x) -> helper(x: x) + helper(x: x)   (two calls, sum)
     * Demonstrate fresh env per call. */
    const char *ir =
        "{\"ir_version\":\"1.0\",\"types\":[],\"tools\":[],"
        " \"flows\":["
        " {\"name\":\"helper\","
        "  \"params\":[{\"name\":\"it\",\"type\":\"int\",\"implicit\":true}],"
        "  \"output\":\"int\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"binop\",\"op\":\"+\","
        "   \"left\":{\"kind\":\"path\",\"segments\":[\"it\"]},"
        "   \"right\":{\"kind\":\"literal\",\"type\":\"int\",\"value\":1}}},"
        " {\"name\":\"main\","
        "  \"params\":[{\"name\":\"x\",\"type\":\"int\",\"implicit\":false}],"
        "  \"output\":\"int\","
        "  \"bindings\":[{\"name\":\"y\",\"expr\":"
        "   {\"kind\":\"subflow_call\",\"flow\":\"helper\","
        "    \"args\":[{\"field\":\"it\","
        "      \"value\":{\"kind\":\"path\",\"segments\":[\"x\"]}}]}}],"
        "  \"return\":{\"kind\":\"binop\",\"op\":\"+\","
        "   \"left\":{\"kind\":\"path\",\"segments\":[\"y\"]},"
        "   \"right\":{\"kind\":\"subflow_call\",\"flow\":\"helper\","
        "    \"args\":[{\"field\":\"it\","
        "      \"value\":{\"kind\":\"path\",\"segments\":[\"x\"]}}]}}}]}";

    flowd_runtime *rt = flowd_load_ir(ir);
    if (!rt) { fail("subflow/load", flowd_last_error_json(NULL)); return; }
    /* main flow is at index 1 — but flowd_run uses flow_idx=0
     * (helper). To test main we'd need flowd_run_named. For now
     * verify helper works on its own. */
    /* Skip — flowd_run runs flow 0 which is helper. */
    char *susp = NULL;
    char *out = flowd_run(rt, "10", NULL, &susp);
    assert_eq("subflow/helper-direct", out, "11");
    free(out);
    flowd_destroy(rt);
}

static void
test_match_variant(void)
{
    /* type R = | A {n: int} | B {s: string}
     * flow describe(it: R) -> string =
     *   match it { A x -> "a", B y -> y.s } */
    const char *ir =
        "{\"ir_version\":\"1.0\","
        " \"types\":[{\"name\":\"R\",\"kind\":\"sum\","
        "  \"variants\":[{\"name\":\"A\",\"fields\":[{\"name\":\"n\",\"type\":\"int\"}]},"
        "                {\"name\":\"B\",\"fields\":[{\"name\":\"s\",\"type\":\"string\"}]}]}],"
        " \"tools\":[],"
        " \"flows\":[{\"name\":\"describe\","
        "  \"params\":[{\"name\":\"it\",\"type\":\"R\",\"implicit\":true}],"
        "  \"output\":\"string\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"match\","
        "   \"scrutinee\":{\"kind\":\"path\",\"segments\":[\"it\"]},"
        "   \"scrutinee_type\":\"R\","
        "   \"arms\":[{\"pattern\":{\"kind\":\"variant\",\"variant\":\"A\",\"binder\":\"x\"},"
        "             \"body\":{\"kind\":\"literal\",\"type\":\"string\",\"value\":\"a\"}},"
        "             {\"pattern\":{\"kind\":\"variant\",\"variant\":\"B\",\"binder\":\"y\"},"
        "             \"body\":{\"kind\":\"path\",\"segments\":[\"y\",\"s\"]}}]}}]}";

    flowd_runtime *rt = flowd_load_ir(ir);
    if (!rt) { fail("match/load", flowd_last_error_json(NULL)); return; }
    char *susp = NULL;
    char *out = flowd_run(rt,
        "{\"variant\":\"A\",\"fields\":{\"n\":42}}", NULL, &susp);
    assert_eq("match/variant-A", out, "\"a\"");
    free(out);
    out = flowd_run(rt,
        "{\"variant\":\"B\",\"fields\":{\"s\":\"hi\"}}", NULL, &susp);
    assert_eq("match/variant-B-binder", out, "\"hi\"");
    free(out);
    flowd_destroy(rt);
}


/* ---- pipelines + aggregators ---- */

/* Tool that returns a fixed list of {cost: int, kind: string} */
static char *
tool_three_items(const char *args_json, char **err_json, void *ctx)
{
    (void)args_json; (void)err_json; (void)ctx;
    return strdup("[{\"cost\":10,\"kind\":\"a\"},"
                  "{\"cost\":30,\"kind\":\"b\"},"
                  "{\"cost\":20,\"kind\":\"a\"}]");
}

static void
test_pipeline_take(void)
{
    /* Pipeline: fetch_items() | take 2.   Result: first two of three. */
    const char *ir =
        "{\"ir_version\":\"1.0\","
        " \"types\":[{\"name\":\"Item\",\"kind\":\"record\","
        "  \"fields\":[{\"name\":\"cost\",\"type\":\"int\"},"
        "             {\"name\":\"kind\",\"type\":\"string\"}]}],"
        " \"tools\":[{\"name\":\"fetch_items\","
        "  \"input\":[{\"name\":\"x\",\"type\":\"int\"}],"
        "  \"output\":\"[Item]\",\"effect\":{\"level\":\"pure\"}}],"
        " \"flows\":[{\"name\":\"f\","
        "  \"params\":[{\"name\":\"x\",\"type\":\"int\",\"implicit\":false}],"
        "  \"output\":\"[Item]\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"pipeline\","
        "   \"source\":{\"kind\":\"call\",\"tool\":\"fetch_items\","
        "    \"args\":[{\"field\":\"x\","
        "     \"value\":{\"kind\":\"path\",\"segments\":[\"x\"]}}]},"
        "   \"stages\":[{\"kind\":\"take\",\"op\":\"take\",\"count\":2}]}}]}";

    flowd_runtime *rt = flowd_load_ir(ir);
    if (!rt) { fail("pipe-take/load", flowd_last_error_json(NULL)); return; }
    flowd_register_tool(rt, "fetch_items", FLOWD_EFFECT_PURE,
                        "(int) -> [Item]", tool_three_items, "v1", NULL);
    char *susp = NULL;
    char *out = flowd_run(rt, "{\"x\":0}", NULL, &susp);
    assert_eq("pipe-take/two",
              out,
              "[{\"cost\":10,\"kind\":\"a\"},"
              "{\"cost\":30,\"kind\":\"b\"}]");
    free(out);
    flowd_destroy(rt);
}

static void
test_pipeline_where_count(void)
{
    /* fetch_items() | where cost <= 20 | count → 2 */
    const char *ir =
        "{\"ir_version\":\"1.0\","
        " \"types\":[{\"name\":\"Item\",\"kind\":\"record\","
        "  \"fields\":[{\"name\":\"cost\",\"type\":\"int\"},"
        "             {\"name\":\"kind\",\"type\":\"string\"}]}],"
        " \"tools\":[{\"name\":\"fetch_items\","
        "  \"input\":[{\"name\":\"x\",\"type\":\"int\"}],"
        "  \"output\":\"[Item]\",\"effect\":{\"level\":\"pure\"}}],"
        " \"flows\":[{\"name\":\"f\","
        "  \"params\":[{\"name\":\"x\",\"type\":\"int\",\"implicit\":false}],"
        "  \"output\":\"int\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"pipeline\","
        "   \"source\":{\"kind\":\"call\",\"tool\":\"fetch_items\","
        "    \"args\":[{\"field\":\"x\","
        "     \"value\":{\"kind\":\"path\",\"segments\":[\"x\"]}}]},"
        "   \"stages\":[{\"kind\":\"filter\",\"op\":\"where\","
        "                \"predicate\":\"cost <= 20\"},"
        "              {\"kind\":\"terminal\",\"op\":\"count\"}]}}]}";

    flowd_runtime *rt = flowd_load_ir(ir);
    if (!rt) { fail("pipe-where/load", flowd_last_error_json(NULL)); return; }
    flowd_register_tool(rt, "fetch_items", FLOWD_EFFECT_PURE,
                        "(int) -> [Item]", tool_three_items, "v1", NULL);
    char *susp = NULL;
    char *out = flowd_run(rt, "{\"x\":0}", NULL, &susp);
    assert_eq("pipe-where/count-after-filter", out, "2");
    free(out);
    flowd_destroy(rt);
}

static void
test_pipeline_sum(void)
{
    /* fetch_items() | sum cost → 60 */
    const char *ir =
        "{\"ir_version\":\"1.0\","
        " \"types\":[{\"name\":\"Item\",\"kind\":\"record\","
        "  \"fields\":[{\"name\":\"cost\",\"type\":\"int\"},"
        "             {\"name\":\"kind\",\"type\":\"string\"}]}],"
        " \"tools\":[{\"name\":\"fetch_items\","
        "  \"input\":[{\"name\":\"x\",\"type\":\"int\"}],"
        "  \"output\":\"[Item]\",\"effect\":{\"level\":\"pure\"}}],"
        " \"flows\":[{\"name\":\"f\","
        "  \"params\":[{\"name\":\"x\",\"type\":\"int\",\"implicit\":false}],"
        "  \"output\":\"int\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"pipeline\","
        "   \"source\":{\"kind\":\"call\",\"tool\":\"fetch_items\","
        "    \"args\":[{\"field\":\"x\","
        "     \"value\":{\"kind\":\"path\",\"segments\":[\"x\"]}}]},"
        "   \"stages\":[{\"kind\":\"terminal\",\"op\":\"sum\",\"field\":\"cost\"}]}}]}";

    flowd_runtime *rt = flowd_load_ir(ir);
    if (!rt) { fail("pipe-sum/load", flowd_last_error_json(NULL)); return; }
    flowd_register_tool(rt, "fetch_items", FLOWD_EFFECT_PURE,
                        "(int) -> [Item]", tool_three_items, "v1", NULL);
    char *susp = NULL;
    char *out = flowd_run(rt, "{\"x\":0}", NULL, &susp);
    assert_eq("pipe-sum/total", out, "60");
    free(out);
    flowd_destroy(rt);
}

static void
test_pipeline_dedupe(void)
{
    /* fetch_items() | dedupe by row.kind | count → 2 (kinds "a","b") */
    const char *ir =
        "{\"ir_version\":\"1.0\","
        " \"types\":[{\"name\":\"Item\",\"kind\":\"record\","
        "  \"fields\":[{\"name\":\"cost\",\"type\":\"int\"},"
        "             {\"name\":\"kind\",\"type\":\"string\"}]}],"
        " \"tools\":[{\"name\":\"fetch_items\","
        "  \"input\":[{\"name\":\"x\",\"type\":\"int\"}],"
        "  \"output\":\"[Item]\",\"effect\":{\"level\":\"pure\"}}],"
        " \"flows\":[{\"name\":\"f\","
        "  \"params\":[{\"name\":\"x\",\"type\":\"int\",\"implicit\":false}],"
        "  \"output\":\"int\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"pipeline\","
        "   \"source\":{\"kind\":\"call\",\"tool\":\"fetch_items\","
        "    \"args\":[{\"field\":\"x\","
        "     \"value\":{\"kind\":\"path\",\"segments\":[\"x\"]}}]},"
        "   \"stages\":[{\"kind\":\"dedupe\",\"op\":\"dedupe_by\","
        "                \"key\":{\"kind\":\"path\",\"segments\":[\"row\",\"kind\"]}},"
        "              {\"kind\":\"terminal\",\"op\":\"count\"}]}}]}";

    flowd_runtime *rt = flowd_load_ir(ir);
    if (!rt) { fail("pipe-dedupe/load", flowd_last_error_json(NULL)); return; }
    flowd_register_tool(rt, "fetch_items", FLOWD_EFFECT_PURE,
                        "(int) -> [Item]", tool_three_items, "v1", NULL);
    char *susp = NULL;
    char *out = flowd_run(rt, "{\"x\":0}", NULL, &susp);
    assert_eq("pipe-dedupe/by-kind", out, "2");
    free(out);
    flowd_destroy(rt);
}


/* ---- aggregator on a runtime-empty list ---- */

static void
test_top_empty_R110(void)
{
    /* flow f(xs: [int]) -> int = xs | top
     * The source is a path to the list param, so an input of {"xs":[]}
     * drives the `top` terminal over a runtime-empty list → R110. */
    const char *ir =
        "{\"ir_version\":\"1.0\",\"types\":[],\"tools\":[],"
        " \"flows\":[{\"name\":\"f\","
        "  \"params\":[{\"name\":\"xs\",\"type\":\"[int]\",\"implicit\":false}],"
        "  \"output\":\"int\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"pipeline\","
        "   \"source\":{\"kind\":\"path\",\"segments\":[\"xs\"]},"
        "   \"stages\":[{\"kind\":\"terminal\",\"op\":\"top\"}]}}]}";

    flowd_runtime *rt = flowd_load_ir(ir);
    if (!rt) { fail("top-empty/load", flowd_last_error_json(NULL)); return; }
    char *susp = NULL;
    /* Non-empty input still flows through `top` (sanity: returns head). */
    char *out = flowd_run(rt, "{\"xs\":[5,9]}", NULL, &susp);
    assert_eq("top-empty/nonempty-head", out, "5");
    free(out);
    /* Empty input → R110. */
    out = flowd_run(rt, "{\"xs\":[]}", NULL, &susp);
    if (out != NULL) {
        fail("top-empty/reports-R110",
             "top of empty list should fail, got non-NULL");
        free(out);
    } else {
        char *err = flowd_last_error_json(NULL);
        if (err && strstr(err, "R110")) {
            ok("top-empty/reports-R110");
        } else {
            fail("top-empty/reports-R110", err ? err : "(no error message)");
        }
        free(err);
    }
    flowd_destroy(rt);
}

static void
test_max_empty_R141(void)
{
    /* flow f(xs: [Item]) -> int = xs | max cost
     * With input {"xs":[]} the `max` terminal sees n==0 and, unlike
     * sum (which yields 0), cannot determine a value → R141. */
    const char *ir =
        "{\"ir_version\":\"1.0\","
        " \"types\":[{\"name\":\"Item\",\"kind\":\"record\","
        "  \"fields\":[{\"name\":\"cost\",\"type\":\"int\"},"
        "             {\"name\":\"kind\",\"type\":\"string\"}]}],"
        " \"tools\":[],"
        " \"flows\":[{\"name\":\"f\","
        "  \"params\":[{\"name\":\"xs\",\"type\":\"[Item]\",\"implicit\":false}],"
        "  \"output\":\"int\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"pipeline\","
        "   \"source\":{\"kind\":\"path\",\"segments\":[\"xs\"]},"
        "   \"stages\":[{\"kind\":\"terminal\",\"op\":\"max\",\"field\":\"cost\"}]}}]}";

    flowd_runtime *rt = flowd_load_ir(ir);
    if (!rt) { fail("max-empty/load", flowd_last_error_json(NULL)); return; }
    char *susp = NULL;
    /* Non-empty input still flows through `max` (sanity: picks larger). */
    char *out = flowd_run(rt,
        "{\"xs\":[{\"cost\":10,\"kind\":\"a\"},{\"cost\":30,\"kind\":\"b\"}]}",
        NULL, &susp);
    assert_eq("max-empty/nonempty-max", out, "30");
    free(out);
    /* Empty input → R141. */
    out = flowd_run(rt, "{\"xs\":[]}", NULL, &susp);
    if (out != NULL) {
        fail("max-empty/reports-R141",
             "max of empty list should fail, got non-NULL");
        free(out);
    } else {
        char *err = flowd_last_error_json(NULL);
        if (err && strstr(err, "R141")) {
            ok("max-empty/reports-R141");
        } else {
            fail("max-empty/reports-R141", err ? err : "(no error message)");
        }
        free(err);
    }
    flowd_destroy(rt);
}


/* ---- try/else discriminates by R-code ---- */

static char *
tool_always_fails(const char *args, char **err, void *ctx)
{
    (void)args; (void)ctx;
    *err = strdup("{\"message\":\"simulated failure\"}");
    return NULL;
}

static void
test_try_else_catches_R101(void)
{
    /* flow t(x: int) -> int { try fails(x: x) else 99 }
     * fails returns NULL → R101 → recoverable → else fires → 99. */
    const char *ir =
        "{\"ir_version\":\"1.0\",\"types\":[],"
        " \"tools\":[{\"name\":\"fails\","
        "  \"input\":[{\"name\":\"x\",\"type\":\"int\"}],"
        "  \"output\":\"int\",\"effect\":{\"level\":\"pure\"}}],"
        " \"flows\":[{\"name\":\"t\","
        "  \"params\":[{\"name\":\"x\",\"type\":\"int\",\"implicit\":false}],"
        "  \"output\":\"int\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"try_else\","
        "   \"try\":{\"kind\":\"call\",\"tool\":\"fails\","
        "    \"args\":[{\"field\":\"x\",\"value\":"
        "     {\"kind\":\"path\",\"segments\":[\"x\"]}}]},"
        "   \"else\":{\"kind\":\"literal\",\"type\":\"int\",\"value\":99}}}]}";

    flowd_runtime *rt = flowd_load_ir(ir);
    flowd_register_tool(rt, "fails", FLOWD_EFFECT_PURE,
                        "(int) -> int", tool_always_fails, "v1", NULL);
    char *susp = NULL;
    char *out = flowd_run(rt, "{\"x\":1}", NULL, &susp);
    assert_eq("try_else/catches-R101", out, "99");
    free(out);
    flowd_destroy(rt);
}

static void
test_try_else_propagates_R140(void)
{
    /* flow t(x: int) -> int { try (10 / x) else 99 }
     * x=0 → R140 division by zero → unrecoverable → propagates. */
    const char *ir =
        "{\"ir_version\":\"1.0\",\"types\":[],\"tools\":[],"
        " \"flows\":[{\"name\":\"t\","
        "  \"params\":[{\"name\":\"x\",\"type\":\"int\",\"implicit\":false}],"
        "  \"output\":\"int\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"try_else\","
        "   \"try\":{\"kind\":\"binop\",\"op\":\"/\","
        "    \"left\":{\"kind\":\"literal\",\"type\":\"int\",\"value\":10},"
        "    \"right\":{\"kind\":\"path\",\"segments\":[\"x\"]}},"
        "   \"else\":{\"kind\":\"literal\",\"type\":\"int\",\"value\":99}}}]}";

    flowd_runtime *rt = flowd_load_ir(ir);
    char *susp = NULL;
    /* x=2: try succeeds, returns 5 */
    char *out = flowd_run(rt, "{\"x\":2}", NULL, &susp);
    assert_eq("try_else/passthrough-on-success", out, "5");
    free(out);
    /* x=0: R140 division by zero → should NOT be caught */
    out = flowd_run(rt, "{\"x\":0}", NULL, &susp);
    if (out != NULL) {
        fail("try_else/propagates-R140",
             "R140 should propagate, but try/else swallowed it");
        free(out);
    } else {
        char *err = flowd_last_error_json(NULL);
        if (err && strstr(err, "R140")) {
            ok("try_else/propagates-R140");
        } else {
            fail("try_else/propagates-R140",
                 err ? err : "(no error message)");
        }
        free(err);
    }
    flowd_destroy(rt);
}


/* ---- Malformed-input robustness ----
 *
 * flowd_load_ir is a public entry point fed untrusted IR JSON; it must
 * reject malformed input gracefully (return NULL with a readable R150),
 * never crash. Regression for a heap-use-after-free in the loader's
 * error path: the recorded diagnostic borrowed an arena-owned source
 * name that arena_destroy freed before flowd_last_error_json read it.
 * Run under ASan (`make sanitize`) this is the cheap gate for that bug.
 */
static void
test_load_errors_no_crash(void)
{
    static const char *bad[] = {
        "", "x", "{", "[]", "42", "not json",
        "{\"ir_version\":\"9.9\"}",          /* unsupported version */
        "{\"ir_version\":\"1.0\"}",          /* missing types/tools/flows */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        flowd_runtime *rt = flowd_load_ir(bad[i]);
        if (rt != NULL) {
            fail("load-error/rejects-malformed", bad[i]);
            flowd_destroy(rt);
            continue;
        }
        /* Reading the error blob must not touch freed memory. */
        char *err = flowd_last_error_json(NULL);
        if (err && strstr(err, "R150") != NULL) {
            ok("load-error/reports-R150");
        } else {
            fail("load-error/reports-R150", err ? err : "no error json");
        }
        free(err);   /* flowd_last_error_json read-and-clears: the caller owns the string */
    }
}


/* ---- R155: input type mismatch (value-decode catch-all) ---- */

static void
test_input_type_mismatch_R155(void)
{
    /* flow f(x: int) -> int = x. Feeding a string where an int is
     * declared makes value_from_json fail with R155 ("expected int") —
     * the structural/decode catch-all (exec.c value decoding). */
    const char *ir =
        "{\"ir_version\":\"1.0\",\"types\":[],\"tools\":[],"
        " \"flows\":[{\"name\":\"f\","
        "  \"params\":[{\"name\":\"x\",\"type\":\"int\",\"implicit\":false}],"
        "  \"output\":\"int\",\"bindings\":[],"
        "  \"return\":{\"kind\":\"path\",\"segments\":[\"x\"]}}]}";
    flowd_runtime *rt = flowd_load_ir(ir);
    if (!rt) { fail("type-mismatch/load", flowd_last_error_json(NULL)); return; }
    char *susp = NULL;
    char *out  = flowd_run(rt, "{\"x\":\"not an int\"}", NULL, &susp);
    if (out != NULL) {
        fail("type-mismatch/R155", "expected NULL on a wrong-typed input");
        free(out);
    } else {
        char *err = flowd_last_error_json(NULL);
        if (err && strstr(err, "R155")) ok("type-mismatch/R155");
        else fail("type-mismatch/R155", err ? err : "(no error)");
        free(err);
    }
    flowd_destroy(rt);
}


/* ---- stub host: type-directed stubs run a flow with no host code ---- */

/* Tools output a record (with a list field), a sum, and an int model tool; the
 * flow picks from its list input and calls the model tool. Installing stubs
 * renders a default for each output type and serves both the model-id and the
 * `pick` gateway paths, so the flow runs end to end offline. */
static const char *STUB_HOST_IR =
"{\"ir_version\":\"1.0\","
"\"types\":["
 "{\"name\":\"Rec\",\"kind\":\"record\",\"fields\":["
   "{\"name\":\"a\",\"type\":\"int\"},{\"name\":\"tags\",\"type\":\"[string]\"}]},"
 "{\"name\":\"S\",\"kind\":\"sum\",\"variants\":["
   "{\"name\":\"V\",\"fields\":[{\"name\":\"x\",\"type\":\"int\"}]}]}],"
"\"tools\":["
 "{\"name\":\"fetch\",\"input\":[{\"name\":\"n\",\"type\":\"int\"}],"
  "\"output\":\"Rec\",\"effect\":{\"level\":\"deterministic\"}},"
 "{\"name\":\"classify\",\"input\":[{\"name\":\"n\",\"type\":\"int\"}],"
  "\"output\":\"S\",\"effect\":{\"level\":\"deterministic\"}},"
 "{\"name\":\"ask\",\"input\":[{\"name\":\"q\",\"type\":\"int\"}],"
  "\"output\":\"int\",\"effect\":{\"level\":\"model\",\"model\":\"m1\"}}],"
"\"flows\":[{\"name\":\"demo\","
 "\"params\":[{\"name\":\"it\",\"type\":\"[int]\",\"implicit\":true}],"
 "\"output\":\"int\",\"bindings\":["
  "{\"name\":\"best\",\"expr\":{\"kind\":\"pipeline\","
    "\"source\":{\"kind\":\"path\",\"segments\":[\"it\"]},"
    "\"stages\":[{\"kind\":\"terminal\",\"op\":\"pick\",\"model\":\"primary\"}]}},"
  "{\"name\":\"a\",\"expr\":{\"kind\":\"call\",\"tool\":\"ask\","
    "\"args\":[{\"field\":\"q\",\"value\":{\"kind\":\"path\",\"segments\":[\"best\"]}}]}}],"
 "\"return\":{\"kind\":\"path\",\"segments\":[\"a\"]}}]}";

static void
test_stub_host(void)
{
    flowd_runtime *rt = flowd_load_ir(STUB_HOST_IR);
    if (rt == NULL) { fail("stub_host/load", flowd_last_error_json(NULL)); return; }
    flowd_install_stub_host(rt);

    const flow_t *flow = flowd_flow_at(rt, 0);
    char *in = flowd_default_input_json(rt, flow);   /* synthesizes a [int] */
    char *susp = NULL;
    char *out  = flowd_run_named(rt, "demo", in, NULL, &susp);
    if (out != NULL) { ok("stub_host/runs-offline"); free(out); }
    else             { fail("stub_host/runs-offline", flowd_last_error_json(rt)); }
    free(in);
    flowd_destroy(rt);
}

int
main(void)
{
    /* literal-and-path subset */
    test_identity_string();
    test_record_field_pluck();
    test_literal_only();
    test_nyi_call_reports();

    /* construct + binop + conditional */
    test_record_construct();
    test_binops_int();
    test_binops_cmp();
    test_short_circuit_and();
    test_conditional();
    test_list_literal();

    /* tool registration + call dispatch */
    test_tool_call();
    test_register_R151_level_mismatch();

    /* subflow + match */
    test_subflow();
    test_match_variant();

    /* pipelines + aggregators */
    test_pipeline_take();
    test_pipeline_where_count();
    test_pipeline_sum();
    test_pipeline_dedupe();

    /* aggregator over a runtime-empty list */
    test_top_empty_R110();
    test_max_empty_R141();

    /* try/else */
    test_try_else_catches_R101();
    test_try_else_propagates_R140();

    /* malformed-input robustness (UAF regression) */
    test_load_errors_no_crash();

    /* value-decode catch-all on a wrong-typed input */
    test_input_type_mismatch_R155();

    /* type-directed stub host (the basis of `flowd run` and the playground) */
    test_stub_host();

    printf("\nPASS %d  FAIL %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
