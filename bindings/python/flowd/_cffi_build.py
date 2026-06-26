"""cffi build script for the flowd Python binding (API/static mode).

Static-links the existing ``flowd/libflowd.a`` directly into the
compiled extension ``flowd._flowd`` — no shared ``libflowd`` and no
``FLOWD_LIB`` dlopen lookup. Run standalone to build in place::

    python -m flowd._cffi_build

or let ``pip install`` drive it via ``cffi_modules`` (see setup.py).

Paths default to the monorepo layout (``../../flowd`` relative to
this file) and are overridable for out-of-tree builds:

    FLOWD_LIB            path to libflowd.a   (default <repo>/flowd/libflowd.a)
    FLOWD_SRC           dir holding flowd.h   (default <repo>/flowd/src)
    FLOWD_OPENSSL_LIBDIR  libcrypto lib dir   (default Homebrew openssl@3)
    FLOWD_CURL_LIBDIR     libcurl lib dir     (default Homebrew curl, optional)

libflowd.a references libcrypto (SHA256) and libcurl (the model
gateway's HTTP adapters), so both are linked.
"""

import os

from cffi import FFI

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.abspath(os.path.join(_HERE, "..", "..", ".."))

FLOWD_SRC = os.environ.get("FLOWD_SRC", os.path.join(_REPO, "flowd", "src"))
LIBFLOWD_A = os.environ.get("FLOWD_LIB", os.path.join(_REPO, "flowd", "libflowd.a"))


def _libdir(env_name, brew_formula):
    d = os.environ.get(env_name)
    if d:
        return d
    for base in ("/opt/homebrew/opt", "/usr/local/opt"):
        p = os.path.join(base, brew_formula, "lib")
        if os.path.isdir(p):
            return p
    return None


_OPENSSL_LIBDIR = _libdir("FLOWD_OPENSSL_LIBDIR", "openssl@3")
_CURL_LIBDIR = _libdir("FLOWD_CURL_LIBDIR", "curl")
_LIBRARY_DIRS = [d for d in (_OPENSSL_LIBDIR, _CURL_LIBDIR) if d]

ffibuilder = FFI()

# The subset of the libflowd C ABI (flowd/src/flowd.h) the binding uses.
ffibuilder.cdef(
    """
    typedef struct flowd_runtime flowd_runtime;

    typedef enum {
        FLOWD_EFFECT_PURE          = 0,
        FLOWD_EFFECT_DETERMINISTIC = 1,
        FLOWD_EFFECT_MODEL         = 2,
        FLOWD_EFFECT_MUTATION      = 3
    } flowd_effect_level;

    typedef char *(*flowd_tool_fn)(const char *, char **, void *);
    typedef char *(*flowd_model_fn)(const char *, char **, void *);
    typedef char *(*flowd_redactor_fn)(const char *, size_t, size_t *, void *);

    typedef struct {
        char    *response_json;
        char    *err_msg;
        uint64_t tokens_in;
        uint64_t tokens_out;
        double   cost_cents;
    } flowd_adapter_response_t;

    /* Named typedefs for the adapter's function pointers so cffi.callback
       can reference them by name (inline `const` types don't parse). The
       struct layout is identical to flowd.h's inline form (all pointers). */
    typedef const char *(*flowd_provider_name_fn)(void);
    typedef int (*flowd_provider_supports_fn)(const char *, void *);
    typedef char *(*flowd_provider_invoke_fn)(const char *, const char *,
                                              char **, void *);
    typedef void (*flowd_provider_metrics_fn)(const char *, const char *,
                                              flowd_adapter_response_t *, void *);

    typedef struct {
        flowd_provider_name_fn     provider_name;
        flowd_provider_supports_fn supports_model;
        flowd_provider_invoke_fn   invoke;
        void                      *user_ctx;
        flowd_provider_metrics_fn  invoke_with_metrics;
    } flowd_provider_adapter_t;

    flowd_runtime *flowd_load_ir(const char *ir_json);

    int flowd_register_provider(flowd_runtime *,
                                const flowd_provider_adapter_t *adapter);

    int flowd_register_tool(flowd_runtime *, const char *name,
                            flowd_effect_level level, const char *signature,
                            flowd_tool_fn impl, const char *impl_version,
                            void *user_ctx);
    int flowd_register_model(flowd_runtime *, const char *name,
                             const char *signature, flowd_model_fn impl,
                             const char *impl_version, void *user_ctx);
    int flowd_set_redactor(flowd_runtime *, flowd_redactor_fn, void *user_ctx);

    char *flowd_run(flowd_runtime *, const char *input_json,
                    const char *trace_dir, char **suspension_token);
    char *flowd_run_named(flowd_runtime *, const char *flow_name,
                          const char *input_json, const char *trace_dir,
                          char **suspension_token);
    char *flowd_resume(flowd_runtime *, const char *token,
                       const char *decision_json, char **suspension_token);
    char *flowd_replay(flowd_runtime *, const char *flow_name,
                       const char *original_trace_dir,
                       const char *new_trace_dir, const char *new_model_id);
    char *flowd_last_error_json(flowd_runtime *);
    void flowd_destroy(flowd_runtime *);

    /* heap helpers (defined below) + libc free for runtime-owned returns */
    char *flowd_py_strdup(const char *s);
    char *flowd_py_memdup(const char *src, size_t n);
    void free(void *);
    """
)

# Two heap helpers so callback return values land in the SAME allocator
# the runtime free()s (a cffi-owned buffer must never be handed back).
ffibuilder.set_source(
    "flowd._flowd",
    """
    #include "flowd.h"
    #include <stdlib.h>
    #include <string.h>
    #include <stdint.h>

    char *flowd_py_strdup(const char *s) {
        if (!s) return NULL;
        size_t n = strlen(s) + 1;
        char *p = (char *)malloc(n);
        if (p) memcpy(p, s, n);
        return p;
    }
    char *flowd_py_memdup(const char *src, size_t n) {
        char *p = (char *)malloc(n ? n : 1);
        if (p && n) memcpy(p, src, n);
        return p;
    }
    """,
    include_dirs=[FLOWD_SRC],
    extra_objects=[LIBFLOWD_A],
    libraries=["crypto", "curl"],
    library_dirs=_LIBRARY_DIRS,
)


if __name__ == "__main__":
    ffibuilder.compile(verbose=True)
