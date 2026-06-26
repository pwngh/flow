/* tests/fuzz/fuzz_flowc.c
 *
 * libFuzzer harness for the flowc front-end: feeds arbitrary bytes
 * through the full lex -> parse -> resolve -> check -> emit pipeline via
 * the public library entry point, catching crashes/UB when the compiler
 * is given untrusted .flow source.
 *
 * Build (clang with libFuzzer + ASan):
 *   CC=clang CFLAGS='-fsanitize=address,fuzzer-no-link -g -O1' \
 *       ./configure && make libflowc.a
 *   clang -fsanitize=address,fuzzer -I src \
 *       tests/fuzz/fuzz_flowc.c libflowc.a -o fuzz_flowc
 *   ./fuzz_flowc <corpus-dir> -max_total_time=60
 *
 * Seed the corpus from examples/*.flow and flowc/tests/fixtures/*.flow.
 * Keep the empty 0-byte input as a permanent seed: it once aborted the
 * process via fmemopen(len=0) -> ICE (now fixed in src/api.c).
 */
#include "api.h"

#include <stddef.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    flowc_context *ctx = flowc_context_create();
    flowc_compile_source(ctx, (const char *)data, size, "fuzz");
    flowc_context_destroy(ctx);
    return 0;
}
