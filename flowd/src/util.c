/* src/util.c
 *
 * Implementation of the foundation utilities declared in util.h.
 *
 * Order matches the header: arena allocator first, then the diagnostic
 * stream, then the SHA-256 wrapper, then the single fatal-exit path
 * (flowd_ice_at). flowc/src/util.c has three fatal-exit functions
 * (flowc_invocation_error, flowc_io_error, flowc_ice_at); flowd, which
 * only loads a JSON IR rather than parsing source files, keeps only the
 * ICE path.
 *
 * The arena and DiagStream implementations are ported essentially
 * verbatim from flowc/src/util.c (the patterns are stable; mirroring
 * them keeps the two codebases' diagnostics interchangeable). The
 * SHA-256 wrapper is new — flowc has no equivalent.
 */

#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef FLOWD_BUILTIN_SHA256
#include "sha256_builtin.h"   /* no-libcrypto builds, e.g. WASM */
#else
#include <openssl/sha.h>
#endif


/* ====================================================================
 * Arena allocator
 * ==================================================================== */

#define FLOWD_ARENA_ALIGN       16u
#define FLOWD_ARENA_DEFAULT     (64u * 1024u)

typedef struct ArenaChunk {
    struct ArenaChunk *next;
    unsigned char     *data;
    size_t             size;
    size_t             used;
} ArenaChunk;

struct Arena {
    ArenaChunk *first;
    ArenaChunk *current;
    size_t      chunk_size;
};

static size_t align_up(size_t n, size_t a)
{
    return (n + (a - 1u)) & ~(a - 1u);
}

Arena *arena_create(size_t chunk_size)
{
    Arena *a = malloc(sizeof *a);
    if (!a) {
        return NULL;
    }
    a->first       = NULL;
    a->current     = NULL;
    a->chunk_size  = chunk_size ? chunk_size : FLOWD_ARENA_DEFAULT;
    return a;
}

void arena_destroy(Arena *a)
{
    if (!a) {
        return;
    }
    for (ArenaChunk *c = a->first; c != NULL; ) {
        ArenaChunk *next = c->next;
        free(c->data);
        free(c);
        c = next;
    }
    free(a);
}

static ArenaChunk *arena_new_chunk(Arena *a, size_t min_size)
{
    ArenaChunk *c = malloc(sizeof *c);
    if (!c) {
        return NULL;
    }
    size_t size = a->chunk_size;
    if (size < min_size) {
        size = min_size;
    }
    c->data = malloc(size);
    if (!c->data) {
        free(c);
        return NULL;
    }
    c->next = NULL;
    c->size = size;
    c->used = 0;

    if (a->first == NULL) {
        a->first = c;
    }
    if (a->current != NULL) {
        a->current->next = c;
    }
    a->current = c;
    return c;
}

void *arena_alloc(Arena *a, size_t size)
{
    if (size == 0u) {
        size = 1u;
    }
    /* Guard the round-up: align_up adds (FLOWD_ARENA_ALIGN - 1) before
     * masking, which wraps to a small value for sizes within the
     * alignment of SIZE_MAX. A wrapped-small size would also defeat the
     * capacity check below and hand out an undersized block. */
    if (size > SIZE_MAX - (FLOWD_ARENA_ALIGN - 1u)) {
        FLOWD_ICE("arena allocation too large (requested %zu bytes)", size);
    }
    size = align_up(size, FLOWD_ARENA_ALIGN);

    ArenaChunk *c = a->current;
    /* Compare against remaining capacity instead of summing used + size,
     * which could itself overflow. */
    if (c == NULL || size > c->size - c->used) {
        c = arena_new_chunk(a, size);
        if (c == NULL) {
            FLOWD_ICE("arena out of memory (requested %zu bytes)", size);
        }
    }
    void *p = c->data + c->used;
    c->used += size;
    return p;
}

void *arena_alloc_zero(Arena *a, size_t size)
{
    void *p = arena_alloc(a, size);
    memset(p, 0, size);
    return p;
}

