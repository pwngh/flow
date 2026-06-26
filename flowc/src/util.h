/* src/util.h
 *
 * Foundation utilities for flowc: source locations, the arena
 * allocator, the diagnostic stream, and the three fatal-exit paths.
 *
 * Every other translation unit in the compiler includes this header.
 * It is the only module without intra-project dependencies.
 *
 * Conventions
 * -----------
 *   - Functions exported from this module are prefixed with `arena_`,
 *     `diag_`, or `flowc_`. Types and macros use mixed-case names
 *     instead (e.g. `SrcLoc`, `SRCLOC_NONE`, `Diagnostic`, `DiagStream`,
 *     the `Diag*`/`DIAG_*` enums, `FileReadStatus`/`FILE_READ_*`, and
 *     the `FLOWC_*` attribute and ICE macros).
 *   - Functions documented as "aborts on failure" exit the process via
 *     flowc_ice_at(). They never return NULL.
 *   - The arena owns all memory it allocates; pointers handed out by
 *     arena_alloc() are invalidated by arena_destroy() and remain
 *     valid until then.
 *   - Diagnostic identifiers (e.g. "E140" for errors, "W101" for
 *     warnings) are stable across versions; the message text is not.
 *     Pass the identifier as a string literal at each call site so it
 *     can be grepped.
 */

#ifndef FLOWC_UTIL_H
#define FLOWC_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>


/* ====================================================================
 * Portable attributes
 * ====================================================================
 *
 * C99 has no _Noreturn (that's C11) and no portable printf-format
 * attribute. We wrap the GCC/Clang attributes behind macros and
 * expand to nothing on other compilers — the standard portable-C
 * pattern, also used by PostgreSQL's c.h.
 * ==================================================================== */

#if defined(__GNUC__) || defined(__clang__)
#  define FLOWC_NORETURN  __attribute__((noreturn))
#  define FLOWC_PRINTF(fmt_idx, var_idx) \
        __attribute__((format(printf, fmt_idx, var_idx)))
#else
#  define FLOWC_NORETURN
#  define FLOWC_PRINTF(fmt_idx, var_idx)
#endif


/* ====================================================================
 * Source locations
 * ====================================================================
 *
 * Every AST node, every token, and every diagnostic carries a SrcLoc.
 * Lines and columns are 1-based, byte-oriented (byte offsets for
 * column, not character offsets).
 *
 * A SrcLoc with `line == 0` denotes "no location" and is used for
 * diagnostics not tied to a specific point in the source (for
 * example, invocation errors). Such diagnostics are formatted
 * without the `file:line:column:` prefix.
 *
 * The `file` member's lifetime must outlive the SrcLoc. In practice
 * source filenames are arena-allocated once at compilation start and
 * shared across every SrcLoc.
 * ==================================================================== */

typedef struct {
    const char *file;
    int         line;
    int         column;
} SrcLoc;

#define SRCLOC_NONE ((SrcLoc){ NULL, 0, 0 })


/* ====================================================================
 * Arena allocator
 * ====================================================================
 *
 * The compiler allocates from a single arena that is freed in its
 * entirety at process exit. Individual allocations are never freed.
 * This is the right shape for a batch compiler: lifetimes are
 * uniform, the allocator is fast, and no leak analysis is needed
 * because there is nothing to leak by accident.
 *
 * Allocation failure is treated as a fatal internal error
 * (FLOWC_ICE). Callers never see a NULL return from arena_alloc.
 *
 * Each allocation's size is rounded up to a multiple of 16, so every
 * allocation begins at a 16-byte offset within its backing chunk.
 * The chunk base itself comes straight from malloc, so the absolute
 * alignment of a returned pointer is whatever malloc guarantees
 * (max_align_t) — 16 bytes on the platforms this compiler targets,
 * where malloc returns at least 16-byte-aligned memory. This is
 * sufficient for every scalar and aggregate type this compiler
 * manipulates.
 * ==================================================================== */

typedef struct Arena Arena;

/* Create an arena. chunk_size is the size of each backing
 * allocation; pass 0 for the default (64 KiB).  Returns NULL only
 * if the initial allocation fails; callers commonly route this
 * through FLOWC_ICE at startup. */
Arena *arena_create(size_t chunk_size);

/* Free every byte ever allocated from this arena, including the
 * arena handle itself. Passing NULL is a no-op. */
void arena_destroy(Arena *a);

/* Allocate `size` bytes from the arena, aligned as described in the
 * section comment above. The contents are uninitialized. Aborts on
 * out-of-memory. */
void *arena_alloc(Arena *a, size_t size);

/* As arena_alloc, but zeros the returned memory. */
void *arena_alloc_zero(Arena *a, size_t size);

/* Copy a NUL-terminated string into the arena and return a pointer
 * to the copy. Aborts on out-of-memory. */
