/* web/flowc_wasm.c — the flowc half of the playground, exported to JS.
 *
 * Compiles Flow source to IR JSON. Links libflowc only; the flowd half is a
 * separate WASM module. They don't share an address space — the IR string is
 * the seam, exactly as the native flowc -> flowd CLI chain is. Keeping them
 * apart also avoids a link clash: flowc and flowd each define their own
 * arena_/diag_ symbols.
 */
#include "api.h"
#include "util.h"   /* Diagnostic, diag_count, diag_at, diag_set_max_errors */

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_err[2048];

/* Last compile's diagnostics (empty string if the last compile succeeded). */
const char *flow_compile_error(void) { return g_err; }

static const char *
severity_word(DiagSeverity s)
{
    return s == DIAG_ERROR ? "error" : s == DIAG_WARNING ? "warning" : "note";
}

/* Compile `src` to IR. Returns the malloc'd IR string (JS frees it) on
 * success, or NULL with flow_compile_error() set to the rendered
 * diagnostics. */
char *
flow_compile(const char *src)
{
    /* The IR emitter formats floats with %g, which honors LC_NUMERIC; pin it
     * to "C" so the decimal separator is '.' and the IR stays valid JSON
     * regardless of the host locale (api.h spells out this requirement). */
    setlocale(LC_NUMERIC, "C");
    g_err[0] = '\0';

    flowc_context *ctx = flowc_context_create();
    diag_set_max_errors(flowc_diag_stream(ctx), 0);   /* record every error */
    const char *ir = flowc_compile_source(ctx, src, strlen(src),
                                          "playground.flow");

    char *out = NULL;
    if (ir != NULL && flowc_error_count(ctx) == 0) {
        out = strdup(ir);
        if (out == NULL) snprintf(g_err, sizeof g_err, "out of memory");
    } else {
        DiagStream *ds = flowc_diag_stream(ctx);
        size_t off = 0, n = diag_count(ds);
        for (size_t i = 0; i < n && off + 256 < sizeof g_err; i++) {
            const Diagnostic *d = diag_at(ds, i);
            const char *sev = severity_word(d->severity);
            const char *id  = d->id ? d->id : "";
            const char *sp  = d->id ? " " : "";
            const char *msg = d->message ? d->message : "";
            /* line == 0 means "no location" (util.h); emit it without the
             * line:col prefix, as emit_text does — the page then shows it as a
             * plain message rather than trying (and failing) to frame it. */
            int w = d->loc.line > 0
                ? snprintf(g_err + off, sizeof g_err - off, "%d:%d: %s%s%s: %s\n",
                           d->loc.line, d->loc.column, sev, sp, id, msg)
                : snprintf(g_err + off, sizeof g_err - off, "%s%s%s: %s\n",
                           sev, sp, id, msg);
            if (w > 0) off += (size_t)w;
        }
        if (off == 0) snprintf(g_err, sizeof g_err, "compilation failed");
    }
    flowc_context_destroy(ctx);
    return out;
}
