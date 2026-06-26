/* tests/api/test_reset_cycle.c
 *
 * Compile the canonical onboard fixture 100 times in a row, calling
 * flowc_context_reset between each, and assert that peak resident
 * memory (ru_maxrss) does not grow unboundedly.
 *
 * The property under test: flowc_context_reset actually frees the
 * per-context arena and DiagStream. Without it, the long-running
 * consumers of libflowc (the eventual LSP server, watch-mode host
 * bindings) would accumulate ~100 KiB of garbage per compile and
 * exhaust memory in minutes. A silent failure of reset (no-op or
 * partial free) is hard to catch any other way — the next compile
 * still works, just leaks. Hence the explicit RSS bound.
 *
 * Why ru_maxrss and not a current-RSS query: getrusage is portable
 * across macOS, Linux, the BSDs, and Solaris with the same API.
 * /proc/self/status is Linux-only, and Mach task_info is macOS-only.
 * ru_maxrss is a peak gauge — if reset works, the peak after warmup
 * holds; if reset leaks, the peak climbs.
 *
 * Units of ru_maxrss differ by platform:
 *   - Linux: kibibytes (1024 bytes)
 *   - macOS / BSD / Solaris: bytes
 * The bound is set generously enough (5 MB equivalent) that the
 * unit difference does not matter for the gross-leak detection
 * this test performs.
 *
 * Feature-test macro caveat: ru_maxrss is a BSD extension hidden
 * under strict _POSIX_C_SOURCE. We opt in to the BSD/glibc views of
 * sys/resource.h before the system headers see the build's
 * _POSIX_C_SOURCE=200809L. This is the only translation unit in
 * the project that touches a non-POSIX field, so the leak is
 * contained to the test harness.
 */

#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE 1
#elif defined(__GLIBC__) || defined(__linux__)
#  define _DEFAULT_SOURCE 1
#  define _BSD_SOURCE 1     /* older glibc */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/time.h>

#include "api.h"
#include "util.h"

#define ITERATIONS  100
#define WARMUP      5

#ifdef __linux__
/* ru_maxrss is in KiB on Linux; the bound is 5 MB = 5 * 1024 KiB. */
#  define RSS_GROWTH_BOUND  (5L * 1024L)
#else
/* ru_maxrss is in bytes on macOS/BSD; the bound is 5 MB. */
#  define RSS_GROWTH_BOUND  (5L * 1024L * 1024L)
#endif

static const char *FIXTURE_PATH = "tests/fixtures/onboard.flow";

static long peak_rss(void)
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
        perror("getrusage");
        return -1;
    }
    return (long)ru.ru_maxrss;
}

int main(void)
{
    /* Determinism: pin compiled_at so each compile is identical work
     * (saves a few cycles of ctime each iteration but mostly removes
     * a noise source if the test ever starts asserting on cycle
     * counts too). */
    setenv("SOURCE_DATE_EPOCH", "0", 1);

    flowc_context *ctx = flowc_context_create();

    /* Warmup: a few compiles to amortize one-time costs (initial
     * malloc heap layout, libc lazy bindings, etc.) so the baseline
     * we capture below reflects the steady state, not the cold start. */
    for (int i = 0; i < WARMUP; i++) {
        const char *out = flowc_compile_file(ctx, FIXTURE_PATH);
        if (out == NULL) {
            fprintf(stderr,
                    "test_reset_cycle: warmup compile %d failed (%d errors)\n",
                    i, flowc_error_count(ctx));
            flowc_context_destroy(ctx);
            return 1;
        }
        flowc_context_reset(ctx);
    }

    long rss_start = peak_rss();

    for (int i = 0; i < ITERATIONS; i++) {
        const char *out = flowc_compile_file(ctx, FIXTURE_PATH);
        if (out == NULL) {
            fprintf(stderr,
                    "test_reset_cycle: compile %d failed (%d errors)\n",
                    i, flowc_error_count(ctx));
            flowc_context_destroy(ctx);
            return 1;
        }
        flowc_context_reset(ctx);
    }

    long rss_end = peak_rss();
    flowc_context_destroy(ctx);

    long growth = rss_end - rss_start;
    if (growth > RSS_GROWTH_BOUND) {
        fprintf(stderr,
                "test_reset_cycle: peak RSS grew by %ld over %d iterations "
                "(bound = %ld); flowc_context_reset is leaking\n",
                growth, ITERATIONS, (long)RSS_GROWTH_BOUND);
        return 1;
    }

    printf("test_reset_cycle: PASS (%d iterations, RSS growth = %ld, "
           "bound = %ld)\n",
           ITERATIONS, growth, (long)RSS_GROWTH_BOUND);
    return 0;
}
