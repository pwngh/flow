/* src/util.h
 *
 * Foundation utilities for flowd: source locations, the arena
 * allocator, the diagnostic stream, the SHA-256 wrapper over
 * libcrypto, and the fatal-exit paths.
 *
 * Mirrors flowc/src/util.h. Re-implemented here rather than linked
 * against libflowc because flowd has a different lifecycle (per-load
 * arena, multiple runtime handles per process) and its own R-code
 * vocabulary.
 *
 * Conventions
 * -----------
 *   - All symbols exported from this module are prefixed with `arena_`,
 *     `diag_`, `srcloc_`, `sha256_`, or `flowd_`.
 *   - Functions documented as "aborts on failure" exit the process via
 *     flowd_ice_at(). They never return NULL.
 *   - The arena owns all memory it allocates; pointers handed out by
 *     arena_alloc() are invalidated by arena_destroy() and remain
 *     valid until then.
 *   - Diagnostic identifiers (e.g. "R150") are stable across versions;
 *     the message text is not. Pass the
 *     identifier as a string literal at each call site so it can be
 *     grepped.
 */

#ifndef FLOWD_UTIL_H
#define FLOWD_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/* ====================================================================
 * Portable attributes
 * ==================================================================== */

#if defined(__GNUC__) || defined(__clang__)
#  define FLOWD_NORETURN  __attribute__((noreturn))
#  define FLOWD_PRINTF(fmt_idx, var_idx) \
        __attribute__((format(printf, fmt_idx, var_idx)))
#else
#  define FLOWD_NORETURN
#  define FLOWD_PRINTF(fmt_idx, var_idx)
#endif


/* ====================================================================
 * Source locations
 *
 * For IR loading, `file` is the IR file path and `line`/`column` are
 * meaningless (the IR is a JSON document; lines do not correspond
 * directly to user-visible source). The text-format emitter omits the
 * `line:column:` prefix when line == 0. The IR loader uses a separate
 * JSON-path string (e.g. "tools[3].input[1].type") embedded in the
 * diagnostic message itself.
 * ==================================================================== */

typedef struct {
    const char *file;
    int         line;
    int         column;
} SrcLoc;

#define SRCLOC_NONE ((SrcLoc){ NULL, 0, 0 })


/* ====================================================================
 * Arena allocator
 *
 * The runtime allocates from per-load arenas. A successful
 * flowd_load_ir hands an arena to the new flowd_runtime; the arena is
 * freed in its entirety by flowd_destroy.
 *
 * Allocation failure is treated as a fatal internal error (FLOWD_ICE).
 * Callers never see a NULL return from arena_alloc.
 *
 * Pointers are aligned to 16 bytes, which meets or exceeds the alignment
 * of any scalar the runtime stores in arena memory, so callers can place
 * an arbitrary struct at an arena_alloc result without passing a
 * per-type alignment.
 * ==================================================================== */

typedef struct Arena Arena;

Arena *arena_create(size_t chunk_size);
void   arena_destroy(Arena *a);
void  *arena_alloc(Arena *a, size_t size);
void  *arena_alloc_zero(Arena *a, size_t size);
char  *arena_strdup(Arena *a, const char *s);
char  *arena_strndup(Arena *a, const char *s, size_t n);


/* ====================================================================
 * Diagnostics
 *
 * Same shape as flowc's DiagStream: per-handle, record-and-optionally-
 * sink. The IR loader threads a stream through every pass; the public
 * API wrapper (flowd_load_ir) builds an internal stream, calls the
 * file/buffer entry point, and on failure JSON-formats the recorded
 * diagnostics for flowd_last_error_json.
 * ==================================================================== */

typedef enum {
    DIAG_ERROR,
    DIAG_WARNING,
    DIAG_NOTE
} DiagSeverity;

/* A recorded diagnostic. `message` and `loc.file` are copied into the
 * DiagStream's own arena (owned, valid until diag_destroy), so a
 * diagnostic may outlive the caller's strings; `id` is a borrowed
 * string literal from the call site. */
typedef struct {
    SrcLoc       loc;
    DiagSeverity severity;
    const char  *id;
    const char  *message;
} Diagnostic;

typedef struct DiagStream DiagStream;

DiagStream *diag_create(void);
void        diag_destroy(DiagStream *s);
void        diag_attach_stderr(DiagStream *s);
void        diag_record_only(DiagStream *s);

void        diag_set_max_errors(DiagStream *s, int n);

void diag_emit(DiagStream *s, SrcLoc loc, DiagSeverity sev,
               const char *id, const char *fmt, ...) FLOWD_PRINTF(5, 6);
void diag_vemit(DiagStream *s, SrcLoc loc, DiagSeverity sev,
                const char *id, const char *fmt, va_list ap);
void diag_note(DiagStream *s, SrcLoc loc, const char *fmt, ...)
    FLOWD_PRINTF(3, 4);

size_t            diag_count(const DiagStream *s);
const Diagnostic *diag_at(const DiagStream *s, size_t i);


/* ====================================================================
 * SHA-256 wrapper
 *
 * Hashes a contiguous byte buffer with libcrypto and writes 64 lower-
 * case hex characters plus a NUL terminator into `out_hex`. The intended
 * input is the canonical bytes from value_canonical_serialize.
 *
 * sha256_hex_string is the NUL-terminated C string variant.
 * ==================================================================== */

void sha256_hex(const void *data, size_t len, char out_hex[65]);
void sha256_hex_string(const char *s, char out_hex[65]);


/* ====================================================================
 * Fatal exits
 *
 * FLOWD_ICE reports an unrecoverable internal bug (not a user/IR error,
 * which goes through the diagnostic stream instead) and exits with code
 * 4. The macro captures __FILE__/__LINE__ at the call site so the abort
 * point is identifiable from the message alone.
 * ==================================================================== */

#define FLOWD_ICE(...) flowd_ice_at(__FILE__, __LINE__, __VA_ARGS__)

void flowd_ice_at(const char *file, int line, const char *fmt, ...)
    FLOWD_NORETURN FLOWD_PRINTF(3, 4);

#endif /* FLOWD_UTIL_H */
