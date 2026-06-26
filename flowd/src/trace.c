/* src/trace.c
 *
 * Trace writer + reader implementation. Lays down the trace
 * directory structure and the manifest and node-record formats.
 *
 * The writer accumulates per-node records in memory; nothing is
 * written to disk until seal time. trace_writer_seal() then flushes
 * every node record to nodes/<id>.json and writes the manifest
 * durably and atomically: the manifest is written to manifest.json.tmp,
 * fsync'd, then renamed onto manifest.json (rename gives atomic
 * visibility; the fsync makes the bytes durable before that rename so
 * a reader never sees a present-but-empty manifest after a crash).
 *
 * Value encoding follows inline-with-hashes: canonical-
 * serialize each value, SHA-256 the bytes, write either
 *   {"inline": <value>}
 * (when canonical size ≤ threshold) or
 *   {"hash": "sha256:<hex>"}
 * (with the value's full bytes at values/<hex>.json) otherwise.
 *
 * Determinism: every timestamp, every hash, every node id is
 * derived from inputs and stable wall-clock values. The
 * execution_id is the only non-deterministic field (a CSPRNG
 * suffix) — when the host pins SOURCE_DATE_EPOCH and we
 * want byte-identical replay traces, the caller can pass
 * FLOWD_EXECUTION_ID_SUFFIX via env to override the random hex
 * suffix (the date prefix comes from SOURCE_DATE_EPOCH). The
 * replay tests pin it.
 */

#include "trace.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>


/* ====================================================================
 * Helpers
 * ==================================================================== */

static int trace_audit_append(const char *trace_dir, const char *event,
                              const char *caller, const char *trace_id,
                              const char *decision);

/* Sanitize a flow name into a filesystem-safe slug: the flow name is
 * attacker-influenced and becomes a path component, so collapse
 * everything outside [0-9A-Za-z_] to '_' to keep it path-safe. */
static char *
slug(const char *name)
{
    size_t n = strlen(name);
    char  *out = malloc(n + 1u);
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        out[i] = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                 (c >= 'A' && c <= 'Z') || c == '_' ? (char)c : '_';
    }
    out[n] = '\0';
    return out;
}

/* mkdir -p for a path. Returns 0 on success or errno value. */
static int
ensure_dir(const char *path)
{
    char *tmp = strdup(path);
    if (!tmp) return ENOMEM;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                int e = errno; free(tmp); return e;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        int e = errno; free(tmp); return e;
    }
    free(tmp);
    return 0;
}

