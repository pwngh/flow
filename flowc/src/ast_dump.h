/* src/ast_dump.h
 *
 * Pretty-print the AST in a stable, golden-testable form.
 *
 * The dump format is the canonical interface for --dump=ast and is
 * relied upon by tests/fixtures/.  See the comment at the top of
 * src/ast_dump.c for a description; changes there must come with
 * updated .expected.out fixtures.
 *
 * The dumper is read-only over the AST.  It allocates from the
 * provided arena (for canonical type strings via ast_type_to_string)
 * but never modifies any AST node.
 */

#ifndef FLOWC_AST_DUMP_H
#define FLOWC_AST_DUMP_H

#include <stdio.h>

#include "ast.h"
#include "util.h"

void ast_dump(FILE *out, Arena *arena, const Program *program);

#endif /* FLOWC_AST_DUMP_H */
