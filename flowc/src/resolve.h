/* src/resolve.h
 *
 * Name resolution.  Builds the global symbol table (types, tools,
 * flows) and the per-flow local symbol table (parameters plus
 * bindings), and verifies that every identifier reference in the
 * program resolves to a declared name.  Diagnostics are emitted
 * through the diagnostic stream:
 *
 *   E120 -- redeclaration of a global name, or a binding name that
 *           collides with an earlier binding or parameter in the
 *           same flow body
 *   E122 -- a call expression names a tool or flow that does not
 *           exist
 *   E123 -- a type expression names a type that does not exist
 *   E124 -- a path expression's head segment is not in scope
 *   E125 -- a sum variant's name collides with a record type, tool,
 *           or flow (cross-role collision)
 *   E127 -- a construct expression's variant name is ambiguous
 *           between sum types
 *   E128 -- a construct expression names a variant or type that
 *           does not exist
 *   E129 -- best-effort: a self-recursive flow whose recursive call
 *           appears unconditionally (no base case)
 *   E198 -- a tool declaration lacks an effect clause
 *
 * The resolver does not walk where-predicates, rank fields, or
 * pick model names.  where/rank identifiers name fields of the
 * upstream value's element type, so they need that type and fall to
 * the type checker (step 4, E15x).  A `pick using <model>` name is
 * implementation-defined and resolved by the runtime, not here.
 *
 * The Resolved struct returned by resolve_run() carries the
 * program plus the index of resolved names.  It is the input to
 * the type checker.  All storage is arena-allocated; lifetime
 * matches the arena's.
 */

#ifndef FLOWC_RESOLVE_H
#define FLOWC_RESOLVE_H

#include <stdio.h>

#include "ast.h"
#include "util.h"


typedef struct {
    const TypeDecl **types;
    size_t           n_types;
    const ToolDecl **tools;
    size_t           n_tools;
    const FlowDecl **flows;
    size_t           n_flows;
} ResolveGlobals;


typedef struct {
    /* Names visible inside the flow body, in declaration order:
     * parameters first (including the implicit `it`), then
     * bindings in source order. */
    const char **names;
    SrcLoc      *locs;
    int         *is_binding;   /* 0 = param/it, 1 = binding */
    size_t       n;
} ResolveLocals;


typedef struct {
    Program        *program;
    ResolveGlobals  globals;

    /* One ResolveLocals per FlowDecl in globals.flows, same order. */
    ResolveLocals  *flow_locals;
    size_t          n_flow_locals;
} Resolved;


/* Build globals, walk all Type expressions, walk each flow body.
 * Diagnostics are emitted through `diag`; the Resolved struct is
 * always returned non-NULL. Consult diag_error_count(diag)
 * afterward to determine success. Storage is arena-allocated. */
Resolved *resolve_run(Arena *arena, DiagStream *diag, Program *program);


/* Lookups into the global symbol table by name. Return the matching
 * declaration, or NULL if no global of that role bears the name.
 * Linear strcmp scan, not a hash table: at v0 scale (a handful of
 * decls per file) it beats a hash on constant factors and cache
 * behaviour. Shared with the type checker. */
const TypeDecl *globals_find_type(const ResolveGlobals *g, const char *name);
const ToolDecl *globals_find_tool(const ResolveGlobals *g, const char *name);


/* Dump the resolved program in the canonical golden-test format
 * documented at the top of src/resolve.c. */
void resolve_dump(FILE *out, const Resolved *r);


#endif /* FLOWC_RESOLVE_H */
