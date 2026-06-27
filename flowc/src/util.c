/* src/util.c
 *
 * Implementation of the foundation utilities declared in util.h.
 *
 * The file is laid out in the same order as the header: arena
 * allocator first, then the whole-file read helper, then JSON string
 * escaping, then the diagnostic stream, then the fatal-exit paths.
 *
 * No assumption is made about character size, signedness, or
 * representation beyond what the C99 standard guarantees. The arena
 * uses unsigned (size_t) arithmetic for all size calculations, so
 * overflow wraps with defined behavior rather than being UB; where
 * such a wrap could cause an under-allocation (arena_strndup) it is
 * detected and turned into an ICE rather than silently truncated.
 * The diagnostic stream formats each message with vsnprintf into a
 * grown heap buffer (see format_message) and writes the result to
 * stderr with fprintf/fputs/fputc; the fatal-exit paths at the end of
 * the file use vfprintf directly.
 */

#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HAVE_ISATTY) && HAVE_ISATTY
#  include <unistd.h>
#endif


/* ====================================================================
 * Arena allocator
 *
 * Chunks are a linked list, each holding a separately malloc'd data
 * buffer. The two-allocation design (header + data) keeps alignment
 * trivial: malloc(3) returns memory aligned for any object, so the
 * data buffer is already aligned to whatever the platform requires.
 *
 * An allocation that exceeds the configured chunk size gets its own
 * oversize chunk. This means an arena can satisfy arbitrarily large
 * allocations without wasting space in normal-sized chunks.
 * ==================================================================== */

#define FLOWC_ARENA_ALIGN       16u
#define FLOWC_ARENA_DEFAULT     (64u * 1024u)

typedef struct ArenaChunk {
    struct ArenaChunk *next;
    unsigned char     *data;
    size_t             size;   /* bytes available in `data` */
    size_t             used;   /* bytes consumed from `data` */
} ArenaChunk;

struct Arena {
    ArenaChunk *first;
    ArenaChunk *current;
    size_t      chunk_size;
};

static size_t align_up(size_t n, size_t a)
{
    /* `a` must be a power of two. */
    return (n + (a - 1u)) & ~(a - 1u);
}

Arena *arena_create(size_t chunk_size)
{
    Arena *a = malloc(sizeof *a);
    if (!a) {
        return NULL;
    }
    a->first        = NULL;
    a->current      = NULL;
    a->chunk_size   = chunk_size ? chunk_size : FLOWC_ARENA_DEFAULT;
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
        size = 1u;  /* every allocation has a unique address */
    }
    size = align_up(size, FLOWC_ARENA_ALIGN);

    ArenaChunk *c = a->current;
    if (c == NULL || c->used + size > c->size) {
        c = arena_new_chunk(a, size);
        if (c == NULL) {
            FLOWC_ICE("arena out of memory (requested %zu bytes)", size);
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
    /* Guard the n + 1 below: if n == SIZE_MAX it would wrap to 0 and
     * arena_alloc would bump it to a 1-byte block, into which the
     * memcpy of n bytes would overflow. align_up inside arena_alloc
     * wraps similarly for sizes near SIZE_MAX, so reject anything that
     * cannot leave room for the terminator and the alignment round-up. */
    if (n > SIZE_MAX - FLOWC_ARENA_ALIGN) {
        FLOWC_ICE("arena_strndup: length %zu too large", n);
    }
    char *p = arena_alloc(a, n + 1u);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}


/* ====================================================================
 * Whole-file read
 * ==================================================================== */

FileReadStatus flowc_read_file(const char *path, char **out_buf,
                               size_t *out_len, int *out_errno)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        if (out_errno != NULL) *out_errno = errno;
        return FILE_READ_ERR_OPEN;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        if (out_errno != NULL) *out_errno = errno;
        fclose(fp);
        return FILE_READ_ERR_SEEK;
    }
    long size = ftell(fp);
    if (size < 0) {
        if (out_errno != NULL) *out_errno = errno;
        fclose(fp);
        return FILE_READ_ERR_TELL;
    }
    rewind(fp);

    char *buf = malloc((size_t)size + 1u);
    if (buf == NULL) {
        fclose(fp);
        FLOWC_ICE("flowc_read_file: out of memory (size=%ld)", size);
    }
    size_t n = fread(buf, 1u, (size_t)size, fp);
    if (n < (size_t)size && ferror(fp)) {
        /* A real stream error (not a clean early EOF). Check ferror
         * before fclose, which clears the stream state. */
        if (out_errno != NULL) *out_errno = errno;
        fclose(fp);
        free(buf);
        return FILE_READ_ERR_READ;
    }
    fclose(fp);
    buf[n] = '\0';

    *out_buf = buf;
    *out_len = n;
    return FILE_READ_OK;
}


