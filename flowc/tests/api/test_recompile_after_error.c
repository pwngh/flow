/* tests/api/test_recompile_after_error.c
 *
 * A compile that fails on a syntax error must not break the NEXT compile in
 * the same process. flowc's lexer keeps process-global state (the flex buffer);
 * a parse that stops early on an error never drains the lexer to EOF, so
 * lex_open has to reset it. The one-shot CLI never hits this — fresh process
 * per run — but libflowc consumers compile repeatedly in one process (the
 * playground, an LSP, watch mode), where a transient typo would otherwise
 * poison every later compile. Regression test for that reset.
 */
#include <stdio.h>
#include <string.h>

#include "api.h"
#include "util.h"

static const char *VALID  = "flow f(int) -> int {\n  it\n}\n";
static const char *BROKEN = "flow f(int) -> int {{{\n";   /* syntax error */

static const char *compile(flowc_context *ctx, const char *s)
{
    return flowc_compile_source(ctx, s, strlen(s), "t");
}

int main(void)
{
    flowc_context *ctx = flowc_context_create();
    int rc = 0;

    if (compile(ctx, VALID) == NULL) {
        fprintf(stderr, "FAIL: initial valid compile failed\n");
        rc = 1;
    }
    flowc_context_reset(ctx);

    if (compile(ctx, BROKEN) != NULL || flowc_error_count(ctx) == 0) {
        fprintf(stderr, "FAIL: broken program did not error\n");
        rc = 1;
    }
    flowc_context_reset(ctx);

    /* The crux: the same valid program must STILL compile after the error. */
    if (compile(ctx, VALID) == NULL) {
        fprintf(stderr, "FAIL: valid compile after an error failed "
                        "(%d errors) — lexer state leaked\n",
                flowc_error_count(ctx));
        rc = 1;
    }

    flowc_context_destroy(ctx);
    if (rc == 0) printf("test_recompile_after_error: PASS\n");
    return rc;
}