char *arena_strdup(Arena *a, const char *s);

/* Copy at most `n` bytes of `s` into the arena and append a NUL.
 * Aborts on out-of-memory. */
char *arena_strndup(Arena *a, const char *s, size_t n);


/* ====================================================================
 * Whole-file read
 * ==================================================================== */

/* Outcome of flowc_read_file. Distinguishes the failing syscall so
 * callers can report their own contract-specific message. */
typedef enum {
    FILE_READ_OK = 0,
    FILE_READ_ERR_OPEN,
    FILE_READ_ERR_SEEK,
    FILE_READ_ERR_TELL,
    FILE_READ_ERR_READ   /* fread reported a stream error (ferror) */
} FileReadStatus;

/* Read the entire file at `path` (binary mode) into a freshly
 * malloc'd, NUL-terminated buffer. On FILE_READ_OK, *out_buf is the
 * buffer (caller frees) and out_len is its length excluding the
 * terminator. On any error, returns the failing-step code, leaves
 * out_buf and out_len untouched, and stores the errno of the failing
 * call in out_errno (which may be NULL if the caller does not want
 * it). Aborts via FLOWC_ICE on out-of-memory.
 *
 * Requires a regular, seekable file: it sizes the buffer via
 * fseek/ftell, so a FIFO, char device, or synthetic /proc file would
 * mis-size and under-read (their ftell length does not match what a
 * subsequent read returns). */
FileReadStatus flowc_read_file(const char *path, char **out_buf,
                               size_t *out_len, int *out_errno);


/* ====================================================================
 * JSON string escaping
 * ==================================================================== */

/* Write `s` to `out` with " \ and control characters escaped per
 * RFC 8259. Does NOT emit the surrounding quotes — the caller frames
 * the string. A NULL `s` writes nothing. Shared by the IR emitter
 * and the JSON diagnostic format so both escape identically. */
void flowc_json_escape(FILE *out, const char *s);

/* Format `v` as the shortest decimal that strtod reads back exactly (a literal
 * keeps its precision; a simple value stays short). `buf` needs ~32 bytes.
 * Assumes LC_NUMERIC=C. */
void flowc_format_double(double v, char *buf, size_t cap);

/* Return the entry of `names` closest to `target` by edit distance, or NULL if
 * none is within a small length-scaled threshold. Powers "did you mean". */
const char *flowc_suggest(const char *target, const char **names, size_t n);


/* ====================================================================
 * Diagnostics
 * ====================================================================
 *
 * Diagnostics flow through an explicit `DiagStream` value rather
 * than a process-global static. This lets the compiler be called
 * as a library: each invocation owns its own stream and cannot
 * inherit counts, recorded messages, or JSON-array state from a
 * previous run. The CLI driver creates one stream at startup,
 * attaches stderr, threads it through every stage, finalizes, and
 * destroys it at exit; library callers (see api.h) do the same
 * inside `flowc_context`.
 *
 * Modes
 * -----
 *
 * Every stream records each emitted diagnostic into an internal
 * slab so callers can introspect (diag_count, diag_at) after the
 * run. A stream may also be attached to stderr via
 * diag_attach_stderr(), in which case formatted output is written
 * to fd 2 in addition to being recorded. The CLI uses the
 * attached-stderr mode; library users typically use record-only,
 * but hybrid use is supported.
 *
 * Formatting
 * ----------
 *
 * The stable diagnostic format:
 *
 *     <file>:<line>:<column>: <severity>[<id>]: <message>
 *
 * The id (e.g. "E140" for an error, "W101" for a warning) is stable
 * across versions; the message text is not. Pass the id as a string
 * literal at each call site.
 *
 * Maximum-errors and -Werror behavior are stream-local. Once the
 * -fmax-errors limit is reached, diag_vemit drops further error
 * diagnostics, and the pipeline stops at the next per-pass error
 * gate. This module does not call exit() except via the fatal paths
 * below.
 * ==================================================================== */

typedef enum {
    DIAG_ERROR,
    DIAG_WARNING,
    DIAG_NOTE
} DiagSeverity;

typedef enum {
    DIAG_FORMAT_TEXT,
    DIAG_FORMAT_JSON
} DiagFormat;

typedef enum {
    DIAG_COLOR_AUTO,
    DIAG_COLOR_ALWAYS,
    DIAG_COLOR_NEVER
} DiagColorMode;

/* Recorded diagnostic. `id` is a string literal at the call site
 * and is borrowed, not copied. `message` is arena-allocated inside
 * the stream and lives until diag_clear() or diag_destroy().
 * `loc.file` is borrowed from the caller (the convention every
 * SrcLoc in the compiler follows). */
typedef struct {
    SrcLoc       loc;
    DiagSeverity severity;
    const char  *id;
    const char  *message;
} Diagnostic;