/* ====================================================================
 * JSON string escaping
 * ==================================================================== */

void flowc_json_escape(FILE *out, const char *s)
{
    if (s == NULL) return;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\b': fputs("\\b",  out); break;
            case '\f': fputs("\\f",  out); break;
            case '\n': fputs("\\n",  out); break;
            case '\r': fputs("\\r",  out); break;
            case '\t': fputs("\\t",  out); break;
            default:
                if (*p < 0x20) {
                    fprintf(out, "\\u%04x", (unsigned)*p);
                } else {
                    fputc(*p, out);
                }
        }
    }
}


/* ====================================================================
 * Shortest-round-trip float formatting
 *
 * Format `v` as the shortest decimal that strtod reads back exactly: a literal
 * keeps the precision it was written with (3.141592 stays 3.141592, where plain
 * "%g" rounds to 6 figures) while a simple value stays short (0.1, not
 * 0.10000000000000001). The loop is bounded at 17 digits — the precision that
 * round-trips any IEEE-754 double — so it always terminates and the result is
 * exact. Assumes LC_NUMERIC=C so the separator is '.'. Shared by the IR emitter
 * and the AST dump, so the two never disagree on a float.
 * ==================================================================== */

void flowc_format_double(double v, char *buf, size_t cap)
{
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(buf, cap, "%.*g", prec, v);
        if (prec == 17 || strtod(buf, NULL) == v) return;
    }
}


/* ====================================================================
 * "Did you mean" suggestions
 *
 * Nearest in-scope name to a misspelled one, used to turn an "unknown name"
 * error into a directly-actionable one ("did you mean 'total'?").
 *
 * The metric is optimal string alignment (restricted Damerau-Levenshtein):
 * insertion, deletion, and substitution each cost 1, and — the case plain
 * Levenshtein gets wrong for real typos — so does transposing two adjacent
 * characters, so `csot` is one slip from `cost`, not two. Comparison is
 * case-insensitive, so a stray capital still resolves to the correctly-cased
 * name. The accept threshold is proportional to length (about one edit per
 * three characters), keeping suggestions relevant for short and long
 * identifiers alike without proposing something unrelated. The length gap is a
 * lower bound on the distance, so a candidate that cannot fit the budget — or
 * cannot beat the best match so far — is skipped before running the matrix.
 *
 * MAX_NAME caps the matrix dimensions so the rows live on the stack; an
 * identifier longer than that (rare) simply isn't offered as a suggestion.
 * ==================================================================== */

#define FLOWC_SUGGEST_MAX_NAME 64u

static int suggest_lc(char c) { return tolower((unsigned char)c); }

