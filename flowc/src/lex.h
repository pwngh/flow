/* src/lex.h
 *
 * Public interface to the lexer defined in src/lex.l.
 *
 * The lexer is a single instance per process. Open it with
 * lex_open(), pull tokens with lex_token() until it returns 0
 * (end of file), then call lex_close(). Token kind values are
 * defined by bison in src/parse.tab.h; consumers of the lexer
 * include that header to name them.
 *
 * The lexer does not own the FILE * it reads from or the Arena it
 * allocates lexeme storage in. Both are borrowed for the lifetime
 * of the open lexer, and the caller is responsible for closing
 * the file and destroying the arena afterward.
 *
 * The semantic value of the most recently produced token is
 * available in the flex/bison global yylval, which has type
 * YYSTYPE (defined in parse.tab.h). The source location of the
 * most recently produced token is available via lex_last_loc().
 */

#ifndef FLOWC_LEX_H
#define FLOWC_LEX_H

#include <stdio.h>

#include "util.h"

/* Begin lexing from `fp`. `filename` is recorded in every SrcLoc
 * the lexer emits; it is borrowed and must outlive the lexer.
 * `arena` is used to allocate lexeme storage (identifier and
 * string-literal text); `diag` receives
 * any error diagnostics the lexer emits. Both are borrowed for the
 * lifetime of the open lexer. */
void lex_open(FILE *fp, const char *filename, Arena *arena, DiagStream *diag);

/* Return the next token, or 0 at end of file. The token's
 * semantic value (if any) is in the global yylval; its source
 * location is available via lex_last_loc(). */
int lex_token(void);

/* The source location of the most recently produced token. After
 * lex_token() returns 0, the location points one past the last
 * consumed character. */
SrcLoc lex_last_loc(void);

/* Release the lexer's references to the file and arena passed to
 * lex_open(). Does not close the file or destroy the arena. */
void lex_close(void);

#endif /* FLOWC_LEX_H */