/* Opaque stream. Definition lives in util.c. */
typedef struct DiagStream DiagStream;

/* --- Lifecycle ---------------------------------------------------- */

/* Allocate a fresh stream. Defaults: text format, max_errors=1,
 * werror=false, no stderr sink (record-only). Aborts on OOM. */
DiagStream *diag_create(void);

/* Free the stream and every diagnostic recorded in it. Passing
 * NULL is a no-op. */
void diag_destroy(DiagStream *s);

/* Enable stderr as a secondary sink for this stream. `color_mode`
 * controls ANSI coloring (auto consults isatty + NO_COLOR +
 * FLOWC_DIAGNOSTICS_COLOR). Subsequent diag_emit calls write to
 * fd 2 in addition to recording. */
void diag_attach_stderr(DiagStream *s, DiagColorMode color_mode);

/* Disable the stderr sink. Recording continues; calls to
 * diag_emit no longer write to fd 2. */
void diag_record_only(DiagStream *s);

/* Reset error/warning counters and discard recorded diagnostics.
 * The stream's arena is reused (cheap). For a heavyweight reset
 * that returns memory to the system, destroy and create a new
 * stream. */
void diag_clear(DiagStream *s);

/* --- Configuration ----------------------------------------------- */

/* Each may be called any number of times; the last call wins. */
void diag_set_format(DiagStream *s, DiagFormat fmt);
void diag_set_color(DiagStream *s, DiagColorMode mode);
void diag_set_max_errors(DiagStream *s, int n);  /* 0 = unlimited; default 1 */
void diag_set_werror(DiagStream *s, bool on);

/* --- Emission ---------------------------------------------------- */

/* Emit a diagnostic. id is the stable identifier (e.g. "E140") or
 * NULL for notes that follow a previous diagnostic. fmt is a printf
 * format string for the message. */
void diag_emit(DiagStream *s, SrcLoc loc, DiagSeverity sev,
               const char *id, const char *fmt, ...) FLOWC_PRINTF(5, 6);

/* va_list variant of diag_emit. */
void diag_vemit(DiagStream *s, SrcLoc loc, DiagSeverity sev,
                const char *id, const char *fmt, va_list ap);

/* Emit a `note` continuation tied to the immediately preceding
 * diagnostic. Equivalent to
 * diag_emit(s, loc, DIAG_NOTE, NULL, fmt, ...). */
void diag_note(DiagStream *s, SrcLoc loc, const char *fmt, ...)
    FLOWC_PRINTF(3, 4);

/* --- Query ------------------------------------------------------- */

/* Returns the number of diagnostics emitted at each severity. */
int diag_error_count(const DiagStream *s);
int diag_warning_count(const DiagStream *s);

/* True once the configured -fmax-errors limit has been reached
 * (always false when the limit is 0 = unlimited). diag_vemit consults
 * this to drop further error diagnostics past the limit; the pipeline
 * driver need not poll it, since the per-pass error gate already
 * stops compilation once any error has been recorded. */
bool diag_should_stop(const DiagStream *s);

/* Number of diagnostics recorded in the slab (errors + warnings +
 * notes, in emission order). */
size_t diag_count(const DiagStream *s);

/* Pointer to the i-th recorded diagnostic, or NULL if i is out of
 * range. The returned pointer is invalidated by diag_clear,
 * diag_destroy, or any subsequent diag_emit that grows the slab. */
const Diagnostic *diag_at(const DiagStream *s, size_t i);

/* End-of-compilation cleanup. In JSON mode with an attached stderr
 * sink, closes the diagnostic array. Safe to call multiple times. */
void diag_finalize(DiagStream *s);


/* ====================================================================
 * Fatal exits
 * ====================================================================
 *
 * Three distinct failure modes, each with its own exit code:
 *
 *   2  Invalid command-line invocation.
 *   3  Source file could not be opened or read.
 *   4  Internal compiler error.
 *
 * Each writes a message to stderr and exits; none return.
 * flowc_invocation_error and flowc_io_error write a single line;
 * flowc_ice_at writes the error line followed by a second line asking
 * the user to report the bug.
 * ==================================================================== */

void flowc_invocation_error(const char *fmt, ...)
    FLOWC_NORETURN FLOWC_PRINTF(1, 2);

void flowc_io_error(const char *path, const char *what)
    FLOWC_NORETURN;

/* Internal compiler error. Use the FLOWC_ICE macro at call sites so
 * that __FILE__ and __LINE__ are captured automatically. */
#define FLOWC_ICE(...) flowc_ice_at(__FILE__, __LINE__, __VA_ARGS__)

void flowc_ice_at(const char *file, int line, const char *fmt, ...)
    FLOWC_NORETURN FLOWC_PRINTF(3, 4);

#endif /* FLOWC_UTIL_H */
