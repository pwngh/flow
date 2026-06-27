/* src/check.h
 *
 * Type checking.  Bottom-up inference plus constraint checks at every
 * boundary (call arguments, constructor fields, pipeline source,
 * stage internals, flow return).
 *
 * Diagnostics emitted (several codes below cover more than one
 * related condition):
 *
 *   E128  unknown variant in a sum type
 *   E140  type mismatch (flow return, field access, comparison, general)
 *   E141  type disagreement across positions, or missing argument
 *         (list elements, conditional/try branches, match arms)
 *   E142  argument does not name a field / too many positional args /
 *         binary-operand type mismatch
 *   E143  duplicate argument, or predicate not of type bool
 *   E144  argument type incompatible with field type
 *   E145  empty list literal `[]` needs a type from context
 *   E146  equality not admitted for the type (only primitives compare)
 *   E149  operand/field fails an operator's type requirement
 *         (numeric, bool, string, or record element)
 *   E150  pipeline applied to a non-list value
 *   E151  `where` predicate references unknown field
 *   E152  `rank` clause references unknown field
 *   E153  `where` predicate literal incompatible with field type
 *   E154  terminal stage appears before the end of the pipeline
 *   E155  flow-scope name collides with element field inside a stage
 *   E160  non-exhaustive match (uncovered variant)
 *   E161  match scrutinee is not a sum type
 *   E162  duplicate variant in match arms
 *
 * The checker consumes a Resolved (from resolve.c) and produces a
 * Checked.  The Checked carries the inferred type of every flow
 * local (parallel to ResolveLocals.names) and the inferred return
 * type of each flow.  This is the input to the IR emitter
 * (step 5).
 *
 * All storage is arena-allocated; lifetime matches the arena's.
 */

#ifndef FLOWC_CHECK_H
#define FLOWC_CHECK_H

#include <stdio.h>

#include "ast.h"
#include "resolve.h"
#include "util.h"

typedef struct {
    /* types[i] is the inferred type of resolved->flow_locals[k].names[i]
     * for the flow at index k in resolved->globals.flows.  May be
     * NULL if the local could not be typed (because of an earlier
     * error in its initializer). */
    const Type **types;
    size_t       n;

    /* Inferred type of the flow's return_expr.  NULL when the
     * return expression could not be typed, which means a prior
     * diagnostic was already emitted for it (mirrors the per-local
     * NULL convention above). */
    const Type *return_type;
} CheckedFlow;

typedef struct {
    Resolved     *resolved;
    CheckedFlow **flows;       /* parallel to resolved->globals.flows */
    size_t        n_flows;
} Checked;

/* Run the type checker on a resolved program. Diagnostics are
 * emitted through `diag`; consult diag_error_count(diag) afterward
 * to determine success. The Checked struct is always returned
 * non-NULL; on errors, some type slots may be NULL. */
Checked *check_run(Arena *arena, DiagStream *diag, Resolved *resolved);

/* Dump the checked program in the canonical golden-test format
 * documented at the top of src/check.c. */
void check_dump(FILE *out, Arena *arena, const Checked *c);

#endif /* FLOWC_CHECK_H */