const char *flowc_suggest(const char *target, const char **names, size_t n)
{
    size_t tlen = strlen(target);
    if (tlen == 0 || tlen > FLOWC_SUGGEST_MAX_NAME) return NULL;

    const char *best = NULL;
    size_t best_dist = (size_t)-1;

    /* Three rolling rows: the transposition case reaches two rows back. tlen
     * can equal MAX_NAME (the guard above rejects only > MAX_NAME), so columns
     * 0..MAX_NAME need MAX_NAME+1 slots; +2 leaves a slot of slack. */
    size_t row0[FLOWC_SUGGEST_MAX_NAME + 2];
    size_t row1[FLOWC_SUGGEST_MAX_NAME + 2];
    size_t row2[FLOWC_SUGGEST_MAX_NAME + 2];

    for (size_t k = 0; k < n; k++) {
        const char *cand = names[k];
        size_t clen = strlen(cand);
        if (clen > FLOWC_SUGGEST_MAX_NAME) continue;

        size_t longer = clen > tlen ? clen : tlen;
        size_t budget = longer < 3u ? 1u : longer / 3u;
        size_t gap    = clen > tlen ? clen - tlen : tlen - clen;
        if (gap > budget || gap >= best_dist) continue;

        size_t *two = row0, *one = row1, *cur = row2;
        for (size_t j = 0; j <= tlen; j++) one[j] = j;        /* the i == 0 row */
        for (size_t i = 1; i <= clen; i++) {
            cur[0] = i;
            int ci = suggest_lc(cand[i - 1]);
            for (size_t j = 1; j <= tlen; j++) {
                int tj   = suggest_lc(target[j - 1]);
                size_t d = one[j - 1] + (ci == tj ? 0u : 1u);  /* substitute */
                if (one[j]     + 1u < d) d = one[j]     + 1u;  /* delete     */
                if (cur[j - 1] + 1u < d) d = cur[j - 1] + 1u;  /* insert     */
                if (i > 1 && j > 1 &&
                    ci == suggest_lc(target[j - 2]) &&
                    suggest_lc(cand[i - 2]) == tj &&
                    two[j - 2] + 1u < d) d = two[j - 2] + 1u;  /* transpose  */
                cur[j] = d;
            }
            size_t *t = two; two = one; one = cur; cur = t;    /* roll the rows */
        }
        size_t dist = one[tlen];        /* `one` holds the last row after the roll */
        if (dist <= budget && dist < best_dist) { best_dist = dist; best = cand; }
    }
    return best;
}


/* ====================================================================
 * Diagnostics
 *
 * Each stream owns its own configuration, counters, recording slab,
 * and (when attached) JSON-array state. The library serializes
 * compiles per flowc_context, so streams are never accessed
 * concurrently and need no internal locking.
 * ==================================================================== */

struct DiagStream {
    /* Configuration. format/color affect only stderr; werror and
     * max_errors also reshape the record slab (see diag_vemit). */
    DiagFormat    format;
    DiagColorMode color_mode;
    bool          color_active;
    bool          werror;
    int           max_errors;

    /* Counters. Incremented on every diag_emit after -Werror
     * upgrade, so they reflect the effective severity. */
    int           error_count;
    int           warning_count;

    /* stderr sink. When attached, formatted output is written to
     * fd 2 in addition to being recorded. */
    bool          stderr_attached;
    bool          json_started;  /* JSON array opened on stderr */

    /* Record slab. Always populated, regardless of attach state. */
    Arena        *arena;
    Diagnostic   *records;
    size_t        record_count;
    size_t        record_capacity;
};

static bool detect_color_auto(void)
{
    /* https://no-color.org/: any non-empty NO_COLOR disables color. */
    const char *no_color = getenv("NO_COLOR");
    if (no_color != NULL && no_color[0] != '\0') {
        return false;
    }
    const char *flowc_color = getenv("FLOWC_DIAGNOSTICS_COLOR");
    if (flowc_color != NULL) {
        if (strcmp(flowc_color, "always") == 0) return true;
        if (strcmp(flowc_color, "never")  == 0) return false;
        /* "auto" or anything else falls through to isatty(). */
    }
#if defined(HAVE_ISATTY) && HAVE_ISATTY
    return isatty(2) ? true : false;
#else
    return false;
#endif
}


/* --- Lifecycle --------------------------------------------------- */

