/* src/parse.h
 *
 * Public interface to the parser defined in src/parse.y.
 *
 * The parser is non-reentrant: bison's yyparse and the parser's own
 * state live in file-scope statics (see parse.y), so only one parse
 * can be in flight at a time, mirroring the lexer in lex.h.  Three
 * entry points control its lifetime:
 *
 *   parse_open()   seeds the arena and source filename used by
 *                  semantic actions and yyerror diagnostics.
 *   parse_run()    invokes yyparse.  Returns 0 on success, 1 on
 *                  syntax error, 2 on memory exhaustion.
 *   parse_close()  retrieves the completed Program* and clears
 *                  the parser's static state.  Returns NULL if
 *                  the parse failed or was never run.
 *
 * The parser pulls tokens from the global lexer (lex.h); the caller
 * must start the lexer with lex_open() before calling parse_run()
 * and stop it with lex_close() after.
 *
 * Syntax errors are emitted through the diagnostic stream as E110.
 * The driver should consult diag_error_count() after parse_run() to
 * determine whether the returned Program is well-formed.
 */

#ifndef FLOWC_PARSE_H
#define FLOWC_PARSE_H

#include "ast.h"
#include "util.h"

void     parse_open(Arena *arena, const char *source_file, DiagStream *diag);
int      parse_run(void);
Program *parse_close(void);

#endif /* FLOWC_PARSE_H */
