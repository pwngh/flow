/* src/api.h
 *
 * Public C API for `libflowc`. A single header shipped alongside the
 * static archive (`libflowc.a`) for hosts that want to drive the
 * compiler in-process — language bindings, the LSP server, watch-mode
 * tooling, test harnesses, anything that would otherwise spawn the
 * `flowc` binary and round-trip through disk.
 *
 * Shape: an opaque context owning per-invocation state, plus entry
 * points that take source-as-a-buffer and return results-as-a-buffer.
 * Same lex → parse → resolve → check → emit pipeline as the CLI; the
 * only difference is what the FILE pointers at the boundaries are
 * backed by (POSIX 2008's fmemopen / open_memstream rather than disk).
 *
 * Lifetime model
 * --------------
 *
 *   - Pointers returned by flowc_compile_* and flowc_dump_* are
 *     valid until the next call into the same context or until
 *     flowc_context_destroy / flowc_context_reset. Callers wanting
 *     persistence copy out.
 *
 *   - Diagnostics are owned by the context; their lifetime matches
 *     the context's. flowc_context_reset clears them.
 *
 *   - Source-name pointers passed in are borrowed; the context copies
 *     them into its own arena before keeping them.
 *
 * Failure model
 * -------------
 *
 *   flowc_compile_source / flowc_compile_file / flowc_dump_*  return
 *   NULL on any error. flowc_error_count(ctx) is the authoritative
 *   success signal. flowc_check returns the error count directly.
 *
 *   The success signal is cumulative over the context's lifetime,
 *   not per-call: every entry point gates on the context's total
 *   recorded error count, so once any call has recorded an error,
 *   all subsequent compile/dump/check calls on that context fail
 *   (NULL / non-zero) even for valid source. After a failed call,
 *   call flowc_context_reset() — or diag_clear() on
 *   flowc_diag_stream(ctx) to keep the rest of the context's state —
 *   before compiling again.
 *
 * Locale
 * ------
 *
 *   IR emission formats floating-point literals with a shortest
 *   round-trip printf (flowc_format_double in src/util.c) and assumes
 *   LC_NUMERIC is "C" (the CLI sets this in main()). Under a
 *   comma-decimal locale such as de_DE it would emit ',' as the
 *   decimal separator and the IR would no longer be valid JSON.
 *   Library callers must ensure setlocale(LC_NUMERIC, "C") is in
 *   effect before calling flowc_compile_* / flowc_dump_*.
 */

#ifndef FLOWC_API_H
#define FLOWC_API_H

#include <stddef.h>

#include "util.h"   /* Diagnostic, DiagStream, DiagSeverity, SrcLoc */