/* RFC 3339 UTC with millisecond precision. */
static void
rfc3339_now(char out[32])
{
    struct timespec ts;
#if defined(CLOCK_REALTIME)
    clock_gettime(CLOCK_REALTIME, &ts);
#else
    ts.tv_sec  = time(NULL);
    ts.tv_nsec = 0;
#endif
    /* Honor SOURCE_DATE_EPOCH for replay determinism. */
    const char *sde = getenv("SOURCE_DATE_EPOCH");
    if (sde && *sde) {
        ts.tv_sec  = (time_t)strtoll(sde, NULL, 10);
        ts.tv_nsec = 0;
    }
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    int ms = (int)(ts.tv_nsec / 1000000L);
    snprintf(out, 32, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

/* Milliseconds since epoch (matching rfc3339_now's clock). */
static int64_t
now_ms(void)
{
    struct timespec ts;
#if defined(CLOCK_REALTIME)
    clock_gettime(CLOCK_REALTIME, &ts);
#else
    ts.tv_sec  = time(NULL);
    ts.tv_nsec = 0;
#endif
    const char *sde = getenv("SOURCE_DATE_EPOCH");
    if (sde && *sde) {
        ts.tv_sec  = (time_t)strtoll(sde, NULL, 10);
        ts.tv_nsec = 0;
    }
    return (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000L);
}

/* Six-hex CSPRNG suffix for execution_id. Honors
 * FLOWD_EXECUTION_ID_SUFFIX env var for deterministic test runs, but
 * only when it is at least 6 bytes long: the first 6 bytes are used
 * verbatim and the rest ignored. A shorter value is silently rejected
 * and a random suffix is generated instead, so callers pinning the
 * suffix for byte-identical replay must supply >= 6 bytes. */
static void
exec_suffix(char out[7])
{
    const char *pin = getenv("FLOWD_EXECUTION_ID_SUFFIX");
    if (pin && strlen(pin) >= 6u) {
        memcpy(out, pin, 6); out[6] = '\0'; return;
    }
    static const char hex[] = "0123456789abcdef";
    unsigned char buf[3];
    /* /dev/urandom is non-blocking on every POSIX system since 1996. */
    FILE *fp = fopen("/dev/urandom", "rb");
    if (fp && fread(buf, 1, 3, fp) == 3) {
        fclose(fp);
        for (size_t i = 0; i < 3; i++) {
            out[2u * i + 0u] = hex[(buf[i] >> 4) & 0xF];
            out[2u * i + 1u] = hex[buf[i] & 0xF];
        }
    } else {
        if (fp) fclose(fp);
        /* urandom unavailable: this suffix only needs to keep
         * execution_id dirs from colliding, not to be unpredictable,
         * so pid+time is good enough as a fallback. */
        unsigned long x = (unsigned long)getpid() * 31u
                        + (unsigned long)time(NULL);
        for (size_t i = 0; i < 6; i++) {
            out[i] = hex[(x >> (i * 4u)) & 0xF];
        }
    }
    out[6] = '\0';
}

/* Build execution_id in the canonical format. An optional tag (e.g.
 * "resumed") is appended as a trailing "_<tag>" so derived runs are
 * recognizable by directory name alone. The six-hex suffix is fresh
 * per call, so two tagged runs never collide. */
static char *
make_execution_id(const char *tag)
{
    char date[16];
    time_t t = time(NULL);
    const char *sde = getenv("SOURCE_DATE_EPOCH");
    if (sde && *sde) t = (time_t)strtoll(sde, NULL, 10);
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(date, sizeof date, "%04d_%02d_%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    char suf[7];
    exec_suffix(suf);
    const char *t_sep = (tag && tag[0]) ? "_" : "";
    const char *t_str = (tag && tag[0]) ? tag : "";
    size_t len = strlen("exec_") + strlen(date) + 1u + strlen(suf)
               + strlen(t_sep) + strlen(t_str) + 1u;
    char *id = malloc(len);
    if (!id) return NULL;
    snprintf(id, len, "exec_%s_%s%s%s", date, suf, t_sep, t_str);
    return id;
}

/* Serialize a value to its canonical bytes in a fresh malloc'd buffer
 * (via open_memstream). Caller frees with free(). */
static char *
canonical_bytes(const value_t *v, size_t *out_len)
{
    char  *buf = NULL;
    size_t sz  = 0;
    FILE *fp = open_memstream(&buf, &sz);
    if (!fp) return NULL;
    value_canonical_serialize(v, fp);
    fclose(fp);
    *out_len = sz;
    return buf;
}


/* ====================================================================
 * Per-node accumulator
 * ==================================================================== */

/* One invocation record per row of input the node processed. Direct
 * tool calls produce a single invocation; pipeline-driven tools
 * (when per-site aggregation lands) accumulate multiple. The shape
 * is forward-compatible with the per-row encoding. */
typedef struct {
    char *inputs_json;             /* canonical JSON, or NULL */
    char *output_inline_or_hash;   /* {"inline":...} or {"hash":...} */
    char *output_hash_hex;         /* always — even when inlined */
} invocation_t;

typedef struct {
    char         *node_id;       /* "n0", "n1", ... */
    char         *kind;          /* "input", "tool_call", ... */
    char         *effect_level;  /* "pure", ... or NULL */
    char         *tool_name;     /* tool_call/model_call only */
    char         *tool_version;
    char         *provider;      /* model_call only */
    char         *model;
    char         *model_version;
    char         *callee_flow;   /* subflow_call only */

    /* Provider-reported usage stats (model_call only). */
    bool          has_metrics;
    uint64_t      tokens_in;
    uint64_t      tokens_out;
    double        cost_cents;
    uint32_t      retry_attempts;   /* gateway retries before success */

    /* replay_of cross-reference. */
    char         *replay_orig_path;
    char         *replay_orig_node;
    char         *replay_mode;   /* restored_from_trace | re_invoked |
                                  *  substituted */

    invocation_t *invocations;
    size_t        n_inv;
    size_t        cap_inv;

    char         started_at[32];
    char         ended_at[32];
    int64_t      started_ms;
    int64_t      ended_ms;
    char         *error_msg;     /* NULL on success */
} node_acc_t;


/* ====================================================================
 * Writer struct
 * ==================================================================== */

#define DEFAULT_INLINE_THRESHOLD ((size_t)(64u * 1024u))

struct trace_writer {
    char         *trace_dir;          /* full path */
    char         *execution_id;
    char         *flow_name;
    char         *ir_hash_hex;        /* full sha256: */
    char         *input_hash_hex;
    char         *output_hash_hex;
    char         *resumed_from;       /* original trace dir; NULL unless a resume */
    size_t        inline_threshold;

    char          started_at[32];
    int64_t       started_ms;

    /* Node accumulators, kept until seal time so the manifest's
     * node_count and model_calls index can be filled in. */
    node_acc_t  **nodes;
    size_t        n_nodes;
    size_t        cap_nodes;

    /* Redaction hook. NULL = no transform. */
    flowd_redactor_fn redactor;
    void             *redactor_ctx;

    DiagStream   *diag;
};

int
trace_writer_set_redactor(trace_writer_t *w,
                          flowd_redactor_fn redactor,
                          void *user_ctx)
{
    if (!w) return -1;
    w->redactor     = redactor;
    w->redactor_ctx = user_ctx;
    return 0;
}


/* ====================================================================
 * Writer impl
 * ==================================================================== */

static char *
dup_or_null(const char *s)
{
    return s ? strdup(s) : NULL;
}

/* Apply the writer's redactor (if any) to a byte buffer. Takes
 * ownership of `bytes`: if the redactor rewrites it, the old buffer is
 * freed and the new one returned; otherwise (no redactor, or it
 * declined) the same pointer is returned unchanged. Either way the
 * caller owns exactly one buffer to free. */
static char *
maybe_redact(trace_writer_t *w, char *bytes, size_t *len)
{
    if (!w->redactor) return bytes;
    size_t new_len = 0;
    char *redacted = w->redactor(bytes, *len, &new_len, w->redactor_ctx);
    /* NULL is the documented "leave untouched" signal, not an error:
     * return the original bytes unchanged rather than treating it as
     * a failure (see flowd_redactor_fn contract in trace.h). */
    if (redacted == NULL) return bytes;
    free(bytes);
    *len = new_len;
    return redacted;
}

/* Write a value's canonical form to values/<hash>.json if its bytes
 * exceed the threshold; in either case return a malloc'd JSON
 * fragment representing the inline-or-hash reference, and
 * the canonical hash (always). */
static int
encode_value(trace_writer_t *w, const value_t *v,
             char **out_ref_json, char **out_hash_hex)
{
    size_t blen = 0;
    char  *bytes = canonical_bytes(v, &blen);
    if (!bytes) return -1;
    /* The redactor's output is embedded verbatim into the node record:
     * the inline branch below memcpy's these bytes straight after
     * {"inline": with no validation or re-encoding. The redactor must
     * therefore return valid canonical JSON; a redactor that emits
     * non-JSON or unbalanced bytes produces a nodes/<id>.json that only
     * fails later at read time (cJSON_Parse in slurp_json_file). */
    bytes = maybe_redact(w, bytes, &blen);

    char *hash_hex = malloc(73u);  /* "sha256:" + 64 + NUL */
    if (!hash_hex) { free(bytes); return -1; }
    memcpy(hash_hex, "sha256:", 7);
    sha256_hex(bytes, blen, hash_hex + 7);
    hash_hex[71] = '\0';
    /* sha256_hex wrote 64 hex chars + NUL at position 64 of its buf;
     * combined with our "sha256:" prefix the full string is 71 chars. */

    if (blen > w->inline_threshold) {
        /* Externalize. */
        size_t path_len = strlen(w->trace_dir) + strlen("/values/") +
                          64u + strlen(".json") + 1u;
        char *path = malloc(path_len);
        if (!path) { free(bytes); free(hash_hex); return -1; }
        snprintf(path, path_len, "%s/values/%s.json",
                 w->trace_dir, hash_hex + 7);
        FILE *fp = fopen(path, "wb");
        if (!fp) { free(path); free(bytes); free(hash_hex); return -1; }
        size_t wrote = fwrite(bytes, 1, blen, fp);
        fclose(fp);
        free(path);
        if (wrote != blen) { free(bytes); free(hash_hex); return -1; }

        /* {"hash":"sha256:HEX"} */
        size_t ref_len = strlen("{\"hash\":\"\"}") + strlen(hash_hex) + 1u;
        *out_ref_json = malloc(ref_len);
        if (!*out_ref_json) { free(bytes); free(hash_hex); return -1; }
        snprintf(*out_ref_json, ref_len, "{\"hash\":\"%s\"}", hash_hex);
    } else {
        /* {"inline": <value canonical bytes>} */
        size_t ref_len = strlen("{\"inline\":}") + blen + 1u;
        *out_ref_json = malloc(ref_len);
        if (!*out_ref_json) { free(bytes); free(hash_hex); return -1; }
        memcpy(*out_ref_json, "{\"inline\":", 10);
        memcpy(*out_ref_json + 10, bytes, blen);
        (*out_ref_json)[10 + blen] = '}';
        (*out_ref_json)[10 + blen + 1] = '\0';
    }
    free(bytes);
    *out_hash_hex = hash_hex;
    return 0;
}

static node_acc_t *
new_node(trace_writer_t *w, const char *kind, const char *effect_level)
{
    if (w->n_nodes == w->cap_nodes) {
        size_t ncap = w->cap_nodes ? w->cap_nodes * 2u : 16u;
        node_acc_t **g = realloc(w->nodes, ncap * sizeof *g);
        if (!g) return NULL;
        w->nodes = g; w->cap_nodes = ncap;
    }
    node_acc_t *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    char id_buf[16];
    snprintf(id_buf, sizeof id_buf, "n%zu", w->n_nodes);
    n->node_id      = strdup(id_buf);
    n->kind         = strdup(kind);
    n->effect_level = dup_or_null(effect_level);
    n->started_ms   = now_ms();
    rfc3339_now(n->started_at);
    w->nodes[w->n_nodes++] = n;
    return n;
}

static node_acc_t *
find_node(trace_writer_t *w, const char *id)
{
    for (size_t i = 0; i < w->n_nodes; i++) {
        if (strcmp(w->nodes[i]->node_id, id) == 0) return w->nodes[i];
    }
    return NULL;
}

trace_writer_t *
trace_writer_open(const char *trace_root, const char *flow_name,
                  const char *ir_json, size_t inline_threshold,
                  DiagStream *diag, const char *exec_tag)
{
    if (!trace_root || !flow_name || !ir_json) return NULL;
    if (inline_threshold == 0) inline_threshold = DEFAULT_INLINE_THRESHOLD;

    trace_writer_t *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->diag             = diag;
    w->inline_threshold = inline_threshold;
    w->flow_name        = strdup(flow_name);
    w->execution_id     = make_execution_id(exec_tag);
    if (!w->flow_name || !w->execution_id) goto fail;

    /* trace_dir = "<root>/<slug(flow)>/<execution_id>" */
    char *sl = slug(flow_name);
    if (!sl) goto fail;
    size_t dlen = strlen(trace_root) + 1u + strlen(sl) + 1u +
                  strlen(w->execution_id) + 1u;
    w->trace_dir = malloc(dlen);
    if (!w->trace_dir) { free(sl); goto fail; }
    snprintf(w->trace_dir, dlen, "%s/%s/%s",
             trace_root, sl, w->execution_id);
    free(sl);

    /* Create directories. */
    char sub[1024];
    if (ensure_dir(w->trace_dir) != 0) {
        diag_emit(diag, SRCLOC_NONE, DIAG_ERROR, "R301",
                  "trace dir create failed: %s", w->trace_dir);
        goto fail;
    }
    snprintf(sub, sizeof sub, "%s/nodes",  w->trace_dir);
    if (ensure_dir(sub) != 0) goto fail;
    snprintf(sub, sizeof sub, "%s/values", w->trace_dir);
    if (ensure_dir(sub) != 0) goto fail;

    /* ir_hash. */
    char hash[73];
    memcpy(hash, "sha256:", 7);
    sha256_hex_string(ir_json, hash + 7);
    hash[71] = '\0';
    w->ir_hash_hex = strdup(hash);
    if (!w->ir_hash_hex) goto fail;

    rfc3339_now(w->started_at);
    w->started_ms = now_ms();
    return w;
fail:
    trace_writer_close(w);
    return NULL;
}

const char *trace_writer_dir         (const trace_writer_t *w) { return w ? w->trace_dir    : NULL; }
const char *trace_writer_last_node_id(const trace_writer_t *w) {
    return (w && w->n_nodes > 0) ? w->nodes[w->n_nodes - 1]->node_id : NULL;
}

/* Takes ownership of the three string args regardless of outcome
 * (see the OOM branch below). */
static int
node_push_invocation(node_acc_t *n, char *inputs_json,
                     char *output_ref, char *output_hash)
{
    if (n->n_inv == n->cap_inv) {
        size_t ncap = n->cap_inv ? n->cap_inv * 2u : 2u;
        invocation_t *g = realloc(n->invocations, ncap * sizeof *g);
        if (!g) {
            /* Free the strings even on failure: callers always hand off
             * ownership here, so freeing on the OOM path keeps the
             * contract leak-free whether or not the caller checks the
             * return value. */
            free(inputs_json); free(output_ref); free(output_hash);
            return -1;
        }
        n->invocations = g; n->cap_inv = ncap;
    }
    n->invocations[n->n_inv].inputs_json           = inputs_json;
    n->invocations[n->n_inv].output_inline_or_hash = output_ref;
    n->invocations[n->n_inv].output_hash_hex       = output_hash;
    n->n_inv++;
    return 0;
}

int
trace_writer_set_input(trace_writer_t *w, const value_t *input)
{
    if (!w || !input) return -1;
    /* Hash the raw input here (not via encode_value) because the
     * manifest's input_hash must reflect the value before any
     * redactor rewrites it for on-disk storage. */
    size_t blen; char *bytes = canonical_bytes(input, &blen);
    if (!bytes) return -1;
    char hash[73]; memcpy(hash, "sha256:", 7);
    sha256_hex(bytes, blen, hash + 7); hash[71] = '\0';
    free(bytes);
    w->input_hash_hex = strdup(hash);
    if (!w->input_hash_hex) return -1;

    /* n0 is a synthetic node: the flow input is recorded as the first
     * node so replay and provenance can address it like any other. */
    node_acc_t *n = new_node(w, "input", "pure");
    if (!n) return -1;
    char *ref = NULL, *h = NULL;
    if (encode_value(w, input, &ref, &h) != 0) return -1;
    if (node_push_invocation(n, NULL, ref, h) != 0) return -1;
    rfc3339_now(n->ended_at);
    n->ended_ms = now_ms();
    return 0;
}

const char *
trace_writer_begin_node(trace_writer_t *w, const char *kind,
                        const char *effect_level)
{
    if (!w) return NULL;
    node_acc_t *n = new_node(w, kind, effect_level);
    return n ? n->node_id : NULL;
}

int
trace_writer_set_tool(trace_writer_t *w, const char *node_id,
                      const char *tool_name, const char *tool_version)
{
    node_acc_t *n = find_node(w, node_id);
    if (!n) return -1;
    n->tool_name    = dup_or_null(tool_name);
    n->tool_version = dup_or_null(tool_version);
    return 0;
}

int
trace_writer_set_model(trace_writer_t *w, const char *node_id,
                       const char *provider, const char *model,
                       const char *version)
{
    node_acc_t *n = find_node(w, node_id);
    if (!n) return -1;
    n->provider      = dup_or_null(provider);
    n->model         = dup_or_null(model);
    n->model_version = dup_or_null(version);
    return 0;
}

int
trace_writer_set_model_metrics(trace_writer_t *w, const char *node_id,
                               uint64_t tokens_in, uint64_t tokens_out,
                               double cost_cents, uint32_t retry_attempts)
{
    node_acc_t *n = find_node(w, node_id);
    if (!n) return -1;
    n->has_metrics    = true;
    n->tokens_in      = tokens_in;
    n->tokens_out     = tokens_out;
    n->cost_cents     = cost_cents;
    n->retry_attempts = retry_attempts;
    return 0;
}

int
trace_writer_set_subflow(trace_writer_t *w, const char *node_id,
                         const char *callee_flow)
{
    node_acc_t *n = find_node(w, node_id);
    if (!n) return -1;
    n->callee_flow = dup_or_null(callee_flow);
    return 0;
}

int
trace_writer_set_replay_of(trace_writer_t *w, const char *node_id,
                           const char *trace_path,
                           const char *orig_node,
                           const char *mode)
{
    node_acc_t *n = find_node(w, node_id);
    if (!n) return -1;
    n->replay_orig_path = dup_or_null(trace_path);
    n->replay_orig_node = dup_or_null(orig_node);
    n->replay_mode      = dup_or_null(mode);
    return 0;
}

int
trace_writer_invocation(trace_writer_t *w, const char *node_id,
                        const char *inputs_json, const value_t *output)
{
    node_acc_t *n = find_node(w, node_id);
    if (!n) return -1;
    char *in = dup_or_null(inputs_json);
    if (in && w->redactor) {
        size_t in_len = strlen(in);
        in = maybe_redact(w, in, &in_len);
        /* The redactor only promises to set out_len, not to NUL-
         * terminate, so grow by one byte and terminate. This buffer is
         * later emitted via fprintf("%s") in flush_node, so a missing
         * terminator would be an out-of-bounds read; if the grow fails
         * we cannot safely terminate, so fail rather than persist it. */
        char *grown = realloc(in, in_len + 1u);
        if (!grown) { free(in); return -1; }
        in = grown; in[in_len] = '\0';
    }
    char *ref  = NULL, *h = NULL;
    if (output && encode_value(w, output, &ref, &h) != 0) {
        free(in);
        return -1;
    }
    return node_push_invocation(n, in, ref, h);
}

int
trace_writer_end_node(trace_writer_t *w, const char *node_id,
                      const char *error_msg)
{
    node_acc_t *n = find_node(w, node_id);
    if (!n) return -1;
    if (error_msg) n->error_msg = strdup(error_msg);
    rfc3339_now(n->ended_at);
    n->ended_ms = now_ms();
    return 0;
}

int
trace_writer_set_resumed_from(trace_writer_t *w, const char *original_dir)
{
    if (!w) return -1;
    free(w->resumed_from);
    w->resumed_from = dup_or_null(original_dir);
    return 0;
}

int
trace_writer_set_output(trace_writer_t *w, const value_t *output)
{
    if (!w || !output) return -1;
    size_t blen; char *bytes = canonical_bytes(output, &blen);
    if (!bytes) return -1;
    char hash[73]; memcpy(hash, "sha256:", 7);
    sha256_hex(bytes, blen, hash + 7); hash[71] = '\0';
    free(bytes);
    w->output_hash_hex = strdup(hash);
    if (!w->output_hash_hex) return -1;

    node_acc_t *n = new_node(w, "output", "pure");
    if (!n) return -1;
    char *ref = NULL, *h = NULL;
    if (encode_value(w, output, &ref, &h) != 0) return -1;
    if (node_push_invocation(n, NULL, ref, h) != 0) return -1;
    rfc3339_now(n->ended_at);
    n->ended_ms = now_ms();
    return 0;
}

/* JSON-escape a string into a fixed buffer. Returns bytes written
 * (excluding NUL). Silently truncates if `s` doesn't fit — callers
 * pass generously-sized stack buffers (e.g. char eb[256]) for fields
 * that are bounded in practice, so truncation isn't expected to fire. */
static size_t
jstr_escape(char *out, size_t cap, const char *s)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        /* 7 = the worst-case expansion of one input byte (\uXXXX is 6)
         * plus a margin, so the body below never overruns `cap`. */
        if (o + 7u >= cap) break;
        switch (*p) {
            case '"':  out[o++]='\\'; out[o++]='"';  break;
            case '\\': out[o++]='\\'; out[o++]='\\'; break;
            case '\n': out[o++]='\\'; out[o++]='n';  break;
            case '\r': out[o++]='\\'; out[o++]='r';  break;
            case '\t': out[o++]='\\'; out[o++]='t';  break;
            default:
                if (*p < 0x20) {
                    /* The +7 slack guarantees >= 8 bytes free here, so the
                     * \uXXXX snprintf below (6 chars + NUL) never truncates;
                     * its return value is therefore the exact byte count,
                     * safe to add to o. */
                    o += (size_t)snprintf(out + o, cap - o,
                                          "\\u%04x", *p);
                } else out[o++] = (char)*p;
        }
    }
    if (o < cap) out[o] = '\0';
    return o;
}

/* Flush one node record to nodes/<node_id>.json. */
static int
flush_node(trace_writer_t *w, const node_acc_t *n)
{
    size_t path_len = strlen(w->trace_dir) + strlen("/nodes/") +
                      strlen(n->node_id) + strlen(".json") + 1u;
    char *path = malloc(path_len);
    if (!path) return -1;
    snprintf(path, path_len, "%s/nodes/%s.json",
             w->trace_dir, n->node_id);

    FILE *fp = fopen(path, "wb");
    free(path);
    if (!fp) return -1;

    fputs("{\n", fp);
    fprintf(fp, "  \"node_id\": \"%s\",\n", n->node_id);
    fprintf(fp, "  \"node_kind\": \"%s\",\n", n->kind);
    if (n->effect_level) {
        fprintf(fp, "  \"effect_level\": \"%s\",\n", n->effect_level);
    }
    if (n->tool_name) {
        char eb[256]; jstr_escape(eb, sizeof eb, n->tool_name);
        fprintf(fp, "  \"tool\": \"%s\",\n", eb);
    }
    if (n->tool_version) {
        char eb[256]; jstr_escape(eb, sizeof eb, n->tool_version);
        fprintf(fp, "  \"tool_version\": \"%s\",\n", eb);
    }
    if (n->provider) {
        fprintf(fp, "  \"provider\": \"%s\",\n", n->provider);
    }
    if (n->model) {
        fprintf(fp, "  \"model\": \"%s\",\n", n->model);
    }
    if (n->model_version) {
        fprintf(fp, "  \"model_version\": \"%s\",\n", n->model_version);
    }
    if (n->has_metrics) {
        fprintf(fp, "  \"tokens_in\": %" PRIu64 ",\n",  n->tokens_in);
        fprintf(fp, "  \"tokens_out\": %" PRIu64 ",\n", n->tokens_out);
        fprintf(fp, "  \"cost_cents\": %.4f,\n",        n->cost_cents);
        fprintf(fp, "  \"retry_attempts\": %" PRIu32 ",\n", n->retry_attempts);
    }
    if (n->callee_flow) {
        fprintf(fp, "  \"callee_flow\": \"%s\",\n", n->callee_flow);
    }
    if (n->replay_orig_path && n->replay_orig_node && n->replay_mode) {
        char p_esc[1024], n_esc[64], m_esc[64];
        jstr_escape(p_esc, sizeof p_esc, n->replay_orig_path);
        jstr_escape(n_esc, sizeof n_esc, n->replay_orig_node);
        jstr_escape(m_esc, sizeof m_esc, n->replay_mode);
        fprintf(fp,
            "  \"replay_of\": {"
            "\"trace_path\": \"%s\", "
            "\"node_id\": \"%s\", "
            "\"mode\": \"%s\"},\n",
            p_esc, n_esc, m_esc);
    }
    fprintf(fp, "  \"started_at\": \"%s\",\n", n->started_at);
    fprintf(fp, "  \"ended_at\": \"%s\",\n",   n->ended_at);
    /* Clamp: started_ms/ended_ms come from now_ms() (CLOCK_REALTIME), which
     * can step backward (NTP, manual set, SOURCE_DATE_EPOCH pinning), making
     * the raw difference negative.  Duration is never negative, so floor at 0. */
    fprintf(fp, "  \"elapsed_ms\": %" PRId64 ",\n",
            n->ended_ms - n->started_ms < 0 ? 0 : n->ended_ms - n->started_ms);

    fputs("  \"invocations\": [", fp);
    for (size_t i = 0; i < n->n_inv; i++) {
        const invocation_t *iv = &n->invocations[i];
        fputs(i == 0 ? "\n    " : ",\n    ", fp);
        fputc('{', fp);
        if (iv->inputs_json) {
            fprintf(fp, "\"inputs\": %s, ", iv->inputs_json);
        }
        fprintf(fp, "\"output\": %s",
                iv->output_inline_or_hash
                  ? iv->output_inline_or_hash : "null");
        fputc('}', fp);
    }
    if (n->n_inv > 0) fputs("\n  ", fp);
    fputs("],\n", fp);

    fputs("  \"retries\": [],\n", fp);
    if (n->error_msg) {
        char eb[1024]; jstr_escape(eb, sizeof eb, n->error_msg);
        fprintf(fp, "  \"errors\": {\"message\": \"%s\"}\n", eb);
    } else {
        fputs("  \"errors\": null\n", fp);
    }
    fputs("}\n", fp);
    fclose(fp);
    return 0;
}

int
trace_writer_seal(trace_writer_t *w, trace_status_t status)
{
    if (!w) return -1;

    /* Accumulators live until seal (the manifest below needs node_count,
     * the model_calls index, and budget totals from them), so this is
     * where every node record finally gets written to disk. */
    for (size_t i = 0; i < w->n_nodes; i++) {
        if (flush_node(w, w->nodes[i]) != 0) return -1;
    }

    /* Write to manifest.json.tmp, fsync it, then rename onto
     * manifest.json so a reader never sees a half-written manifest
     * (rename is atomic; the fsync below makes the bytes durable first). */
    size_t mlen = strlen(w->trace_dir) + strlen("/manifest.json.tmp") + 1u;
    char *path     = malloc(mlen);
    char *tmp_path = malloc(mlen);
    if (!path || !tmp_path) { free(path); free(tmp_path); return -1; }
    snprintf(path,     mlen, "%s/manifest.json",     w->trace_dir);
    snprintf(tmp_path, mlen, "%s/manifest.json.tmp", w->trace_dir);

    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) { free(path); free(tmp_path); return -1; }

    char ended[32]; rfc3339_now(ended);
    int64_t ended_ms = now_ms();

    const char *status_s =
        status == TRACE_STATUS_COMPLETE  ? "complete"  :
        status == TRACE_STATUS_FAILED    ? "failed"    :
        status == TRACE_STATUS_CANCELLED ? "cancelled" :
                                           "suspended";

    fputs("{\n", fp);
    fputs("  \"trace_version\": \"1.0\",\n", fp);
    fprintf(fp, "  \"execution_id\": \"%s\",\n", w->execution_id);
    {
        char eb[256]; jstr_escape(eb, sizeof eb, w->flow_name);
        fprintf(fp, "  \"flow\": \"%s\",\n", eb);
    }
    fputs("  \"ir_version\": \"1.0\",\n", fp);
    fprintf(fp, "  \"ir_hash\": \"%s\",\n", w->ir_hash_hex);
    fprintf(fp, "  \"input_hash\": \"%s\",\n",
            w->input_hash_hex ? w->input_hash_hex : "");
    fprintf(fp, "  \"output_hash\": \"%s\",\n",
            w->output_hash_hex ? w->output_hash_hex : "");
    fprintf(fp, "  \"started_at\": \"%s\",\n", w->started_at);
    fprintf(fp, "  \"ended_at\": \"%s\",\n", ended);
    /* Clamp: see node-level elapsed_ms above.  CLOCK_REALTIME can step
     * backward, so floor the duration at 0 instead of emitting a negative. */
    fprintf(fp, "  \"elapsed_ms\": %" PRId64 ",\n",
            ended_ms - w->started_ms < 0 ? 0 : ended_ms - w->started_ms);
    fprintf(fp, "  \"status\": \"%s\",\n", status_s);
    if (w->resumed_from) {
        char eb[1024]; jstr_escape(eb, sizeof eb, w->resumed_from);
        fprintf(fp, "  \"resumed_from\": \"%s\",\n", eb);
    }
    fprintf(fp, "  \"node_count\": %zu,\n", w->n_nodes);

    /* model_calls: a manifest-level index of just the model_call nodes,
     * so cost/usage tooling can read this one array instead of opening
     * every node file. The same loop accumulates the budget totals
     * emitted below, so do both in one pass. */
    uint64_t tot_in = 0, tot_out = 0;
    double   tot_cost = 0.0;
    fputs("  \"model_calls\": [", fp);
    bool first = true;
    for (size_t i = 0; i < w->n_nodes; i++) {
        const node_acc_t *n = w->nodes[i];
        if (strcmp(n->kind, "model_call") != 0) continue;
        if (first) { fputs("\n", fp); first = false; }
        else       fputs(",\n",   fp);
        fputs("    {", fp);
        fprintf(fp, "\"node\": \"%s\"", n->node_id);
        if (n->provider) fprintf(fp, ", \"provider\": \"%s\"", n->provider);
        if (n->model)    fprintf(fp, ", \"model\": \"%s\"",    n->model);
        if (n->model_version)
            fprintf(fp, ", \"version\": \"%s\"", n->model_version);
        if (n->has_metrics) {
            fprintf(fp, ", \"tokens_in\": %" PRIu64 ", \"tokens_out\": %"
                        PRIu64 ", \"cost_cents\": %.4f",
                    n->tokens_in, n->tokens_out, n->cost_cents);
            tot_in   += n->tokens_in;
            tot_out  += n->tokens_out;
            tot_cost += n->cost_cents;
        }
        fputc('}', fp);
    }
    if (!first) fputs("\n  ", fp);
    fputs("],\n", fp);

    /* budget_summary: totals across all model calls in this run. */
    fprintf(fp, "  \"budget_summary\": {\"tokens_in\": %" PRIu64
                ", \"tokens_out\": %" PRIu64 ", \"cost_cents\": %.4f}\n",
            tot_in, tot_out, tot_cost);
    fputs("}\n", fp);

    /* Make the manifest bytes durable before the atomic rename: flush
     * the stdio buffer into the kernel, then fsync the fd so the data
     * is on stable storage. Without this a crash right after the rename
     * could leave manifest.json present but empty/truncated, which a
     * reader treats as a complete trace. On flush/fsync failure, do not
     * rename — leave the .tmp behind rather than publish a bad manifest. */
    if (fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
        fclose(fp);
        free(path); free(tmp_path);
        return -1;
    }
    if (fclose(fp) != 0) {
        free(path); free(tmp_path);
        return -1;
    }

    int rc = rename(tmp_path, path);
    free(path); free(tmp_path);
    return rc;
}

