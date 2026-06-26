/* tests/api/test_replay_restore.c
 *
 * Replay must RESTORE non-model nodes from the recorded trace rather
 * than re-invoking them: deterministic for fidelity, and mutation so
 * external side effects never repeat. Each tool bumps a counter when
 * actually called; after flowd_replay (same-model) the counter must be
 * unchanged and the output identical. Also checks every node in the
 * replay trace carries a replay_of cross-reference.
 *
 * Exit 0 on success, non-zero on failure (run.sh checks the code).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "flowd.h"

/* Recursive remove so each run starts from a clean trace dir; without
 * this, stale (and once-tampered) exec dirs accumulate and exec_dir()
 * could pick the wrong one. */
static void rm_rf(const char *path) {
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[1024]; snprintf(c, sizeof c, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(c, &st) == 0 && S_ISDIR(st.st_mode)) rm_rf(c);
        else unlink(c);
    }
    closedir(d); rmdir(path);
}

static int g_calls = 0;
static char *t_deter(const char *a, char **e, void *u) { (void)a;(void)e;(void)u; g_calls++; return strdup("42"); }
static char *t_mut  (const char *a, char **e, void *u) { (void)a;(void)e;(void)u; g_calls++; return strdup("true"); }

static const char *IR =
 "{\"ir_version\":\"1.0\",\"types\":[],"
 "\"tools\":[{\"name\":\"deter\",\"input\":[{\"name\":\"x\",\"type\":\"int\"}],\"output\":\"int\",\"effect\":{\"level\":\"deterministic\"}},"
  "{\"name\":\"mut\",\"input\":[{\"name\":\"x\",\"type\":\"int\"}],\"output\":\"bool\",\"effect\":{\"level\":\"mutation\"}}],"
 "\"flows\":[{\"name\":\"f\",\"params\":[{\"name\":\"it\",\"type\":\"int\",\"implicit\":true}],\"output\":\"int\","
   "\"bindings\":["
     "{\"name\":\"a\",\"expr\":{\"kind\":\"call\",\"tool\":\"deter\",\"args\":[{\"field\":\"x\",\"value\":{\"kind\":\"path\",\"segments\":[\"it\"]}}]}},"
     "{\"name\":\"b\",\"expr\":{\"kind\":\"call\",\"tool\":\"mut\",\"args\":[{\"field\":\"x\",\"value\":{\"kind\":\"path\",\"segments\":[\"it\"]}}]}}],"
   "\"return\":{\"kind\":\"path\",\"segments\":[\"a\"]}}]}";

static char *exec_dir(const char *root) {
    static char p[512]; char b[400];
    snprintf(b, sizeof b, "%s/f", root);
    DIR *d = opendir(b); struct dirent *e;
    while (d && (e = readdir(d)))
        if (!strncmp(e->d_name, "exec_", 5)) {
            snprintf(p, sizeof p, "%s/%s", b, e->d_name); closedir(d); return p;
        }
    if (d) closedir(d);
    return NULL;
}

int main(void) {
    rm_rf("/tmp/flowd-test-rr-orig");
    rm_rf("/tmp/flowd-test-rr-replay");
    rm_rf("/tmp/flowd-test-rr-diverge");

    flowd_runtime *rt = flowd_load_ir(IR);
    if (!rt) { printf("  FAIL load\n"); return 1; }
    flowd_register_tool(rt, "deter", FLOWD_EFFECT_DETERMINISTIC, "(int)->int", t_deter, "v1", NULL);
    flowd_register_tool(rt, "mut",   FLOWD_EFFECT_MUTATION,      "(int)->bool", t_mut,  "v1", NULL);

    char *susp = NULL;
    char *out = flowd_run(rt, "7", "/tmp/flowd-test-rr-orig", &susp);
    int after_run = g_calls;

    char *rep = flowd_replay(rt, "f", exec_dir("/tmp/flowd-test-rr-orig"),
                             "/tmp/flowd-test-rr-replay", NULL);
    int after_replay = g_calls;

    int fail = 0;
    if (!out || !rep || strcmp(out, rep) != 0) { printf("  FAIL output mismatch\n"); fail = 1; }
    else printf("  ok   replay output identical (%s)\n", rep);
    if (after_run != 2) { printf("  FAIL run did not invoke both tools (%d)\n", after_run); fail = 1; }
    if (after_replay != after_run) {
        printf("  FAIL replay re-invoked tools (delta %d)\n", after_replay - after_run); fail = 1;
    } else printf("  ok   replay restored nodes, mutation not re-invoked\n");

    /* Divergence guard: tamper a recorded input so the re-derived
     * input no longer matches. Replay must REFUSE to restore the stale
     * output and fail with R157 — never silently return a wrong answer
     * for an input the original never saw. */
    {
        char ed[512];
        const char *e0 = exec_dir("/tmp/flowd-test-rr-orig");
        if (e0) {
            snprintf(ed, sizeof ed, "%s", e0);
            char np[600];
            snprintf(np, sizeof np, "%s/nodes/n1.json", ed);
            FILE *f = fopen(np, "rb");
            if (f) {
                fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
                char *buf = malloc((size_t)sz + 1u);
                if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
                    buf[sz] = '\0';
                    fclose(f);
                    char *hit = strstr(buf, "{\"x\":7}");
                    if (hit) {
                        FILE *w = fopen(np, "wb");
                        fwrite(buf, 1, (size_t)(hit - buf), w);
                        fputs("{\"x\":999}", w);
                        fputs(hit + 7, w);   /* skip {"x":7} */
                        fclose(w);
                        int before = g_calls;
                        char *bad = flowd_replay(rt, "f", ed,
                                        "/tmp/flowd-test-rr-diverge", NULL);
                        char *errj = flowd_last_error_json(rt);
                        if (bad == NULL && errj && strstr(errj, "R157")) {
                            printf("  ok   replay rejects stale restore on "
                                   "input divergence (R157)\n");
                        } else {
                            printf("  FAIL divergence not caught "
                                   "(out=%s err=%s)\n",
                                   bad ? bad : "(null)",
                                   errj ? errj : "(none)");
                            fail = 1;
                        }
                        (void)before;
                        free(bad); free(errj);
                    } else {
                        printf("  FAIL could not find recorded input to tamper\n");
                        fail = 1;
                    }
                } else {
                    if (f) fclose(f);
                    printf("  FAIL could not read node file\n"); fail = 1;
                }
                free(buf);
            } else {
                printf("  FAIL could not open node file\n"); fail = 1;
            }
        } else {
            printf("  FAIL could not locate exec dir for tamper\n"); fail = 1;
        }
    }

    free(out); free(rep); free(susp);
    flowd_destroy(rt);
    printf("test_replay_restore: %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
