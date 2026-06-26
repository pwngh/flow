/* src/stub_host.c
 *
 * Implementation of the type-directed stub host (see stub_host.h). Shared by
 * the `flowd run` CLI and the WASM playground so both stub tools identically.
 */
#include "stub_host.h"

#include <stdlib.h>
#include <string.h>

#include "cjson/cJSON.h"
#include "runtime_internal.h"   /* flowd_runtime::arena */
#include "util.h"               /* Arena, arena_alloc, arena_strdup */

/* Build a default JSON value (as a cJSON node) for a declared type, so a
 * stubbed tool can return something that type-checks. Recurses through
 * records/sums/lists; the depth cap guards pathological/self-referential
 * types. */
static cJSON *
default_value_for(const type_registry_t *types, type_id_t id, int depth)
{
    if (depth > 64) return cJSON_CreateNull();
    const type_t *t = type_registry_get(types, id);
    if (t == NULL) return cJSON_CreateNull();

    switch (t->kind) {
        case TYPE_KIND_PRIMITIVE:
            switch (id) {
                case TYPE_ID_STRING: return cJSON_CreateString("");
                case TYPE_ID_INT:    return cJSON_CreateNumber(0);
                case TYPE_ID_FLOAT:  return cJSON_CreateNumber(0);
                case TYPE_ID_BOOL:   return cJSON_CreateBool(0);
                default:             return cJSON_CreateNull();
            }
        case TYPE_KIND_LIST: {
            /* One element near the surface so pipeline stages (pick, top,
             * rank, aggregations) have data to operate on; empty once nesting
             * gets deep, which also stops recursion on self-referential
             * element types. */
            cJSON *arr = cJSON_CreateArray();
            if (depth < 4)
                cJSON_AddItemToArray(arr,
                    default_value_for(types, t->elem, depth + 1));
            return arr;
        }
        case TYPE_KIND_RECORD: {
            cJSON *o = cJSON_CreateObject();
            for (size_t i = 0; i < t->n_fields; i++)
                cJSON_AddItemToObject(o, t->fields[i].name,
                    default_value_for(types, t->fields[i].type, depth + 1));
            return o;
        }
        case TYPE_KIND_SUM: {
            cJSON *o = cJSON_CreateObject();
            cJSON *fields = cJSON_CreateObject();
            if (t->n_variants > 0) {
                const type_variant_t *v = &t->variants[0];
                cJSON_AddStringToObject(o, "variant", v->name);
                for (size_t i = 0; i < v->n_fields; i++)
                    cJSON_AddItemToObject(fields, v->fields[i].name,
                        default_value_for(types, v->fields[i].type, depth + 1));
            } else {
                cJSON_AddStringToObject(o, "variant", "");
            }
            cJSON_AddItemToObject(o, "fields", fields);
            return o;
        }
    }
    return cJSON_CreateNull();
}

static char *
default_value_json(const type_registry_t *types, type_id_t id)
{
    cJSON *v = default_value_for(types, id, 0);
    char *s = cJSON_PrintUnformatted(v);
    cJSON_Delete(v);
    return s;
}

/* The shared stub: a tool's pre-rendered default output rides in user_ctx;
 * return a copy (the runtime frees it). Args are ignored — stubs are pure. A
 * NULL ctx (default-JSON allocation failed at install) degrades to a JSON
 * null rather than crashing. */
static char *
stub_impl(const char *args_json, char **err_json, void *user_ctx)
{
    (void)args_json; (void)err_json;
    const char *def = user_ctx ? (const char *)user_ctx : "null";
    size_t n = strlen(def) + 1u;
    char *out = malloc(n);
    if (out) memcpy(out, def, n);
    return out;
}

/* The stub gateway serves every model call during a stubbed run. Two shapes:
 *   - a model-effect tool wants a value of its declared output type; we find
 *     the tool by its model id and return that type's default;
 *   - `pick using <model>` sends {"candidates":[...]} and wants {"index":N};
 *     no tool carries the id, so we fall through and pick the first. */
typedef struct { flowd_runtime *rt; const type_registry_t *types; } stub_gw_ctx;

static const char *stub_gw_name(void) { return "flowd-stub"; }

static int
stub_gw_supports(const char *model_id, void *ctx)
{
    (void)model_id; (void)ctx;
    return 1;
}

static char *
stub_gw_invoke(const char *model_id, const char *req_json,
               char **err_msg, void *ctx)
{
    (void)req_json; (void)err_msg;
    stub_gw_ctx *c = (stub_gw_ctx *)ctx;
    size_t nt = flowd_tool_count(c->rt);
    for (size_t i = 0; i < nt; i++) {
        const tool_t *t = flowd_tool_at(c->rt, i);
        if (t->level == EFFECT_MODEL && t->model_id != NULL
            && strcmp(t->model_id, model_id) == 0)
            return default_value_json(c->types, t->output);
    }
    char *s = malloc(12u);
    if (s) memcpy(s, "{\"index\":0}", 12u);
    return s;
}

void
flowd_install_stub_host(flowd_runtime *rt)
{
    const type_registry_t *types = flowd_types(rt);
    Arena *arena = rt->arena;

    /* Non-model tools: a deterministic default of the declared output type.
     * The default string is copied into the runtime arena so it outlives the
     * run with no caller-side bookkeeping. */
    size_t nt = flowd_tool_count(rt);
    for (size_t i = 0; i < nt; i++) {
        const tool_t *t = flowd_tool_at(rt, i);
        if (t->level == EFFECT_MODEL) continue;
        char *def = default_value_json(types, t->output);
        char *owned = def ? arena_strdup(arena, def) : NULL;
        free(def);
        flowd_effect_level lvl =
            t->level == EFFECT_PURE          ? FLOWD_EFFECT_PURE :
            t->level == EFFECT_DETERMINISTIC ? FLOWD_EFFECT_DETERMINISTIC :
                                               FLOWD_EFFECT_MUTATION;
        flowd_register_tool(rt, t->name, lvl, "", stub_impl, "stub", owned);
    }

    /* Model tools + `pick` selectors: one stub gateway adapter. Its context
     * and the adapter struct are arena-owned because register_provider keeps
     * the pointer, not a copy. */
    stub_gw_ctx *ctx = arena_alloc(arena, sizeof *ctx);
    ctx->rt    = rt;
    ctx->types = types;
    flowd_provider_adapter_t *gw = arena_alloc(arena, sizeof *gw);
    gw->provider_name       = stub_gw_name;
    gw->supports_model      = stub_gw_supports;
    gw->invoke              = stub_gw_invoke;
    gw->invoke_with_metrics = NULL;
    gw->user_ctx            = ctx;
    flowd_register_provider(rt, gw);
}

char *
flowd_stub_default_for_tool(flowd_runtime *rt, const char *tool_name)
{
    const type_registry_t *types = flowd_types(rt);
    size_t nt = flowd_tool_count(rt);
    for (size_t i = 0; i < nt; i++) {
        const tool_t *t = flowd_tool_at(rt, i);
        if (strcmp(t->name, tool_name) == 0)
            return default_value_json(types, t->output);
    }
    return NULL;
}

char *
flowd_default_input_json(flowd_runtime *rt, const flow_t *flow)
{
    const type_registry_t *types = flowd_types(rt);
    if (flow->n_params == 1 && flow->params[0].implicit)
        return default_value_json(types, flow->params[0].type);

    cJSON *o = cJSON_CreateObject();
    for (size_t i = 0; i < flow->n_params; i++)
        cJSON_AddItemToObject(o, flow->params[i].name,
            default_value_for(types, flow->params[i].type, 0));
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}