void
trace_writer_close(trace_writer_t *w)
{
    if (!w) return;
    for (size_t i = 0; i < w->n_nodes; i++) {
        node_acc_t *n = w->nodes[i];
        free(n->node_id); free(n->kind); free(n->effect_level);
        free(n->tool_name); free(n->tool_version);
        free(n->provider); free(n->model); free(n->model_version);
        free(n->callee_flow);
        free(n->replay_orig_path);
        free(n->replay_orig_node);
        free(n->replay_mode);
        for (size_t k = 0; k < n->n_inv; k++) {
            free(n->invocations[k].inputs_json);
            free(n->invocations[k].output_inline_or_hash);
            free(n->invocations[k].output_hash_hex);
        }
        free(n->invocations);
        free(n->error_msg);
        free(n);
    }
    free(w->nodes);
    free(w->trace_dir);
    free(w->execution_id);
    free(w->flow_name);
    free(w->ir_hash_hex);
    free(w->input_hash_hex);
    free(w->output_hash_hex);
    free(w->resumed_from);
    free(w);
}


/* ====================================================================
 * Reader
 * ==================================================================== */

struct trace_reader {
    char  *trace_dir;
    char  *caller_id;     /* arena-style strdup; freed at close */
    char  *execution_id;  /* cached from manifest for audit entries */
    cJSON *manifest;
};