char *arena_strdup(Arena *a, const char *s)
{
    return arena_strndup(a, s, strlen(s));
}

char *arena_strndup(Arena *a, const char *s, size_t n)
{
    char *p = arena_alloc(a, n + 1u);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}


/* ====================================================================
 * Diagnostics
 * ==================================================================== */

struct DiagStream {
    int           max_errors;

    int           error_count;
    int           warning_count;

    bool          stderr_attached;

    Arena        *arena;
    Diagnostic   *records;
    size_t        record_count;
    size_t        record_capacity;
};

DiagStream *diag_create(void)
{
    DiagStream *s = calloc(1, sizeof *s);
    if (!s) {
        FLOWD_ICE("diag_create: out of memory");
    }
    s->max_errors      = 1;
    s->error_count     = 0;
    s->warning_count   = 0;
    s->stderr_attached = false;
    s->arena           = arena_create(0);
    if (s->arena == NULL) {
        free(s);
        FLOWD_ICE("diag_create: arena allocation failed");
    }
    s->records         = NULL;
    s->record_count    = 0;
    s->record_capacity = 0;
    return s;
}

void diag_destroy(DiagStream *s)
{
    if (s == NULL) {
        return;
    }
    free(s->records);
    arena_destroy(s->arena);
    free(s);
}

void diag_attach_stderr(DiagStream *s)
{
    s->stderr_attached = true;
}

void diag_record_only(DiagStream *s)
{
    s->stderr_attached = false;
}

void diag_set_max_errors(DiagStream *s, int n) { s->max_errors = n; }

/* True once the configured max_errors limit has been reached (0 =
 * unlimited, so never). diag_vemit consults this to drop further error
 * diagnostics past the limit, matching flowc's behavior — flowd_load_ir
 * sets the cap to 1 so a malformed IR surfaces its first error rather
 * than a cascade. */
static bool diag_should_stop(const DiagStream *s)
{
    if (s->max_errors == 0) return false;
    return s->error_count >= s->max_errors;
}

size_t diag_count(const DiagStream *s) { return s->record_count; }

const Diagnostic *diag_at(const DiagStream *s, size_t i)
{
    if (i >= s->record_count) return NULL;
    return &s->records[i];
}

static const char *format_message(DiagStream *s, const char *fmt, va_list ap)
{
    size_t cap = 1024;
    char  *buf = malloc(cap);
    if (buf == NULL) {
        FLOWD_ICE("diagnostic message allocation failed");
    }
    for (;;) {
        va_list ap2;
        va_copy(ap2, ap);
        int n = vsnprintf(buf, cap, fmt, ap2);
        va_end(ap2);
        if (n < 0) {
            free(buf);
            FLOWD_ICE("vsnprintf failed in diag_emit");
        }
        if ((size_t)n < cap) {
            char *out = arena_strndup(s->arena, buf, (size_t)n);
            free(buf);
            return out;
        }
        size_t new_cap = cap * 2u;
        if (new_cap < (size_t)n + 1u) {
            new_cap = (size_t)n + 1u;
        }
        char *grown = realloc(buf, new_cap);
        if (grown == NULL) {
            free(buf);
            FLOWD_ICE("diagnostic message allocation failed");
        }
        buf = grown;
        cap = new_cap;
    }
}

static void record_push(DiagStream *s, Diagnostic d)
{
    if (s->record_count == s->record_capacity) {
        size_t new_cap = s->record_capacity ? s->record_capacity * 2u : 16u;
        Diagnostic *grown = realloc(s->records, new_cap * sizeof *grown);
        if (grown == NULL) {
            FLOWD_ICE("diagnostic record slab allocation failed");
        }
        s->records         = grown;
        s->record_capacity = new_cap;
    }
    s->records[s->record_count++] = d;
}

static const char *sev_word(DiagSeverity sev)
{
    switch (sev) {
        case DIAG_ERROR:   return "error";
        case DIAG_WARNING: return "warning";
        case DIAG_NOTE:    return "note";
    }
    return "?";
}

