/* tests/api/test_budget_cancel.c
 *
 * Covers two capabilities:
 *   1. flowd_set_budget: a token limit is enforced — a model call that
 *      would push cumulative usage past the limit fails with R161 and
 *      the run returns NULL.
 *   2. flowd_cancel: cooperative cancellation requested from a tool
 *      callback aborts the next binding; the run returns NULL with R160.
 *
 * Exit 0 on success, non-zero on any failed check (run.sh checks the
 * exit code).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flowd.h"

static int g_fail = 0;
static void ok(const char *m)  { printf("  ok   %s\n", m); }
static void bad(const char *m) { printf("  FAIL %s\n", m); g_fail = 1; }

/* Model adapter reporting 10 in + 10 out tokens per call. */
static const char *p_name(void) { return "stub"; }
static int p_supp(const char *m, void *c) { (void)m; (void)c; return 1; }
static void p_inv(const char *model, const char *req,
                  flowd_adapter_response_t *r, void *ctx) {
    (void)model; (void)req; (void)ctx;
    r->response_json = strdup("\"ok\"");
    r->err_msg = NULL;
    r->tokens_in = 10; r->tokens_out = 10; r->cost_cents = 0.0;
}
static const flowd_provider_adapter_t adapter = {
    p_name, p_supp, NULL, NULL, p_inv
};

static const char *BUDGET_IR =
 "{\"ir_version\":\"1.0\",\"types\":[],"
 "\"tools\":[{\"name\":\"ask1\",\"input\":[{\"name\":\"q\",\"type\":\"string\"}],"
   "\"output\":\"string\",\"effect\":{\"level\":\"model\",\"model\":\"m1\"}},"
  "{\"name\":\"ask2\",\"input\":[{\"name\":\"q\",\"type\":\"string\"}],"
   "\"output\":\"string\",\"effect\":{\"level\":\"model\",\"model\":\"m2\"}}],"
 "\"flows\":[{\"name\":\"chat\",\"params\":[{\"name\":\"it\",\"type\":\"string\",\"implicit\":true}],"
   "\"output\":\"string\",\"bindings\":["
     "{\"name\":\"a\",\"expr\":{\"kind\":\"call\",\"tool\":\"ask1\",\"args\":[{\"field\":\"q\",\"value\":{\"kind\":\"path\",\"segments\":[\"it\"]}}]}},"
     "{\"name\":\"b\",\"expr\":{\"kind\":\"call\",\"tool\":\"ask2\",\"args\":[{\"field\":\"q\",\"value\":{\"kind\":\"path\",\"segments\":[\"it\"]}}]}}],"
   "\"return\":{\"kind\":\"path\",\"segments\":[\"b\"]}}]}";

static char *t_cancel(const char *a, char **e, void *u) {
    (void)a; (void)e;
    flowd_cancel((flowd_runtime *)u);   /* cancel mid-run */
    return strdup("true");
}
static char *t_plain(const char *a, char **e, void *u) {
    (void)a; (void)e; (void)u; return strdup("true");
}
static const char *CANCEL_IR =
 "{\"ir_version\":\"1.0\",\"types\":[],"
 "\"tools\":[{\"name\":\"step1\",\"input\":[],\"output\":\"bool\",\"effect\":{\"level\":\"deterministic\"}},"
  "{\"name\":\"step2\",\"input\":[],\"output\":\"bool\",\"effect\":{\"level\":\"deterministic\"}}],"
 "\"flows\":[{\"name\":\"proc\",\"params\":[{\"name\":\"it\",\"type\":\"int\",\"implicit\":true}],"
   "\"output\":\"bool\",\"bindings\":["
     "{\"name\":\"a\",\"expr\":{\"kind\":\"call\",\"tool\":\"step1\",\"args\":[]}},"
     "{\"name\":\"b\",\"expr\":{\"kind\":\"call\",\"tool\":\"step2\",\"args\":[]}}],"
   "\"return\":{\"kind\":\"path\",\"segments\":[\"b\"]}}]}";

int main(void) {
    /* ---- budget ---- */
    flowd_runtime *rt = flowd_load_ir(BUDGET_IR);
    if (!rt) { bad("budget/load"); return 1; }
    flowd_register_provider(rt, &adapter);
    flowd_set_budget(rt, 15, 0.0, 0);   /* call1=20 tokens -> call2 exceeds */
    char *susp = NULL;
    char *out = flowd_run(rt, "\"hi\"", "/tmp/flowd-test-budget", &susp);
    if (out != NULL) { bad("budget/should-have-failed"); free(out); }
    else {
        char *e = flowd_last_error_json(rt);
        /* The budget overrun must surface under its own code (R161),
         * not folded into a generic tool failure (R101). */
        if (e && strstr(e, "R161") && !strstr(e, "R101"))
            ok("budget/R161-on-exceed");
        else bad("budget/error-not-R161");
        free(e);
    }
    flowd_destroy(rt);

    /* ---- cancel ---- */
    rt = flowd_load_ir(CANCEL_IR);
    if (!rt) { bad("cancel/load"); return 1; }
    flowd_register_tool(rt, "step1", FLOWD_EFFECT_DETERMINISTIC,
                        "()->bool", t_cancel, "v1", rt);
    flowd_register_tool(rt, "step2", FLOWD_EFFECT_DETERMINISTIC,
                        "()->bool", t_plain, "v1", NULL);
    susp = NULL;
    out = flowd_run(rt, "0", "/tmp/flowd-test-cancel", &susp);
    if (out != NULL) { bad("cancel/should-have-aborted"); free(out); }
    else {
        char *e = flowd_last_error_json(rt);
        if (e && strstr(e, "R160")) ok("cancel/R160-on-cancel");
        else bad("cancel/error-not-R160");
        free(e);
    }
    flowd_destroy(rt);

    if (g_fail) { printf("test_budget_cancel: FAIL\n"); return 1; }
    printf("test_budget_cancel: PASS\n");
    return 0;
}