static cJSON *
slurp_json_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)sz + 1u);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return NULL;
    }
    buf[sz] = '\0';
    fclose(fp);
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

trace_reader_t *
trace_reader_open(const char *trace_dir, const char *caller_id,
                  DiagStream *diag)
{
    if (!trace_dir) return NULL;
    size_t plen = strlen(trace_dir) + strlen("/manifest.json") + 1u;
    char *mp = malloc(plen);
    if (!mp) return NULL;
    snprintf(mp, plen, "%s/manifest.json", trace_dir);
    cJSON *m = slurp_json_file(mp);
    free(mp);
    if (!m) {
        if (diag) diag_emit(diag, SRCLOC_NONE, DIAG_ERROR, "R301",
                            "no manifest in %s", trace_dir);
        return NULL;
    }
    trace_reader_t *r = calloc(1, sizeof *r);
    if (!r) { cJSON_Delete(m); return NULL; }
    r->trace_dir = strdup(trace_dir);
    r->caller_id = strdup(caller_id ? caller_id : "anonymous");
    r->manifest  = m;

    cJSON *eid = cJSON_GetObjectItemCaseSensitive(m, "execution_id");
    r->execution_id = strdup(cJSON_IsString(eid)
        ? cJSON_GetStringValue(eid) : "");

    /* Audit log: trace_read covers the session's manifest+node
     * access. value_read fires separately per trace_reader_value
     * fetch from the content store. */
    trace_audit_append(trace_dir, "trace_read",
                       r->caller_id, r->execution_id, "allow");
    return r;
}

