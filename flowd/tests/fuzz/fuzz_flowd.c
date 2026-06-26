/* tests/fuzz/fuzz_flowd.c
 *
 * libFuzzer harness for the flowd IR loader: feeds arbitrary bytes to
 * the public flowd_load_ir, which parses untrusted IR JSON. Catches
 * crashes/UB on malformed input.
 *
 * Build (clang with libFuzzer + ASan):
 *   CC=clang CFLAGS='-fsanitize=address,fuzzer-no-link -g -O1' \
 *       ./configure && make libflowd.a
 *   clang -fsanitize=address,fuzzer -I src tests/fuzz/fuzz_flowd.c \
 *       libflowd.a -lcrypto -lcurl -o fuzz_flowd
 *   ./fuzz_flowd <corpus-dir> -max_total_time=60
 *
 * Seed the corpus from flowd/tests/corpus/*.ir.json and examples/*.ir.json.
 * Keep the empty input and "{" as permanent seeds: they once triggered a
 * heap-use-after-free in the loader's error path (now fixed in
 * src/ir_load.c; the cheap regression lives in tests/api/test_exec.c).
 */
#include "flowd.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    char *s = malloc(size + 1u);
    if (!s) return 0;
    memcpy(s, data, size);
    s[size] = '\0';
    flowd_runtime *rt = flowd_load_ir(s);
    if (rt) flowd_destroy(rt);
    free(s);
    return 0;
}
