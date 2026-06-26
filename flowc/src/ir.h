/* src/ir.h
 *
 * JSON Intermediate Representation emitter.
 *
 * Walks a validated AST (after resolve and check have succeeded)
 * and writes the canonical pretty-printed JSON IR to `out`.
 *
 * Determinism contract (matters for golden tests and reproducible
 * builds; see the comment at the top of src/ir.c):
 *
 *   1. The process must be running under the C locale for numeric
 *      formatting.  src/main.c installs setlocale(LC_NUMERIC, "C")
 *      at startup so the CLI inherits this.  Without it, "%g" emits
 *      the locale's decimal separator (',' in de_DE etc.) and the
 *      IR is no longer valid JSON — see rule 4.
 *
 *   2. The `compiled_at` timestamp respects the SOURCE_DATE_EPOCH
 *      environment variable when present (POSIX-friendly date).
 *      Otherwise it uses the current UTC wall time.
 *
 *   3. Field ordering is fixed by this implementation, not by hash
 *      iteration: top-level members in a fixed order,
 *      record members in the order described by their section, and
 *      collections in source order.
 *
 *   4. Floats render via "%g" which under the C locale uses '.' as
 *      the decimal separator and (for the values that appear in
 *      well-formed Flow source) produces stable representations.
 *
 * The emitter does not consult the type checker's Checked struct.
 * Only the AST is needed; the validation gate is the caller's
 * responsibility (the pipeline in api.c only emits after a clean
 * resolve + check).
 */

#ifndef FLOWC_IR_H
#define FLOWC_IR_H

#include <stdio.h>

#include "ast.h"
#include "util.h"

void ir_emit(FILE *out, Arena *arena, const Program *program);

#endif /* FLOWC_IR_H */