const cJSON *
trace_reader_manifest(const trace_reader_t *r)
{
    return r ? r->manifest : NULL;
}

cJSON *
trace_reader_node(const trace_reader_t *r, const char *node_id)
{
    if (!r) return NULL;
    size_t plen = strlen(r->trace_dir) + strlen("/nodes/") +
                  strlen(node_id) + strlen(".json") + 1u;
    char *p = malloc(plen);
    if (!p) return NULL;
    snprintf(p, plen, "%s/nodes/%s.json", r->trace_dir, node_id);
    cJSON *j = slurp_json_file(p);
    free(p);
    return j;
}

/* Internal: given an invocation's inline-or-hash JSON object, return
 * the canonical JSON of the value as a heap-allocated string. */
static char *
resolve_invocation_value(const trace_reader_t *r, cJSON *enc)
{
    if (!enc) return NULL;
    cJSON *inline_v = cJSON_GetObjectItemCaseSensitive(enc, "inline");
    cJSON *hash_v   = cJSON_GetObjectItemCaseSensitive(enc, "hash");
    if (inline_v) {
        /* Re-emit canonical JSON from the parsed cJSON. cJSON's
         * unformatted print doesn't guarantee canonical-ordering of
         * record keys, so for fully canonical output the runtime
         * would re-parse into a value_t and serialize canonically.
         * The replay path trusts the on-disk bytes were canonical to
         * begin with (the writer enforces this) and uses cJSON's
         * compact print as a fast path. */
        return cJSON_PrintUnformatted(inline_v);
    }
    if (cJSON_IsString(hash_v)) {
        cJSON *fetched = trace_reader_value(r,
            cJSON_GetStringValue(hash_v));
        if (!fetched) return NULL;
        char *bytes = cJSON_PrintUnformatted(fetched);
        cJSON_Delete(fetched);
        return bytes;
    }
    return NULL;
}