static void emit_text(SrcLoc loc, DiagSeverity sev,
                      const char *id, const char *msg)
{
    if (loc.line > 0 && loc.file != NULL) {
        fprintf(stderr, "%s:%d:%d: ", loc.file, loc.line, loc.column);
    } else if (loc.file != NULL) {
        fprintf(stderr, "%s: ", loc.file);
    }
    if (id != NULL) {
        fprintf(stderr, "%s[%s]: ", sev_word(sev), id);
    } else {
        fprintf(stderr, "%s: ", sev_word(sev));
    }
    fputs(msg, stderr);
    fputc('\n', stderr);
}

void diag_vemit(DiagStream *s, SrcLoc loc, DiagSeverity sev,
                const char *id, const char *fmt, va_list ap)
{
    /* Once max_errors errors have been recorded, drop further error
     * diagnostics — neither counted nor recorded — so the cap is real.
     * Warnings and notes are unaffected. */
    if (sev == DIAG_ERROR && diag_should_stop(s)) {
        return;
    }

    if (sev == DIAG_ERROR) {
        s->error_count++;
    } else if (sev == DIAG_WARNING) {
        s->warning_count++;
    }

    const char *msg = format_message(s, fmt, ap);

    /* Own the file string, not just the message. A recorded diagnostic
     * can outlive the caller's SrcLoc string: the IR loader destroys the
     * runtime arena on a parse-failure path before the diagnostic is
     * read back via flowd_last_error_json, so borrowing loc.file there
     * is a use-after-free. Copying it into the stream's arena (as the
     * message already is) makes every Diagnostic self-owned and removes
     * the hazard for all call sites. */
    if (loc.file != NULL) {
        loc.file = arena_strdup(s->arena, loc.file);
    }

    Diagnostic d;
    d.loc      = loc;
    d.severity = sev;
    d.id       = id;
    d.message  = msg;
    record_push(s, d);

    if (s->stderr_attached) {
        emit_text(loc, sev, id, msg);
    }
}

void diag_emit(DiagStream *s, SrcLoc loc, DiagSeverity sev,
               const char *id, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_vemit(s, loc, sev, id, fmt, ap);
    va_end(ap);
}

void diag_note(DiagStream *s, SrcLoc loc, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_vemit(s, loc, DIAG_NOTE, NULL, fmt, ap);
    va_end(ap);
}


/* ====================================================================
 * SHA-256 wrapper
 *
 * libcrypto's SHA256() exists in modern OpenSSL but is deprecated in
 * favor of the EVP API. We use the legacy convenience function because
 * it is widely available, unconditional under our pinned OpenSSL 3 dep,
 * and the canonical-value buffers are always small enough that single-
 * shot hashing is the right shape. If a future OpenSSL drops the
 * symbol, swap to EVP_Q_digest or EVP_Digest here without touching
 * callers.
 *
 * Builds compiled with -DFLOWD_BUILTIN_SHA256 use the vendored
 * implementation instead and link no libcrypto; the digest is identical
 * either way (verified in the test suite), so the value store is shared.
 * ==================================================================== */

void sha256_hex(const void *data, size_t len, char out_hex[65])
{
    unsigned char digest[32];     /* SHA-256 is always 32 bytes */
#ifdef FLOWD_BUILTIN_SHA256
    flowd_sha256_builtin(data, len, digest);
#else
    SHA256((const unsigned char *)data, len, digest);
#endif
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32u; i++) {
        out_hex[2u * i + 0u] = hex[(digest[i] >> 4) & 0xF];
        out_hex[2u * i + 1u] = hex[digest[i] & 0xF];
    }
    out_hex[64] = '\0';
}

void sha256_hex_string(const char *s, char out_hex[65])
{
    sha256_hex(s, strlen(s), out_hex);
}


/* ====================================================================
 * Fatal exits
 * ==================================================================== */

void flowd_ice_at(const char *file, int line, const char *fmt, ...)
{
    fprintf(stderr, "flowd: internal runtime error at %s:%d: ", file, line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fputs("flowd: please report this bug.\n", stderr);
    exit(4);
}
