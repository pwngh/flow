/* src/adapters/anthropic.c
 *
 * Anthropic Messages API adapter.
 *
 * The adapter is invoked through the `flowd_provider_adapter_t`
 * vtable. Each `invoke` call:
 *
 *   1. Reads the configuration the host registered (api_key,
 *      base_url, max_tokens, timeout_ms).
 *   2. Constructs an Anthropic Messages request body from the
 *      runtime's request_json — the tool's args object. v1 reads
 *      only the `prompt` string field and sends it as a single
 *      user message; any other fields in the args (system,
 *      temperature, a caller-supplied messages array, etc.) are
 *      ignored. A richer mapping is a future extension.
 *   3. Issues an HTTPS POST via libcurl.
 *   4. Parses the response, extracts the assistant message text,
 *      returns it as a JSON string matching the tool's declared
 *      return type.
 *
 * On transport failure (5xx, network error, timeout) — and on HTTP
 * 429 rate limiting, which is treated as retryable — the adapter
 * returns NULL with err_msg set to a description that does not
 * begin with "noretry:" — the gateway's retry wrapper will then
 * retry per the IR's declared retry policy.
 *
 * On application failure (other 4xx, schema mismatch from the
 * provider) the adapter returns NULL with err_msg prefixed
 * `noretry:` — the gateway honors that as APPLICATION_ERROR and
 * skips retry.
 *
 * Secrets handling: the API key reaches the adapter through
 * `cfg->api_key`. The adapter attaches it to the `x-api-key`
 * request header and uses it nowhere else. It is never written
 * to err_msg, never returned in the response body, never logged.
 */

#include "anthropic.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "../cjson/cJSON.h"


/* No mutable adapter state: all per-call configuration travels through
 * `user_ctx` (the cfg pointer), so the adapter struct itself adds no
 * cross-thread hazard. One caveat applies to concurrent invocation:
 * do_invoke() reaches libcurl via curl_easy_init(), and if no
 * curl_global_init() has run yet the first curl_easy_init() performs an
 * implicit global init that libcurl documents as NOT thread-safe. Safe
 * concurrent invocation therefore requires the host to call
 * curl_global_init(CURL_GLOBAL_DEFAULT) once at startup, before any
 * worker thread can invoke this adapter; given that, separate threads
 * may invoke one registered adapter concurrently. */
static const char *anthropic_provider_name(void)
{
    return "anthropic";
}

static int anthropic_supports_model(const char *model, void *user_ctx)
{
    (void)user_ctx;
    return model && strncmp(model, "claude-", 7) == 0;
}


/* Accumulates the response body across the many recv_cb calls libcurl
 * makes as the body arrives in chunks; recv_cb grows it as needed. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} recv_buf_t;

/* Hard ceiling on a single response body. A runaway/hostile server could
 * otherwise stream until the unbounded doubling exhausts memory, and the
 * `ncap *= 2u` itself wraps once the capacity passes SIZE_MAX/2. */
#define MAX_RESPONSE_BYTES (64u * 1024u * 1024u)

static size_t
recv_cb(char *ptr, size_t size, size_t nmemb, void *user)
{
    recv_buf_t *b = user;
    size_t add = size * nmemb;
    /* Refuse to grow past the cap; a short write tells libcurl to abort
     * the transfer, which surfaces as a transport error to the caller. */
    if (add > MAX_RESPONSE_BYTES || b->len + add > MAX_RESPONSE_BYTES)
        return 0;
    if (b->len + add + 1u > b->cap) {
        size_t ncap = b->cap ? b->cap * 2u : 4096u;
        /* Bounded by the cap above, so this loop terminates well below
         * SIZE_MAX; still, never let the doubling wrap. */
        while (ncap < b->len + add + 1u) {
            if (ncap > SIZE_MAX / 2u) return 0;
            ncap *= 2u;
        }
        char *grown = realloc(b->data, ncap);
        if (!grown) return 0;
        b->data = grown;
        b->cap  = ncap;
    }
    memcpy(b->data + b->len, ptr, add);
    b->len += add;
    b->data[b->len] = '\0';
    return add;
}


/* Build the request body for Anthropic. Reads only the `prompt`
 * field from the args object and sends it as a single user message;
 * all other args fields are ignored (see the v1 note in the file
 * header). A missing or non-string `prompt` becomes the empty string
 * rather than an error — only an unparseable args_json fails here. */
static char *
build_request_body(const char *model, const char *args_json,
                   uint32_t max_tokens)
{
    cJSON *args = cJSON_Parse(args_json ? args_json : "{}");
    if (!args) return NULL;

    cJSON *prompt_v = cJSON_GetObjectItemCaseSensitive(args, "prompt");
    const char *prompt = cJSON_IsString(prompt_v)
                       ? cJSON_GetStringValue(prompt_v)
                       : "";

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", model);
    cJSON_AddNumberToObject(req, "max_tokens",
                            max_tokens ? (double)max_tokens : 1024.0);
    cJSON *messages = cJSON_CreateArray();
    cJSON *msg      = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", prompt);
    cJSON_AddItemToArray(messages, msg);
    cJSON_AddItemToObject(req, "messages", messages);

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    cJSON_Delete(args);
    return body;
}