char *
trace_reader_invocation_output(const trace_reader_t *r,
                               const char *node_id,
                               size_t invocation_idx)
{
    if (!r || !node_id) return NULL;
    cJSON *node = trace_reader_node(r, node_id);
    if (!node) return NULL;
    cJSON *invs = cJSON_GetObjectItemCaseSensitive(node, "invocations");
    if (!cJSON_IsArray(invs)) { cJSON_Delete(node); return NULL; }
    cJSON *inv = cJSON_GetArrayItem(invs, (int)invocation_idx);
    if (!inv) { cJSON_Delete(node); return NULL; }
    cJSON *out_enc = cJSON_GetObjectItemCaseSensitive(inv, "output");
    char *out = resolve_invocation_value(r, out_enc);
    cJSON_Delete(node);
    return out;
}

char *
trace_reader_invocation_inputs(const trace_reader_t *r,
                               const char *node_id,
                               size_t invocation_idx)
{
    if (!r || !node_id) return NULL;
    cJSON *node = trace_reader_node(r, node_id);
    if (!node) return NULL;
    cJSON *invs = cJSON_GetObjectItemCaseSensitive(node, "invocations");
    if (!cJSON_IsArray(invs)) { cJSON_Delete(node); return NULL; }
    cJSON *inv = cJSON_GetArrayItem(invs, (int)invocation_idx);
    if (!inv) { cJSON_Delete(node); return NULL; }
    cJSON *in_enc = cJSON_GetObjectItemCaseSensitive(inv, "inputs");
    if (!in_enc) { cJSON_Delete(node); return NULL; }
    /* Inputs are stored directly as canonical JSON, never wrapped in the
     * inline/hash envelope outputs use, so no resolve step is needed. */
    char *out = cJSON_PrintUnformatted(in_enc);
    cJSON_Delete(node);
    return out;
}