DiagStream *diag_create(void)
{
    DiagStream *s = calloc(1, sizeof *s);
    if (!s) {
        FLOWC_ICE("diag_create: out of memory");
    }
    s->format          = DIAG_FORMAT_TEXT;
    s->color_mode      = DIAG_COLOR_AUTO;
    s->color_active    = false;
    s->werror          = false;
    s->max_errors      = 1;
    s->error_count     = 0;
    s->warning_count   = 0;
    s->stderr_attached = false;
    s->json_started    = false;
    s->arena           = arena_create(0);
    if (s->arena == NULL) {
        free(s);
        FLOWC_ICE("diag_create: arena allocation failed");
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

void diag_attach_stderr(DiagStream *s, DiagColorMode color_mode)
{
    s->stderr_attached = true;
    diag_set_color(s, color_mode);
}

void diag_record_only(DiagStream *s)
{
    s->stderr_attached = false;
}

void diag_clear(DiagStream *s)
{
    s->error_count   = 0;
    s->warning_count = 0;
    s->record_count  = 0;
    s->json_started  = false;
}


/* --- Configuration ----------------------------------------------- */

void diag_set_format(DiagStream *s, DiagFormat fmt)
{
    s->format = fmt;
}

void diag_set_color(DiagStream *s, DiagColorMode mode)
{
    s->color_mode = mode;
    switch (mode) {
        case DIAG_COLOR_ALWAYS: s->color_active = true;  break;
        case DIAG_COLOR_NEVER:  s->color_active = false; break;
        case DIAG_COLOR_AUTO:   s->color_active = detect_color_auto(); break;
    }
}

void diag_set_max_errors(DiagStream *s, int n)
{
    s->max_errors = n;
}

void diag_set_werror(DiagStream *s, bool on)
{
    s->werror = on;
}

int diag_error_count(const DiagStream *s)   { return s->error_count;   }
int diag_warning_count(const DiagStream *s) { return s->warning_count; }

bool diag_should_stop(const DiagStream *s)
{
    if (s->max_errors == 0) {
        return false;  /* unlimited */
    }
    return s->error_count >= s->max_errors;
}

size_t diag_count(const DiagStream *s) { return s->record_count; }

const Diagnostic *diag_at(const DiagStream *s, size_t i)
{
    if (i >= s->record_count) {
        return NULL;
    }
    return &s->records[i];
}


/* --- Recording --------------------------------------------------- */

/* Returns an arena-allocated, NUL-terminated copy of the formatted
 * message. The detour through a heap buffer is unavoidable: the final
 * length is unknown until vsnprintf reports it, so we grow a temporary
 * (1 KiB, doubling) until it fits, then hand a right-sized copy to the
 * arena — the arena has no realloc, so we cannot grow in place there. */
static const char *format_message(DiagStream *s,
                                  const char *fmt, va_list ap)
{
    size_t cap = 1024;
    char  *buf = malloc(cap);
    if (buf == NULL) {
        FLOWC_ICE("diagnostic message allocation failed");
    }
    for (;;) {
        va_list ap2;
        va_copy(ap2, ap);
        int n = vsnprintf(buf, cap, fmt, ap2);
        va_end(ap2);
        if (n < 0) {
            free(buf);
            FLOWC_ICE("vsnprintf failed in diag_emit");
        }
        if ((size_t)n < cap) {
            char *out = arena_strndup(s->arena, buf, (size_t)n);
            free(buf);
            return out;
        }
        /* Need more room. vsnprintf returned the required size
         * excluding the NUL; grow to at least n+1, doubling. */
        size_t new_cap = cap * 2u;
        if (new_cap < (size_t)n + 1u) {
            new_cap = (size_t)n + 1u;
        }
        char *grown = realloc(buf, new_cap);
        if (grown == NULL) {
            free(buf);
            FLOWC_ICE("diagnostic message allocation failed");
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
            FLOWC_ICE("diagnostic record slab allocation failed");
        }
        s->records         = grown;
        s->record_capacity = new_cap;
    }
    s->records[s->record_count++] = d;
}


/* --- text format ------------------------------------------------- */

static const char *sev_word(DiagSeverity sev)
{
    switch (sev) {
        case DIAG_ERROR:   return "error";
        case DIAG_WARNING: return "warning";
        case DIAG_NOTE:    return "note";
    }
    return "?";
}

static const char *sev_color(DiagSeverity sev)
{
    switch (sev) {
        case DIAG_ERROR:   return "\033[1;31m"; /* bold red     */
        case DIAG_WARNING: return "\033[1;35m"; /* bold magenta */
        case DIAG_NOTE:    return "\033[1;36m"; /* bold cyan    */
    }
    return "";
}

static void emit_text(const DiagStream *s, SrcLoc loc, DiagSeverity sev,
                      const char *id, const char *msg)
{
    const char *color_open  = s->color_active ? sev_color(sev) : "";
    const char *color_close = s->color_active ? "\033[0m"      : "";
    const char *bold_open   = s->color_active ? "\033[1m"      : "";
    const char *bold_close  = s->color_active ? "\033[0m"      : "";

    if (loc.line > 0 && loc.file != NULL) {
        fprintf(stderr, "%s%s:%d:%d:%s ",
                bold_open, loc.file, loc.line, loc.column, bold_close);
    }
    if (id != NULL) {
        fprintf(stderr, "%s%s[%s]%s: ",
                color_open, sev_word(sev), id, color_close);
    } else {
        fprintf(stderr, "%s%s%s: ",
                color_open, sev_word(sev), color_close);
    }
    fputs(msg, stderr);
    fputc('\n', stderr);
}


/* --- JSON format ------------------------------------------------- */

static void emit_json(DiagStream *s, SrcLoc loc, DiagSeverity sev,
                      const char *id, const char *msg)
{
    if (!s->json_started) {
        fputc('[', stderr);
        s->json_started = true;
    } else {
        fputc(',', stderr);
    }
    fputs("\n  {", stderr);

    fprintf(stderr, "\"severity\":\"%s\"", sev_word(sev));
    if (id != NULL) {
        fprintf(stderr, ",\"id\":\"%s\"", id);
    }
    if (loc.line > 0 && loc.file != NULL) {
        fputs(",\"file\":\"", stderr);
        flowc_json_escape(stderr, loc.file);
        fputc('"', stderr);
        fprintf(stderr, ",\"line\":%d,\"column\":%d", loc.line, loc.column);
    }
    fputs(",\"message\":\"", stderr);
    flowc_json_escape(stderr, msg);
    fputs("\"}", stderr);
}


/* --- public entry points ---------------------------------------- */

void diag_vemit(DiagStream *s, SrcLoc loc, DiagSeverity sev,
                const char *id, const char *fmt, va_list ap)
{
    /* -Werror upgrades warnings to errors before counting and
     * formatting, so the emitted diagnostic carries the upgraded
     * severity and the error count reflects it. */
    if (sev == DIAG_WARNING && s->werror) {
        sev = DIAG_ERROR;
    }

    /* -fmax-errors: once the error count has reached the limit, drop
     * further error diagnostics — neither recorded nor printed — so the
     * compiler reports at most max_errors of them. Warnings and notes
     * are unaffected. Combined with the pipeline's per-pass error gate
     * (api.c stops before each later pass when error_count != 0), this
     * is what halts compilation after the limit. */
    if (sev == DIAG_ERROR && diag_should_stop(s)) {
        return;
    }

    if (sev == DIAG_ERROR) {
        s->error_count++;
    } else if (sev == DIAG_WARNING) {
        s->warning_count++;
    }

    /* Format once, route to both the record slab and (if attached)
     * the stderr sink. The message is arena-allocated so the
     * recorded Diagnostic's pointer remains valid until diag_clear
     * or diag_destroy. */
    const char *msg = format_message(s, fmt, ap);

    Diagnostic d;
    d.loc      = loc;
    d.severity = sev;
    d.id       = id;
    d.message  = msg;
    record_push(s, d);

    if (s->stderr_attached) {
        switch (s->format) {
            case DIAG_FORMAT_TEXT: emit_text(s, loc, sev, id, msg); break;
            case DIAG_FORMAT_JSON: emit_json(s, loc, sev, id, msg); break;
        }
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

void diag_finalize(DiagStream *s)
{
    if (!s->stderr_attached) {
        return;
    }
    if (s->format == DIAG_FORMAT_JSON) {
        if (!s->json_started) {
            fputs("[]\n", stderr);
        } else {
            fputs("\n]\n", stderr);
        }
        s->json_started = false;
    }
}


/* ====================================================================
 * Fatal exits
 *
 * Each writes a single-line message to stderr and exits with its own
 * code: invocation error 2, I/O error 3, internal compiler error 4.
 * ==================================================================== */

void flowc_invocation_error(const char *fmt, ...)
{
    fputs("flowc: ", stderr);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

void flowc_io_error(const char *path, const char *what)
{
    fprintf(stderr, "flowc: %s: %s: %s\n", what, path, strerror(errno));
    exit(3);
}

void flowc_ice_at(const char *file, int line, const char *fmt, ...)
{
    fprintf(stderr, "flowc: internal compiler error at %s:%d: ", file, line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fputs("flowc: please report this bug.\n",
          stderr);
    exit(4);
}