/* Extract the assistant message text from Anthropic's response and
 * wrap it as a JSON string suitable for the runtime to parse as
 * the tool's declared output value. The expected response shape:
 *
 *   { "content": [{"type": "text", "text": "..."}, ...],
 *     "usage":   {"input_tokens": N, "output_tokens": M}, ... }
 *
 * We return the content of the first block whose `type` is "text"
 * (scanning past any leading "thinking" or "tool_use" blocks) as a
 * JSON string literal; if no block declares type "text" we fall back
 * to a string `text` field on content[0]. Any later text blocks are
 * not concatenated. Tools whose declared output is a richer record
 * type can map across via a host-side adapter wrapper or by parsing
 * the raw response themselves.
 *
 * Also populate *tokens_in / *tokens_out from the
 * `usage` block (0 if absent). */
static char *
extract_text_response(const char *body,
                      uint64_t   *tokens_in,
                      uint64_t   *tokens_out)
{
    if (tokens_in)  *tokens_in  = 0;
    if (tokens_out) *tokens_out = 0;

    cJSON *parsed = cJSON_Parse(body);
    if (!parsed) return NULL;
    cJSON *content = cJSON_GetObjectItemCaseSensitive(parsed, "content");
    if (!cJSON_IsArray(content) || cJSON_GetArraySize(content) == 0) {
        cJSON_Delete(parsed);
        return NULL;
    }
    /* Scan for the first block whose `type` is "text". Extended-thinking
     * models place a "thinking" block (and tool-use responses a "tool_use"
     * block) ahead of the answer, so the text block is not always at index 0.
     * If no block declares type "text" but the first block carries a string
     * `text` field, fall back to that for forward compatibility. */
    cJSON *text = NULL;
    int n = cJSON_GetArraySize(content);
    for (int i = 0; i < n; i++) {
        cJSON *blk  = cJSON_GetArrayItem(content, i);
        cJSON *type = cJSON_GetObjectItemCaseSensitive(blk, "type");
        if (cJSON_IsString(type)
            && strcmp(cJSON_GetStringValue(type), "text") == 0) {
            cJSON *t = cJSON_GetObjectItemCaseSensitive(blk, "text");
            if (cJSON_IsString(t)) { text = t; break; }
        }
    }
    if (!text) {
        cJSON *first = cJSON_GetArrayItem(content, 0);
        cJSON *t = cJSON_GetObjectItemCaseSensitive(first, "text");
        if (cJSON_IsString(t)) text = t;
    }
    if (!text) {
        cJSON_Delete(parsed);
        return NULL;
    }

    cJSON *usage = cJSON_GetObjectItemCaseSensitive(parsed, "usage");
    if (cJSON_IsObject(usage)) {
        cJSON *in  = cJSON_GetObjectItemCaseSensitive(usage, "input_tokens");
        cJSON *out = cJSON_GetObjectItemCaseSensitive(usage, "output_tokens");
        /* Casting a double >= 2^64 to uint64_t is UB; range-check before the
         * cast (1.8e19 < 2^64). An out-of-range/absent value leaves the 0 set
         * at function entry. */
        if (tokens_in && cJSON_IsNumber(in)) {
            double d = in->valuedouble;
            if (d >= 0.0 && d < 1.8e19) *tokens_in = (uint64_t)d;
        }
        if (tokens_out && cJSON_IsNumber(out)) {
            double d = out->valuedouble;
            if (d >= 0.0 && d < 1.8e19) *tokens_out = (uint64_t)d;
        }
    }

    /* Wrap as a JSON string literal: the tool's declared output is `string`. */
    cJSON *wrap = cJSON_CreateString(cJSON_GetStringValue(text));
    char *outs = cJSON_PrintUnformatted(wrap);
    cJSON_Delete(wrap);
    cJSON_Delete(parsed);
    return outs;
}


/* Core dispatch: issues one HTTP request and fills `out`. Used by
 * both the v1 `invoke` shim and the v2 `invoke_with_metrics` entry.
 * On success `out->response_json` is set; on failure `out->err_msg`
 * is set. Tokens/cost are 0 unless the response had a `usage` block. */