cJSON *
trace_reader_value(const trace_reader_t *r, const char *hash)
{
    if (!r || !hash) return NULL;
    /* Accept hash with or without the "sha256:" prefix in the input;
     * the on-disk file is named without the prefix. */
    /* `hex` is interpolated into the path with only the optional "sha256:"
     * prefix stripped -- no hex validation.  Safe only because every caller
     * passes a writer-emitted 64-char hash; a tampered hash containing '/' or
     * '..' would traverse out of values/.  Validate as [0-9a-f]{64} before
     * trusting an untrusted trace dir. */
    const char *hex = hash;
    if (strncmp(hex, "sha256:", 7) == 0) hex += 7;
    size_t plen = strlen(r->trace_dir) + strlen("/values/") +
                  strlen(hex) + strlen(".json") + 1u;
    char *p = malloc(plen);
    if (!p) return NULL;
    snprintf(p, plen, "%s/values/%s.json", r->trace_dir, hex);
    cJSON *j = slurp_json_file(p);
    free(p);

    /* Emit value_read regardless of whether the fetch
     * succeeded — a value-not-found access is still an access. The
     * decision field is "allow" on success, "system" on miss (the
     * runtime initiated the lookup, no caller-policy decision). */
    trace_audit_append(r->trace_dir, "value_read",
                       r->caller_id ? r->caller_id : "anonymous",
                       r->execution_id ? r->execution_id : "",
                       j ? "allow" : "system");
    return j;
}

