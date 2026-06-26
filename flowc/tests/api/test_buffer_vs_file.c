/* tests/api/test_buffer_vs_file.c
 *
 * Compile the canonical onboard fixture two ways — once via
 * flowc_compile_source (in-memory buffer) and once via
 * flowc_compile_file (slurped from disk by the library) — and assert
 * the IR is byte-identical.
 *
 * If this ever diverges, the fmemopen path and the slurp-and-delegate
 * path are doing different things, which would silently corrupt every
 * library caller that picks the buffer entry point over the file one
 * (or vice versa).
 *
 * SOURCE_DATE_EPOCH is pinned to 0 so the IR's compiled_at field is
 * the same across both compiles — otherwise the wall-clock between
 * the two calls would make every run a false negative.
 */

#include "api.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *FIXTURE_PATH = "tests/fixtures/onboard.flow";

static char *slurp_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "fopen(%s): %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    rewind(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    char *buf = malloc((size_t)size + 1u);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    size_t n = fread(buf, 1u, (size_t)size, fp);
    fclose(fp);
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

int main(void)
{
    /* Determinism: same compiled_at across both compiles. */
    setenv("SOURCE_DATE_EPOCH", "0", 1);

    size_t src_len = 0;
    char  *src     = slurp_file(FIXTURE_PATH, &src_len);
    if (src == NULL) {
        fprintf(stderr, "test_buffer_vs_file: cannot read %s\n", FIXTURE_PATH);
        return 2;
    }

    /* Compile via in-memory source buffer. */
    flowc_context *ctx_src = flowc_context_create();
    const char *out_src = flowc_compile_source(ctx_src, src, src_len, FIXTURE_PATH);
    if (out_src == NULL) {
        fprintf(stderr,
                "test_buffer_vs_file: flowc_compile_source returned NULL "
                "(%d errors)\n",
                flowc_error_count(ctx_src));
        flowc_context_destroy(ctx_src);
        free(src);
        return 1;
    }
    /* Copy out — the next library call invalidates out_src, and we
     * want to outlive the source context for the compare. */
    char *ir_from_src = strdup(out_src);
    flowc_context_destroy(ctx_src);
    free(src);

    /* Compile via file path (library does its own slurp). */
    flowc_context *ctx_file = flowc_context_create();
    const char *out_file = flowc_compile_file(ctx_file, FIXTURE_PATH);
    if (out_file == NULL) {
        fprintf(stderr,
                "test_buffer_vs_file: flowc_compile_file returned NULL "
                "(%d errors)\n",
                flowc_error_count(ctx_file));
        free(ir_from_src);
        flowc_context_destroy(ctx_file);
        return 1;
    }

    int rc = 0;
    if (strcmp(ir_from_src, out_file) != 0) {
        fprintf(stderr,
                "test_buffer_vs_file: IR mismatch between buffer and file paths\n"
                "--- buffer (%zu bytes) ---\n%s\n"
                "--- file   (%zu bytes) ---\n%s\n",
                strlen(ir_from_src), ir_from_src,
                strlen(out_file),    out_file);
        rc = 1;
    } else {
        printf("test_buffer_vs_file: PASS (%zu bytes of IR, byte-identical)\n",
               strlen(out_file));
    }

    /* E001: flowc_compile_file on an unreadable path surfaces a library
     * diagnostic (the CLI's exit-3 path is the file twin; E001 is the
     * in-library contract the Python/TS bindings rely on). */
    {
        flowc_context   *ctx_io  = flowc_context_create();
        const char      *out_io  = flowc_compile_file(
                                       ctx_io, "tests/fixtures/does-not-exist.flow");
        DiagStream      *d       = flowc_diag_stream(ctx_io);
        const Diagnostic *first  = diag_count(d) > 0 ? diag_at(d, 0) : NULL;
        if (out_io == NULL && first != NULL && first->id != NULL
            && strcmp(first->id, "E001") == 0) {
            printf("test_buffer_vs_file: PASS (E001 on missing file)\n");
        } else {
            fprintf(stderr,
                    "test_buffer_vs_file: E001 not reported on missing file "
                    "(out=%p, count=%zu, id=%s)\n",
                    (const void *)out_io, diag_count(d),
                    first && first->id ? first->id : "(none)");
            rc = 1;
        }
        flowc_context_destroy(ctx_io);
    }

    free(ir_from_src);
    flowc_context_destroy(ctx_file);
    return rc;
}