static void
do_invoke(const char *model, const char *request_json,
          const anthropic_adapter_config_t *cfg,
          flowd_adapter_response_t *out)
{
    out->response_json = NULL;
    out->err_msg       = NULL;
    out->tokens_in     = 0;
    out->tokens_out    = 0;
    out->cost_cents    = 0.0;

    if (!cfg || !cfg->api_key) {
        out->err_msg = strdup("noretry: anthropic adapter has no API key");
        return;
    }

    char *body = build_request_body(model, request_json,
                                    cfg->max_tokens);
    if (!body) {
        out->err_msg = strdup("noretry: anthropic adapter: cannot "
                              "build request body");
        return;
    }

    const char *base = cfg->base_url ? cfg->base_url
                                     : "https://api.anthropic.com";
    size_t ulen = strlen(base) + strlen("/v1/messages") + 1u;
    char *url = malloc(ulen);
    if (!url) {
        free(body);
        out->err_msg = strdup("noretry: anthropic adapter: out of memory");
        return;
    }
    snprintf(url, ulen, "%s/v1/messages", base);

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(body); free(url);
        out->err_msg = strdup("curl_easy_init failed");
        return;
    }

    /* Headers. The api-key header is the one carrying the secret;
     * we set it on every call and never log the value. */
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "content-type: application/json");
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    char hdr[256];
    snprintf(hdr, sizeof hdr, "x-api-key: %s", cfg->api_key);
    headers = curl_slist_append(headers, hdr);

    recv_buf_t rb = {0};

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  recv_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &rb);
    if (cfg->timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                         (long)cfg->timeout_ms);
    }

    CURLcode rc = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(url);
    free(body);

    if (rc != CURLE_OK) {
        char buf[256];
        snprintf(buf, sizeof buf, "transport: curl %d (%s)",
                 (int)rc, curl_easy_strerror(rc));
        out->err_msg = strdup(buf);
        free(rb.data);
        return;
    }

    if (http_status >= 500) {
        char buf[256];
        snprintf(buf, sizeof buf,
                 "transport: HTTP %ld from anthropic", http_status);
        out->err_msg = strdup(buf);
        free(rb.data);
        return;
    }

    if (http_status == 429) {
        /* 429 Too Many Requests is a rate-limit signal, not a permanent
         * application error: it is the canonical retryable status and what
         * Anthropic returns under rate limiting. We report it WITHOUT the
         * "noretry:" prefix so the gateway's retry wrapper engages per the
         * IR's declared retry policy. (Anthropic's 529 "overloaded" lands in
         * the >=500 branch above and is likewise retryable.) As elsewhere we
         * surface only the status code, never the raw body, to keep request
         * content out of err_msg (which is traced without Layer-3 redaction). */
        out->err_msg = strdup("transport: HTTP 429 from anthropic");
        free(rb.data);
        return;
    }

    if (http_status >= 400) {
        /* Other 4xx → application error; do not retry. We surface only the
         * status code, never the raw response body: that body can echo
         * request content, and err_msg flows verbatim into the trace
         * without passing through the Layer-3 redactor. Keeping the body
         * out is the only way to honor the "never trace secrets" contract
         * here (a truncated snippet would not have sanitized anything). */
        char buf[64];
        snprintf(buf, sizeof buf, "noretry: HTTP %ld from anthropic",
                 http_status);
        out->err_msg = strdup(buf);
        free(rb.data);
        return;
    }

    /* 2xx. A malformed or unexpected body here is the provider's fault,
     * not the transport's, so it is reported noretry: (application error)
     * rather than left to the retry wrapper. */
    char *text = extract_text_response(rb.data ? rb.data : "",
                                       &out->tokens_in,
                                       &out->tokens_out);
    free(rb.data);
    if (!text) {
        out->err_msg = strdup("noretry: anthropic response shape unexpected");
        return;
    }
    /* Cost from host-supplied rates (USD/Mtok) -> cents. Left at 0 when
     * the host did not configure rates. */
    if (cfg->price_in_per_mtok > 0.0 || cfg->price_out_per_mtok > 0.0) {
        double usd = ((double)out->tokens_in  * cfg->price_in_per_mtok
                    + (double)out->tokens_out * cfg->price_out_per_mtok)
                     / 1000000.0;
        out->cost_cents = usd * 100.0;
    }
    out->response_json = text;
}


static char *
anthropic_invoke(const char *model, const char *request_json,
                 char **err_msg, void *user_ctx)
{
    flowd_adapter_response_t r = {0};
    do_invoke(model, request_json, user_ctx, &r);
    if (r.response_json == NULL) {
        if (err_msg) *err_msg = r.err_msg;
        else         free(r.err_msg);
        return NULL;
    }
    return r.response_json;
}


static void
anthropic_invoke_with_metrics(const char *model, const char *request_json,
                              flowd_adapter_response_t *result,
                              void *user_ctx)
{
    do_invoke(model, request_json, user_ctx, result);
}


flowd_provider_adapter_t
anthropic_adapter(const anthropic_adapter_config_t *cfg)
{
    flowd_provider_adapter_t a;
    a.provider_name        = anthropic_provider_name;
    a.supports_model       = anthropic_supports_model;
    a.invoke               = anthropic_invoke;
    a.invoke_with_metrics  = anthropic_invoke_with_metrics;
    a.user_ctx             = (void *)cfg;
    return a;
}