size_t
trace_reader_node_count(const trace_reader_t *r)
{
    if (!r) return 0;
    cJSON *nc = cJSON_GetObjectItemCaseSensitive(r->manifest, "node_count");
    return cJSON_IsNumber(nc) ? (size_t)nc->valuedouble : 0u;
}

void
trace_reader_close(trace_reader_t *r)
{
    if (!r) return;
    free(r->trace_dir);
    free(r->caller_id);
    free(r->execution_id);
    cJSON_Delete(r->manifest);
    free(r);
}


/* ====================================================================
 * Audit log
 * ==================================================================== */

static int
trace_audit_append(const char *trace_dir, const char *event,
                   const char *caller, const char *trace_id,
                   const char *decision)
{
    if (!trace_dir || !event || !caller || !trace_id || !decision)
        return -1;
    size_t plen = strlen(trace_dir) + strlen("/audit.log") + 1u;
    char *p = malloc(plen);
    if (!p) return -1;
    snprintf(p, plen, "%s/audit.log", trace_dir);
    FILE *fp = fopen(p, "ab");
    free(p);
    if (!fp) return -1;
    char ts[32]; rfc3339_now(ts);
    char e_caller[256], e_event[64], e_trace[64], e_dec[32];
    jstr_escape(e_caller, sizeof e_caller, caller);
    jstr_escape(e_event,  sizeof e_event,  event);
    jstr_escape(e_trace,  sizeof e_trace,  trace_id);
    jstr_escape(e_dec,    sizeof e_dec,    decision);
    fprintf(fp,
        "{\"timestamp\":\"%s\","
        "\"event\":\"%s\","
        "\"caller\":\"%s\","
        "\"trace_id\":\"%s\","
        "\"decision\":\"%s\","
        "\"duration_ms\":%u}\n",
        ts, e_event, e_caller, e_trace, e_dec, 0u);
    fclose(fp);
    return 0;
}