#ifdef __cplusplus
extern "C" {
#endif


/* ====================================================================
 * Context
 *
 * Owns a per-invocation arena, a DiagStream (record-only by default),
 * and a one-slot output buffer (heap-allocated, replaced on each
 * compile/dump call).
 *
 * Thread safety: none. The pipeline behind every entry point runs
 * through the non-reentrant flex lexer and bison parser (src/lex.l,
 * src/parse.y), whose working state is file-scope static and
 * therefore process-global. At most one compile/dump/check call may
 * be in flight per process at a time, no matter how many contexts
 * exist; callers on multiple threads must serialize every flowc_*
 * call behind a single lock. (Making the lexer/parser reentrant —
 * %define api.pure full — would lift this; see src/parse.y.)
 * ==================================================================== */

typedef struct flowc_context flowc_context;

/* Public projection of the internal Diagnostic struct. The layout
 * is stable within the v0.1.x line (FLOWC_VERSION in src/main.c);
 * future minor versions may extend it additively (new fields
 * appended at the end). */
typedef Diagnostic flowc_diagnostic;

/* Allocate a fresh context. The context starts in record-only diag
 * mode (no stderr sink) with max_errors = 1 and -Werror off. The CLI
 * driver attaches stderr explicitly via flowc_diag_stream() +
 * diag_attach_stderr(); library callers usually leave it record-only
 * and inspect diagnostics through flowc_diag_stream().
 *
 * max_errors = 1 means only the first error is recorded; raise it via
 * diag_set_max_errors() on flowc_diag_stream() to capture more. */
flowc_context *flowc_context_create(void);

/* Free the context, its arena, its DiagStream, and its output buffer.
 * Every pointer previously returned through this context becomes
 * invalid. Passing NULL is a no-op. */
void flowc_context_destroy(flowc_context *ctx);

/* Free and recreate the context's arena and DiagStream. The context
 * handle stays valid; the output-buffer pointer is cleared (and any
 * pointer previously returned through it becomes invalid). The
 * stream's stderr attachment, format, max_errors, and -Werror state
 * are reset to creation defaults — re-attach explicitly if needed.
 *
 * This is the property long-running consumers (watch-mode bindings,
 * this archive's own test harness) rely on to avoid unbounded arena
 * growth across hundreds of compiles. */
void flowc_context_reset(flowc_context *ctx);


/* ====================================================================
 * Compile / dump entry points
 *
 * Each entry point runs the pipeline on `src` (length `len`,
 * displayed in diagnostics under `name`) and writes the captured
 * output into the context's one-slot buffer. Returns a pointer to
 * a NUL-terminated string owned by the context on success, or NULL
 * if any error diagnostic was emitted. flowc_error_count(ctx) is
 * authoritative for "did this succeed?".
 *
 * `len` may exclude a trailing NUL; the buffer is read up to `len`
 * bytes regardless. `name` is borrowed (caller-owned lifetime) and
 * copied into the context arena before use; NULL is accepted and
 * substituted with "<source>".
 *
 * The returned pointer is invalidated by:
 *   - the next call into the same context (any compile/dump/check),
 *   - flowc_context_reset(ctx),
 *   - flowc_context_destroy(ctx).
 * ==================================================================== */

const char *flowc_compile_source(flowc_context *ctx,
                                 const char *src, size_t len,
                                 const char *name);

/* Slurp the named file into memory and delegate to
 * flowc_compile_source. On an I/O failure — open, seek, tell, or a
 * read stream error (the steps flowc_read_file distinguishes; there
 * is no stat/fstat) — emits E001 with no source location and returns
 * NULL. */
const char *flowc_compile_file(flowc_context *ctx, const char *path);

const char *flowc_dump_tokens   (flowc_context *ctx,
                                 const char *src, size_t len,
                                 const char *name);
const char *flowc_dump_ast      (flowc_context *ctx,
                                 const char *src, size_t len,
                                 const char *name);
const char *flowc_dump_resolved (flowc_context *ctx,
                                 const char *src, size_t len,
                                 const char *name);
const char *flowc_dump_checked  (flowc_context *ctx,
                                 const char *src, size_t len,
                                 const char *name);

/* Run lex → parse → resolve → check, return the error count, and
 * emit no IR. Mirrors `flowc --check-only`. The output buffer is
 * cleared (any previous return becomes invalid). */
int flowc_check(flowc_context *ctx,
                const char *src, size_t len,
                const char *name);


/* ====================================================================
 * Diagnostic queries
 *
 * Diagnostics accumulate in the context across compile/dump/check
 * calls — they are not cleared between calls. flowc_context_reset
 * clears them. Recorded Diagnostic structures live until the next
 * reset/destroy.
 *
 * Because the entry points gate on the accumulated error count (see
 * "Failure model" at the top of this header), a recorded error fails
 * every subsequent call on the context until flowc_context_reset()
 * or diag_clear() on flowc_diag_stream().
 * ==================================================================== */

int                     flowc_error_count(const flowc_context *ctx);

/* Direct handle to the context's DiagStream. Useful for advanced
 * cases that flowc_error_count above doesn't cover — attaching
 * a stderr sink alongside record-only mode (call diag_attach_stderr
 * on the returned pointer for a hybrid setup), setting -Werror /
 * -fmax-errors / -fdiagnostics-format from the binding side, or
 * clearing recorded diagnostics between compiles without dropping
 * the rest of the context's state (diag_clear).
 *
 * Invalidated by flowc_context_reset / flowc_context_destroy. */
DiagStream *flowc_diag_stream(flowc_context *ctx);

#ifdef __cplusplus
}
#endif

#endif /* FLOWC_API_H */
