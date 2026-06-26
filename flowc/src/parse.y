/* src/parse.y
 *
 * Grammar for the Flow language.
 *
 * Build invocation (from the Makefile):
 *
 *     bison -d -o src/parse.tab.c src/parse.y
 *
 * The -d flag emits src/parse.tab.h. The header declares the token
 * enumeration, the YYSTYPE union, the YYLTYPE struct, and the
 * external yylval and yylloc that the lexer (src/lex.l) updates.
 *
 *
 * Discipline
 * ----------
 *
 *   - Pinned shift/reduce conflicts. The %expect 23 directive pins
 *     the conflict count: bison fails the build if the grammar has
 *     more or fewer shift/reduce conflicts than the 23 documented
 *     dangling-else / dangling-then cases (see the comment at the
 *     directive). If
 *     you change the grammar and the count moves, account for every
 *     new conflict before touching the number; do not raise it to
 *     silence a conflict you cannot explain. %prec can resolve a
 *     conflict cleanly when the resolution is a real precedence
 *     call (associativity, prefix-vs-binary `-`); reach for it
 *     then, not as a way to silence a conflict caused by an
 *     ambiguous production shape.
 *
 *   - Explicit precedence. The and/or precedence in predicates is
 *     encoded in the grammar shape (predicate / and_pred / cmp_expr)
 *     rather than via %left declarations. A reader can see what
 *     binds tighter without consulting a precedence table.
 *
 *   - Pipelines are restricted to where they make sense: the
 *     right-hand side of a binding and the return expression. The
 *     grammar uses two non-terminals (`expr` and `binding_rhs`)
 *     where `binding_rhs` extends `expr` with the pipeline form.
 *     Inside argument lists only plain `expr` is accepted, which
 *     rules out the confusing `f(g(x) | where ...)` shape without
 *     a special diagnostic.
 *
 *   - Shorthand vs. positional arguments. The grammar accepts both
 *     `name: expr` and bare `expr`. Both produce Arg{field, value}
 *     with field set or NULL; the resolver decides which the
 *     parameter list of the called tool allows. The grammar does
 *     not try to be smart here.
 *
 *   - Source locations. %locations is enabled with a custom YYLTYPE
 *     = SrcLoc. The lexer sets yylloc after every match. Semantic
 *     actions read @$ for the current rule's location, populated
 *     by our YYLLOC_DEFAULT (below): we take the leftmost
 *     component's location verbatim rather than bison's default
 *     full-extent span. Diagnostics therefore point at the start
 *     of a construct, not its closing brace.
 *
 *   - Diagnostics. The grammar reports a single error, E110, on any
 *     syntactic mismatch and stops. We do not run bison's error
 *     recovery: once the parser is desynchronized the subsequent
 *     diagnostics describe parser-state confusion rather than the
 *     user's mistake, and the cascade obscures the real error.
 *     E110 is reserved here.
 *
 *
 * Reentrancy
 * ----------
 *
 * This grammar is intentionally not reentrant. The compiler runs one
 * source through the pipeline per invocation. If flowc is ever
 * embedded as a library, converting to %define api.pure full is
 * mechanical and localized to this file plus src/lex.l.
 */

%require "3.0"

%{

#include <stdio.h>
#include <string.h>

#include "ast.h"
#include "util.h"


/* ---------------------------------------------------------------
 * Module-private state owned by the parser
 *
 * The parser receives the Arena (used for AST allocation) and the
 * source filename (used by yyerror for diagnostics) from
 * parse_open(). The completed program is captured by parse_close().
 *
 * Per the parser-doesn't-need-reentrancy discipline above, these
 * are file-scope statics. Touching them from outside this
 * translation unit is a bug.
 * --------------------------------------------------------------- */

static Arena      *g_arena;
static const char *g_source_file;
static Program    *g_program;       /* set by the top-level reduction */

/* The diagnostic stream yyerror writes through. Borrowed; set by
 * parse_open and cleared by parse_close. Matches the lexer's
 * non-reentrant g_diag convention. */
static DiagStream *g_diag;

%}


/* ====================================================================
 * Parser configuration
 *
 * %locations enables YYLTYPE tracking. Combined with YYLTYPE_IS_DECLARED
 * below, this routes locations through our SrcLoc rather than the
 * default 4-field struct.
 *
 * %expect 23 pins the shift/reduce conflict count: bison fails the
 * build if the actual count deviates from 23, so a new conflict
 * cannot slip in unnoticed. Reduce/reduce conflicts are never
 * expected and always fail the build when %expect is given.
 *
 * Single-character tokens appear in productions as their literal
 * character ('{' rather than LBRACE); that is standard bison
 * behavior for character tokens, no %define needed.
 * ==================================================================== */

%locations
/* The 23 expected shift/reduce conflicts are the dangling-else /
 * dangling-then ambiguities introduced when conditional / match /
 * try expressions sit in primary_expr while operators (and / or /
 * cmp / +-* etc.) live at outer precedence levels. Default-shift
 * resolution gives the "branch absorbs the longest expression"
 * reading (`if X then 1 else 2 + 3` → `if X then 1 else (2 + 3)`)
 * which matches user intuition and keeps the grammar small. Users
 * who want the alternative binding parenthesise the conditional:
 * `(if X then 1 else 2) + 3`. */
%expect 23

%code requires {
    #include "ast.h"
    #include "util.h"

    /* Custom location type. SrcLoc has the (file, line, column)
     * triple we want; bison's default 4-field YYLTYPE doesn't carry
     * filenames at all. */
    typedef SrcLoc YYLTYPE;
    #define YYLTYPE_IS_DECLARED 1

    /* Use the leftmost component's location verbatim (not bison's
     * full-extent span) so diagnostics point at the start of a
     * construct; zero it when there are no components. */
    #define YYLLOC_DEFAULT(Current, Rhs, N)                 \
        do {                                                \
            if (N) {                                        \
                (Current) = YYRHSLOC(Rhs, 1);               \
            } else {                                        \
                (Current).file   = NULL;                    \
                (Current).line   = 0;                       \
                (Current).column = 0;                       \
            }                                               \
        } while (0)
}


/* ====================================================================
 * Forward declarations
 *
 * yylex is in src/lex.yy.c (generated from src/lex.l).
 * yyerror is defined at the bottom of this file.
 * ==================================================================== */

%code {
    int  yylex(void);
    void yyerror(const char *msg);

    /* Convenience: wrap a finalized AstList into a typed (array,
     * count) pair. The cast is unavoidable because AstList stores
     * void** and AST arrays are arrays of typed pointers. The macro
     * makes the call sites uniform. */
    #define FINALIZE(LST, ARR_T, ARR, N)                    \
        do {                                                \
            (N)   = (LST).n;                                \
            (ARR) = (ARR_T)ast_list_finalize(&(LST), g_arena); \
        } while (0)
}


/* ====================================================================
 * Semantic value types
 *
 * The original three members (str, int_val, float_val) for raw
 * lexer values stay. Added: pointers to every AST node kind a rule
 * can synthesize, plus AstList* for scratch lists that accumulate
 * homogeneous children.
 *
 * AstList is held by pointer, not value: bison copies YYSTYPE values
 * around its parse stack, and the AstList struct contains a malloc'd
 * buffer that must not be shared between copies. Holding by pointer
 * means the (arena-allocated) AstList header is shared but the
 * pointer itself is trivially copyable.
 * ==================================================================== */

%union {
    /* Lexer values */
    char     *str;
    long      int_val;
    double    float_val;

    /* AST nodes */
    Type          *type;
    Field         *field;
    Variant       *variant;
    Param         *param;
    Arg           *arg;
    Expr          *expr;
    Stage         *stage;
    Predicate     *predicate;
    CmpExpr       *cmp;
    Binding       *binding;
    EffectClause  *effect_clause;
    MatchArm      *match_arm;
    TypeDecl      *type_decl;
    ToolDecl      *tool_decl;
    FlowDecl      *flow_decl;
    Decl          *decl;

    /* Patterns travel by value (small POD). */
    Pattern        pattern;

    /* Scratch lists. Each is a pointer to an arena-allocated AstList
     * header. See note above about why we hold by pointer. */
    AstList   *list;

    /* Enum values that semantic actions need to thread through */
    int          sort_dir;       /* SortDir */
    int          cmp_op;         /* CmpOp   */
    int          binop_op;       /* BinopOp */
    int          effect_level;   /* EffectLevel */

    /* Retry policy travels through the parse stack as a value;
     * the AST stores it by value on EffectClause. */
    RetryPolicy  retry;
}


/* ====================================================================
 * Tokens
 *
 * Identifiers and literals come first because they carry semantic
 * values; the union member they bind to is named in the <...>
 * annotation.
 * ==================================================================== */

%token <str>       IDENT
%token <int_val>   INT_LIT
%token <float_val> FLOAT_LIT
%token <str>       STRING_LIT


/* Reserved words for v1, alphabetical. */

%token KW_ALL
%token KW_AND
%token KW_ANY
%token KW_ASC
%token KW_BACKOFF
%token KW_BOOL
%token KW_BY
%token KW_CONCAT
%token KW_COUNT
%token KW_DEDUPE
%token KW_DESC
%token KW_DETERMINISTIC
%token KW_EFFECT
%token KW_ELSE
%token KW_FACTOR
%token KW_FALSE
%token KW_FLOAT
%token KW_FLOW
%token KW_FOREVER
%token KW_IF
%token KW_INITIAL
%token KW_INT
%token KW_IT
%token KW_MATCH
%token KW_MAX
%token KW_MIN
%token KW_MODEL
%token KW_MUTATION
%token KW_NOT
%token KW_OR
%token KW_PICK
%token KW_PURE
%token KW_RANK
%token KW_RETRY
%token KW_ROW
%token KW_SELECT
%token KW_STRING
%token KW_SUM
%token KW_TAKE
%token KW_THEN
%token KW_TOOL
%token KW_TOP
%token KW_TRUE
%token KW_TRY
%token KW_TYPE
%token KW_USING
%token KW_WHERE

/* Multi-character punctuation; single-character punctuation is
 * returned by the lexer as its ASCII code. */

%token ARROW       /* -> */
%token LE          /* <= */
%token GE          /* >= */
%token EQEQ        /* == */
%token NEQ         /* != */


/* ====================================================================
 * Typed non-terminals
 *
 * Every non-terminal whose semantic value is used by another rule
 * needs a %type so bison knows which union member to read from $$
 * and the various $N.
 * ==================================================================== */

%type <type>        type
%type <field>       field
%type <variant>     variant
%type <param>       param
%type <arg>         arg
%type <expr>        expr binding_rhs primary_expr
%type <expr>        match_expr conditional_expr try_expr path_expr list_lit
%type <expr>        or_expr and_expr not_expr cmp_expr
%type <expr>        add_expr mul_expr unary_expr pipeline_expr
%type <stage>       stage
%type <predicate>   predicate and_pred
%type <cmp>         pred_cmp_op_expr
%type <binding>     binding
%type <type_decl>   type_decl
%type <tool_decl>   tool_decl
%type <flow_decl>   flow_decl
%type <decl>        decl

%type <list>        decl_list
%type <list>        field_list field_list_nonempty
%type <list>        variant_list
%type <list>        param_list param_list_nonempty
%type <list>        arg_list  arg_list_nonempty
%type <list>        binding_list
%type <list>        list_elem_list

%type <sort_dir>     sort_dir
%type <cmp_op>       cmp_op
%type <binop_op>     cmp_op_b

%type <effect_clause> effect_clause_opt effect_clause
%type <effect_level>  effect_level
%type <retry>         retry_clause_opt retry_clause retry_spec

%type <match_arm>   match_arm
%type <list>        match_arm_list


/* ====================================================================
 * Destructors
 *
 * AstList scratch buffers are malloc-backed (ast.h, "Scratch lists"):
 * the arena holds only the header; the items array is reclaimed by
 * ast_list_finalize on the normal path. On a syntax error bison pops
 * the parse stack and discards in-progress semantic values, so any
 * list still under construction (a partial param_list, field_list,
 * arg_list, ...) would leak its backing buffer — arena teardown
 * cannot reclaim what is not in the arena. This destructor is the
 * ast_list_free call the contract in ast.h asks for on the error
 * path. Discarded symbols are exactly those never consumed by a
 * reduction, so a list already finalized by a completed rule is
 * never passed here.
 * ==================================================================== */

%destructor {
    if ($$ != NULL) {
        ast_list_free($$);
    }
} <list>


%start program


%%


/* ====================================================================
 * Top-level program
 * ==================================================================== */

program
    : decl_list
        {
            Decl **decls;
            size_t n;
            FINALIZE(*$1, Decl **, decls, n);
            g_program = ast_program(g_arena, g_source_file, decls, n);
        }
    ;

decl_list
    : /* empty */
        {
            $$ = arena_alloc_zero(g_arena, sizeof(AstList));
            ast_list_init($$);
        }
    | decl_list decl
        {
            ast_list_push($1, $2);
            $$ = $1;
        }
    ;

decl
    : type_decl  { $$ = ast_decl_type(g_arena, $1); }
    | tool_decl  { $$ = ast_decl_tool(g_arena, $1); }
    | flow_decl  { $$ = ast_decl_flow(g_arena, $1); }
    ;


/* ====================================================================
 * Type declarations
 *
 *     type Name = { f1: T1, f2: T2, ... }            -- record
 *     type Name = | A { ... } | B { ... }            -- sum
 *
 * The `=` is required. Sum bodies use a leading `|` for
 * every variant including the first — the form chosen for
 * readability. An empty record body parses (field_list has an empty
 * alternative); an empty sum body cannot, because variant_list has
 * no empty alternative and so requires at least one variant.
 * ==================================================================== */

type_decl
    : KW_TYPE IDENT '=' '{' field_list '}'
        {
            Field **fa;
            size_t n;
            FINALIZE(*$5, Field **, fa, n);
            $$ = ast_type_decl_record(g_arena, $2, fa, n, @1);
        }
    | KW_TYPE IDENT '=' variant_list
        {
            Variant **va;
            size_t n;
            FINALIZE(*$4, Variant **, va, n);
            $$ = ast_type_decl_sum(g_arena, $2, va, n, @1);
        }
    ;

variant_list
    : variant
        {
            $$ = arena_alloc_zero(g_arena, sizeof(AstList));
            ast_list_init($$);
            ast_list_push($$, $1);
        }
    | variant_list variant
        {
            ast_list_push($1, $2);
            $$ = $1;
        }
    ;

variant
    : '|' IDENT '{' field_list '}'
        {
            Field **fa;
            size_t n;
            FINALIZE(*$4, Field **, fa, n);
            $$ = ast_variant(g_arena, $2, fa, n, @1);
        }
    ;

field_list
    : /* empty */
        {
            $$ = arena_alloc_zero(g_arena, sizeof(AstList));
            ast_list_init($$);
        }
    | field_list_nonempty
        { $$ = $1; }
    ;

field_list_nonempty
    : field
        {
            $$ = arena_alloc_zero(g_arena, sizeof(AstList));
            ast_list_init($$);
            ast_list_push($$, $1);
        }
    | field_list_nonempty ',' field
        {
            ast_list_push($1, $3);
            $$ = $1;
        }
    ;

field
    : IDENT ':' type
        { $$ = ast_field(g_arena, $1, $3, @1); }
    ;


/* ====================================================================
 * Tool declarations
 *
 *     tool name(p1: T1, p2: T2, ...) -> ReturnType
 *       effect <level>(retry: ...)
 *
 * The effect clause is mandatory in v1. The grammar admits a
 * missing clause so the resolver can emit a single targeted E198
 * instead of a generic E110 from the parser; without that the
 * diagnostic would point at whatever-token-came-next rather than at
 * the tool itself.
 * ==================================================================== */

tool_decl
    : KW_TOOL IDENT '(' param_list ')' ARROW type effect_clause_opt
        {
            Param **pa;
            size_t n;
            FINALIZE(*$4, Param **, pa, n);
            $$ = ast_tool_decl(g_arena, $2, pa, n, $7, $8, @1);
        }
    ;

effect_clause_opt
    : /* empty */            { $$ = NULL; }
    | effect_clause          { $$ = $1;   }
    ;

effect_clause
    : KW_EFFECT effect_level retry_clause_opt
        {
            $$ = ast_effect_clause(g_arena, (EffectLevel)$2, NULL, $3, @1);
        }
    | KW_EFFECT KW_MODEL '(' STRING_LIT ')' retry_clause_opt
        {
            $$ = ast_effect_clause(g_arena, EFFECT_MODEL, $4, $6, @1);
        }
    ;

effect_level
    : KW_PURE          { $$ = (int)EFFECT_PURE;          }
    | KW_DETERMINISTIC { $$ = (int)EFFECT_DETERMINISTIC; }
    | KW_MUTATION      { $$ = (int)EFFECT_MUTATION;      }
    ;

retry_clause_opt
    : /* empty */            { RetryPolicy r = {0}; r.kind = RETRY_DEFAULT; $$ = r; }
    | retry_clause           { $$ = $1; }
    ;

retry_clause
    : '(' KW_RETRY ':' retry_spec ')' { $$ = $4; }
    ;

retry_spec
    : KW_FOREVER
        {
            RetryPolicy r = {0};
            r.kind = RETRY_FOREVER;
            $$ = r;
        }
    | INT_LIT
        {
            RetryPolicy r = {0};
            r.kind  = RETRY_COUNT;
            r.count = $1;
            $$ = r;
        }
    | KW_BACKOFF '(' KW_INITIAL ':' INT_LIT ',' KW_MAX ':' INT_LIT ',' KW_FACTOR ':' INT_LIT ')'
        {
            RetryPolicy r = {0};
            r.kind            = RETRY_BACKOFF;
            r.backoff_initial = $5;
            r.backoff_max     = $9;
            r.backoff_factor  = $13;
            $$ = r;
        }
    ;

param_list
    : /* empty */
        {
            $$ = arena_alloc_zero(g_arena, sizeof(AstList));
            ast_list_init($$);
        }
    | param_list_nonempty
        { $$ = $1; }
    ;

param_list_nonempty
    : param
        {
            $$ = arena_alloc_zero(g_arena, sizeof(AstList));
            ast_list_init($$);
            ast_list_push($$, $1);
        }
    | param_list_nonempty ',' param
        {
            ast_list_push($1, $3);
            $$ = $1;
        }
    ;

param
    : IDENT ':' type
        { $$ = ast_param(g_arena, $1, $3, @1); }
    ;


/* ====================================================================
 * Flow declarations
 *
 *     flow name(Type)                   -> R { ... }       implicit `it`
 *     flow name(p1: T1, p2: T2, ...)    -> R { ... }       explicit
 *
 * The two parameter forms are syntactically distinct: an implicit
 * parameter is a bare type expression with no colon; an explicit
 * parameter is `name: type`. The grammar gives them their own
 * productions to keep the disambiguation table-driven and to avoid
 * any lookahead beyond LALR(1).
 *
 * A flow body is binding* followed by a return expression. The
 * grammar requires the return expression — an empty body is a
 * parse error reported as E110 at the closing brace, so the missing
 * return is caught here rather than reaching a later semantic pass.
 * ==================================================================== */

flow_decl
    : KW_FLOW IDENT '(' type ')' ARROW type '{' binding_list binding_rhs '}'
        {
            /* Implicit `it`: synthesize a single Param named "it" so
             * later phases treat all flows uniformly. The parser is
             * the one place this synthesis happens, per the
             * discipline laid out in ast.h. */
            Param **fp = arena_alloc(g_arena, sizeof(Param *));
            fp[0] = ast_param(g_arena, "it", $4, @4);

            Binding **bs;
            size_t n;
            FINALIZE(*$9, Binding **, bs, n);

            $$ = ast_flow_decl(g_arena, $2,
                                1 /* implicit_it */, fp, 1,
                                $7,
                                bs, n,
                                $10,
                                @1);
        }
    | KW_FLOW IDENT '(' param_list_nonempty ')' ARROW type '{' binding_list binding_rhs '}'
        {
            Param **pa;
            size_t np;
            FINALIZE(*$4, Param **, pa, np);

            Binding **bs;
            size_t nb;
            FINALIZE(*$9, Binding **, bs, nb);

            $$ = ast_flow_decl(g_arena, $2,
                                0 /* implicit_it */, pa, np,
                                $7,
                                bs, nb,
                                $10,
                                @1);
        }
    ;

binding_list
    : /* empty */
        {
            $$ = arena_alloc_zero(g_arena, sizeof(AstList));
            ast_list_init($$);
        }
    | binding_list binding
        {
            ast_list_push($1, $2);
            $$ = $1;
        }
    ;

binding
    : IDENT '=' binding_rhs
        { $$ = ast_binding(g_arena, $1, $3, @1); }
    ;


/* ====================================================================
 * Expressions
 *
 * The grammar has two layers:
 *
 *   binding_rhs   — what appears on the right of `=` and as the
 *                   return expression. Permits the pipeline form
 *                   in addition to everything `expr` accepts.
 *
 *   expr          — what appears inside argument lists, predicates,
 *                   and as the source of a pipeline. Does not include
 *                   the pipeline form.
 *
 * This split is the grammar-level enforcement that pipelines may
 * not be nested inside argument lists. The AST does not need a
 * separate node kind; EXPR_PIPELINE remains a regular Expr.
 *
 *   primary_expr  — a literal, a path, a call, a construct, or a
 *                   parenthesized `'(' pipeline_expr ')'`. The
 *                   parenthesized form is what lets a pipeline be
 *                   embedded inside a larger expression, since
 *                   `expr` itself excludes the pipeline form (see
 *                   the precedence-layer note below).
 *
 * `name { args }` is the construct form. It shares the apply
 * payload with calls; the distinction is the bracket.
 * ==================================================================== */

binding_rhs
    : pipeline_expr
        { $$ = $1; }
    ;


/* ====================================================================
 * Expression precedence layers
 *
 *   1 (lowest):  or             left
 *   2:           and            left
 *   3:           not            prefix
 *   4:           cmp ops        left-recursive (see below)
 *   5:           + -            left
 *   6:           * / %          left
 *   7:           unary -        prefix
 *   8:           | (pipeline)   left           ← only at binding_rhs top
 *   9 (highest): . (field)      left  (inside path_expr)
 *
 * The cmp-op level is plainly left-recursive in the grammar
 * (cmp_expr cmp_op_b add_expr); the grammar does not encode
 * non-associativity. `a < b < c` is accepted by the grammar and
 * builds a left-leaning binop tree; chained comparison is rejected
 * after the fact by the E114 diagnostic raised in the cmp_expr
 * action (a checker-style runtime rejection, not a grammatical one).
 *
 * Pipeline `|` is special: at level 8, but it competes
 * syntactically with stage separators (`... | select foo | top`).
 * To resolve cleanly we keep pipelines confined to binding-RHS top
 * level (`pipeline_expr`), with parenthesised grouping `( ... )` as
 * the way to embed a pipeline inside a larger expression. Stage
 * bodies use the pipeline-free `expr` so the `|` token is
 * unambiguously a stage separator inside them.
 *
 * The where-predicate keeps its own parallel grammar
 * (pred_cmp_op_expr, below) using primary_expr operands, rather than
 * reusing this ladder — see the predicate-operand note there for
 * why predicate operands are deliberately narrower.
 * ==================================================================== */

pipeline_expr
    : expr
        { $$ = $1; }
    | pipeline_expr '|' stage
        {
            /* Left recursion would nest each `| stage` as a pipeline
             * whose source is the previous pipeline. Instead flatten:
             * when the LHS is already a pipeline, append the new stage
             * to its stage array (rebuilt, since the array is
             * immutable) so the result stays one ExprPipeline with a
             * single source and N stages — the shape the checker and
             * IR emitter expect. */
            Expr *lhs = $1;
            if (lhs->kind == EXPR_PIPELINE) {
                size_t old_n = lhs->as.pipeline.n_stages;
                Stage **sa = arena_alloc(g_arena, (old_n + 1) * sizeof(Stage *));
                size_t k;
                for (k = 0; k < old_n; k++) {
                    sa[k] = lhs->as.pipeline.stages[k];
                }
                sa[old_n] = $3;
                /* Reuse lhs->loc (the original source's loc) so a multi-stage
                 * pipeline still points at its source, not at the latest `|`.
                 * The first-stage branch's @1 is the same point; keep both
                 * anchored to the source. */
                $$ = ast_expr_pipeline(g_arena, lhs->as.pipeline.source,
                                       sa, old_n + 1, lhs->loc);
            } else {
                Stage **sa = arena_alloc(g_arena, sizeof(Stage *));
                sa[0] = $3;
                $$ = ast_expr_pipeline(g_arena, lhs, sa, 1, @1);
            }
        }
    ;

expr
    : or_expr
        { $$ = $1; }
    ;

or_expr
    : and_expr
        { $$ = $1; }
    | or_expr KW_OR and_expr
        { $$ = ast_expr_binop(g_arena, BINOP_OR, $1, $3, @2); }
    ;

and_expr
    : not_expr
        { $$ = $1; }
    | and_expr KW_AND not_expr
        { $$ = ast_expr_binop(g_arena, BINOP_AND, $1, $3, @2); }
    ;

not_expr
    : cmp_expr
        { $$ = $1; }
    | KW_NOT not_expr
        { $$ = ast_expr_unop(g_arena, UNOP_NOT, $2, @1); }
    ;

/* Left-recursive form so chained comparisons (`a < b < c`) parse
 * as a tree we can introspect for the E114 specific diagnostic
 * (comparisons are non-associative; chains must use
 * `and`). The action checks whether the LHS is already a
 * comparison binop and, if so, emits E114 at the offending
 * operator's location. The AST node is still built so downstream
 * type checking can proceed without cascading errors.
 *
 * Note this test inspects only the LHS node kind, so an explicitly
 * parenthesised comparison still trips E114: `'(' pipeline_expr ')'`
 * (primary_expr) returns the inner Expr unchanged, so `(a < b) < c`
 * presents an LHS that is still an EXPR_BINOP comparison and is
 * rejected too. That is intentional — comparison results are booleans,
 * which are not ordered, so the parenthesised form has no useful
 * meaning either. */
cmp_expr
    : add_expr
        { $$ = $1; }
    | cmp_expr cmp_op_b add_expr
        {
            /* Test the six comparison ops explicitly, not just
             * kind==EXPR_BINOP: `a + b < c` has an arithmetic-binop LHS and
             * must be allowed. Deeper chains (a<b<c<d) are caught one
             * reduction at a time, since each rebuilt node re-presents a
             * comparison-kind LHS. */
            if ($1->kind == EXPR_BINOP) {
                BinopOp lop = $1->as.binop.op;
                if (lop == BINOP_EQ  || lop == BINOP_NEQ ||
                    lop == BINOP_LT  || lop == BINOP_GT  ||
                    lop == BINOP_LE  || lop == BINOP_GE)
                {
                    diag_emit(g_diag, @2, DIAG_ERROR, "E114",
                              "chained comparison '%s' is not allowed; "
                              "use 'and' to chain",
                              ast_binop_str((BinopOp)$2));
                }
            }
            $$ = ast_expr_binop(g_arena, (BinopOp)$2, $1, $3, @2);
        }
    ;

cmp_op_b
    : EQEQ { $$ = BINOP_EQ;  }
    | NEQ  { $$ = BINOP_NEQ; }
    | '<'  { $$ = BINOP_LT;  }
    | '>'  { $$ = BINOP_GT;  }
    | LE   { $$ = BINOP_LE;  }
    | GE   { $$ = BINOP_GE;  }
    ;

add_expr
    : mul_expr
        { $$ = $1; }
    | add_expr '+' mul_expr
        { $$ = ast_expr_binop(g_arena, BINOP_ADD, $1, $3, @2); }
    | add_expr '-' mul_expr
        { $$ = ast_expr_binop(g_arena, BINOP_SUB, $1, $3, @2); }
    ;

mul_expr
    : unary_expr
        { $$ = $1; }
    | mul_expr '*' unary_expr
        { $$ = ast_expr_binop(g_arena, BINOP_MUL, $1, $3, @2); }
    | mul_expr '/' unary_expr
        { $$ = ast_expr_binop(g_arena, BINOP_DIV, $1, $3, @2); }
    | mul_expr '%' unary_expr
        { $$ = ast_expr_binop(g_arena, BINOP_MOD, $1, $3, @2); }
    ;

unary_expr
    : primary_expr
        { $$ = $1; }
    | '-' unary_expr
        { $$ = ast_expr_unop(g_arena, UNOP_NEG, $2, @1); }
    ;

primary_expr
    : INT_LIT
        { $$ = ast_expr_int(g_arena, $1, @1); }
    | FLOAT_LIT
        { $$ = ast_expr_float(g_arena, $1, @1); }
    | KW_TRUE
        { $$ = ast_expr_bool(g_arena, 1, @1); }
    | KW_FALSE
        { $$ = ast_expr_bool(g_arena, 0, @1); }
    | '(' pipeline_expr ')'
        { $$ = $2; }
    | list_lit
        { $$ = $<expr>1; }
    | path_expr
        { $$ = $<expr>1; }
    | IDENT '(' arg_list ')'
        {
            Arg **aa;
            size_t n;
            FINALIZE(*$3, Arg **, aa, n);
            $$ = ast_expr_call(g_arena, $1, aa, n, @1);
        }
    | IDENT '{' arg_list '}'
        {
            Arg **aa;
            size_t n;
            FINALIZE(*$3, Arg **, aa, n);
            $$ = ast_expr_construct(g_arena, $1, aa, n, @1);
        }
    | match_expr
        { $$ = $<expr>1; }
    | conditional_expr
        { $$ = $<expr>1; }
    | try_expr
        { $$ = $<expr>1; }
    ;


/* ====================================================================
 * Match expressions
 *
 *     match scrutinee { pat -> body, pat binder -> body, _ -> body }
 *
 * The pattern forms are wildcard, bare-variant, and variant-with-
 * binder. Trailing comma admitted. The scrutinee is any expr; the
 * checker validates that it has a sum type (E161). Pattern variants
 * are resolved against the scrutinee's sum at check time.
 * ==================================================================== */

/* Scrutinee form is restricted to path_expr to avoid an LR(1)
 * conflict with construct expressions (`match Foo { ... }` would
 * otherwise be ambiguous between `match (Foo{...}) {...}` and
 * `match Foo {...}`). Users wanting a more complex scrutinee bind
 * it first: `c = expr; match c { ... }`. */
match_expr
    : KW_MATCH path_expr '{' match_arm_list '}'
        {
            MatchArm **ma;
            size_t n;
            FINALIZE(*$4, MatchArm **, ma, n);
            $<expr>$ = ast_expr_match(g_arena, $<expr>2, ma, n, @1);
        }
    ;

match_arm_list
    : match_arm
        {
            $$ = arena_alloc_zero(g_arena, sizeof(AstList));
            ast_list_init($$);
            ast_list_push($$, $1);
        }
    | match_arm_list ',' match_arm
        {
            ast_list_push($1, $3);
            $$ = $1;
        }
    | match_arm_list ','
        { $$ = $1; }
    ;

match_arm
    : IDENT ARROW expr
        {
            /* Capitalisation convention:
             *   `_`            → wildcard
             *   upper-case head → variant tag (no binder)
             *   lower-case head → fresh binder that matches anything
             *                     and binds the whole scrutinee value.
             * The parser does not enforce the convention beyond
             * dispatch; the resolver/checker treat each kind on its
             * own terms. */
            Pattern pat;
            const char *id = $1;
            /* id is arena_strndup'd in scan_ident and NUL-terminated, so id[1]
             * is in bounds even for a one-char name (it reads the terminator).
             * The casing convention is parser-side dispatch only; the checker
             * enforces meaning. */
            if (id[0] == '_' && id[1] == '\0') {
                pat = ast_pat_wildcard(@1);
            } else if (id[0] >= 'A' && id[0] <= 'Z') {
                pat = ast_pat_variant(id, @1);
            } else {
                pat = ast_pat_bind(id, @1);
            }
            $$ = ast_match_arm(g_arena, pat, $3, @1);
        }
    | IDENT IDENT ARROW expr
        {
            /* `Variant binder` form. First identifier is treated as
             * the variant tag regardless of casing; if it's lower-
             * case the checker will fail to match it against the
             * scrutinee's sum and emit E128. */
            Pattern pat = ast_pat_variant_bind($1, $2, @1);
            $$ = ast_match_arm(g_arena, pat, $4, @1);
        }
    ;


/* ====================================================================
 * Conditional expressions
 *
 *     if A then B else C
 *     if A then B else if C then D else E       (right-associative chain)
 *
 * `else` is required (bare `if` is rejected). The grammar flattens
 * an `if/else if/.../else` chain into a single EXPR_CONDITIONAL: at
 * each level we inspect whether the terminal-else position holds
 * another conditional and, if so, splice its branches into ours.
 * The end result is one EXPR_CONDITIONAL with branches in source
 * order plus one final else_expr.
 * ==================================================================== */

conditional_expr
    : KW_IF expr KW_THEN expr KW_ELSE expr
        {
            Expr *alt = $6;
            CondBranch *self = ast_cond_branch(g_arena, $2, $4);
            if (alt->kind == EXPR_CONDITIONAL) {
                size_t inner_n = alt->as.conditional.n_branches;
                size_t new_n   = inner_n + 1;
                CondBranch **branches = arena_alloc(
                    g_arena, new_n * sizeof(*branches));
                branches[0] = self;
                size_t i;
                for (i = 0; i < inner_n; i++) {
                    branches[i + 1] = alt->as.conditional.branches[i];
                }
                $<expr>$ = ast_expr_conditional(
                    g_arena, branches, new_n,
                    alt->as.conditional.else_expr, @1);
            } else {
                CondBranch **branches = arena_alloc(
                    g_arena, sizeof(*branches));
                branches[0] = self;
                $<expr>$ = ast_expr_conditional(
                    g_arena, branches, 1, alt, @1);
            }
        }
    ;


/* ====================================================================
 * Try/else expressions
 *
 *     try E else F
 *
 * Type-checker requires E and F have the same type; runtime
 * decides which R-codes route to F.
 * ==================================================================== */

try_expr
    : KW_TRY expr KW_ELSE expr
        {
            $<expr>$ = ast_expr_try_else(g_arena, $2, $4, @1);
        }
    ;


/* ====================================================================
 * List literals
 *
 *     []                — empty (needs type context)
 *     [e1, e2, e3]      — non-empty, trailing comma admitted
 *
 * Element list assembly mirrors the field_list pattern: scratch
 * AstList during parse, finalize into an Expr** array at close.
 * ==================================================================== */

list_lit
    : '[' ']'
        {
            $<expr>$ = ast_expr_list_literal(g_arena, NULL, 0, @1);
        }
    | '[' list_elem_list ']'
        {
            Expr **ea;
            size_t n;
            FINALIZE(*$2, Expr **, ea, n);
            $<expr>$ = ast_expr_list_literal(g_arena, ea, n, @1);
        }
    | '[' list_elem_list ',' ']'
        {
            Expr **ea;
            size_t n;
            FINALIZE(*$2, Expr **, ea, n);
            $<expr>$ = ast_expr_list_literal(g_arena, ea, n, @1);
        }
    ;

list_elem_list
    : expr
        {
            $$ = arena_alloc_zero(g_arena, sizeof(AstList));
            ast_list_init($$);
            ast_list_push($$, $1);
        }
    | list_elem_list ',' expr
        {
            ast_list_push($1, $3);
            $$ = $1;
        }
    ;


/* path_expr handles IDENT.IDENT..., KW_IT.IDENT..., and KW_ROW.IDENT...
 * uniformly. Each path head allocates a single-segment arena array
 * directly; each `.IDENT` extension copies the previous segments plus
 * the new tail into a fresh contiguous arena array. */

path_expr
    : IDENT
        {
            const char **segs = arena_alloc(g_arena, sizeof(char *));
            SrcLoc *locs = arena_alloc(g_arena, sizeof(SrcLoc));
            segs[0] = $1;
            locs[0] = @1;
            $<expr>$ = ast_expr_path(g_arena, segs, locs, 1, @1);
        }
    | KW_IT
        {
            /* Single-segment "it" path. The lexer returns KW_IT
             * rather than IDENT("it") so that the resolver can
             * tell a bound "it" from any other identifier; we
             * normalize back to a path here so the rest of the
             * compiler treats "it" uniformly with any other path
             * head. */
            const char **segs = arena_alloc(g_arena, sizeof(char *));
            SrcLoc *locs = arena_alloc(g_arena, sizeof(SrcLoc));
            segs[0] = "it";
            locs[0] = @1;
            $<expr>$ = ast_expr_path(g_arena, segs, locs, 1, @1);
        }
    | KW_ROW
        {
            /* `row` is the per-row stage scope's element binding.
             * Same path-head treatment as `it`: the parser normalises
             * to a single-segment path "row"; the checker resolves
             * it via the per-row env extension. */
            const char **segs = arena_alloc(g_arena, sizeof(char *));
            SrcLoc *locs = arena_alloc(g_arena, sizeof(SrcLoc));
            segs[0] = "row";
            locs[0] = @1;
            $<expr>$ = ast_expr_path(g_arena, segs, locs, 1, @1);
        }
    | path_expr '.' IDENT
        {
            /* Each extension reallocates the whole segment array;
             * cheap on the arena, and keeps segments contiguous. */
            Expr *prev = $<expr>1;
            size_t old_n = prev->as.path.n;
            const char **segs = arena_alloc(g_arena, (old_n + 1) * sizeof(char *));
            SrcLoc *locs = arena_alloc(g_arena, (old_n + 1) * sizeof(SrcLoc));
            for (size_t i = 0; i < old_n; i++) {
                segs[i] = prev->as.path.segments[i];
                locs[i] = prev->as.path.seg_locs[i];
            }
            segs[old_n] = $3;
            locs[old_n] = @3;
            $<expr>$ = ast_expr_path(g_arena, segs, locs, old_n + 1, prev->loc);
        }
    ;

/* ====================================================================
 * Pipeline stages
 *
 * One production per stage kind. The grammar permits stages to
 * appear in any order; the type checker enforces "terminal at the
 * end" (E154 if violated). The grammar can't do this cleanly
 * because the terminal/non-terminal distinction is semantic, not
 * syntactic.
 * ==================================================================== */

stage
    : KW_WHERE predicate
        { $$ = ast_stage_where(g_arena, $2, @1); }
    | KW_RANK IDENT sort_dir
        { $$ = ast_stage_rank(g_arena, $2, (SortDir)$3, @1); }
    | KW_SELECT expr
        { $$ = ast_stage_select(g_arena, $2, @1); }
    | KW_DEDUPE KW_BY expr
        { $$ = ast_stage_dedupe(g_arena, $3, @1); }
    | KW_CONCAT expr
        { $$ = ast_stage_concat(g_arena, $2, @1); }
    | KW_TAKE INT_LIT
        { $$ = ast_stage_take(g_arena, $2, @1); }
    | KW_TOP
        { $$ = ast_stage_top(g_arena, @1); }
    | KW_PICK KW_USING IDENT
        { $$ = ast_stage_pick(g_arena, $3, @1); }
    | KW_COUNT
        { $$ = ast_stage_count(g_arena, @1); }
    | KW_SUM IDENT
        { $$ = ast_stage_sum(g_arena, $2, @2, @1); }
    | KW_MAX IDENT
        { $$ = ast_stage_max(g_arena, $2, @2, @1); }
    | KW_MIN IDENT
        { $$ = ast_stage_min(g_arena, $2, @2, @1); }
    | KW_ANY expr
        { $$ = ast_stage_any(g_arena, $2, @1); }
    | KW_ALL expr
        { $$ = ast_stage_all(g_arena, $2, @1); }
    ;

sort_dir
    : KW_ASC  { $$ = SORT_ASC;  }
    | KW_DESC { $$ = SORT_DESC; }
    ;


/* ====================================================================
 * Predicates — precedence encoded in the grammar shape, not %left
 * (see "Explicit precedence" in the file header for why).
 *
 *     predicate ::= and_pred         ('or'  and_pred)*
 *     and_pred  ::= pred_cmp_op_expr ('and' pred_cmp_op_expr)*
 *
 * `and` binds tighter than `or`, matching SQL and conventional
 * boolean precedence. The shape is left-associative for both
 * operators: `A and B and C` parses as `(A and B) and C`. This
 * matches the AST's PRED_AND structure, which carries left and
 * right operands and is naturally left-leaning.
 * ==================================================================== */

predicate
    : and_pred
        { $$ = $1; }
    | predicate KW_OR and_pred
        { $$ = ast_pred_or(g_arena, $1, $3, @2); }
    ;

and_pred
    : pred_cmp_op_expr
        { $$ = ast_pred_cmp(g_arena, $1); }
    | and_pred KW_AND pred_cmp_op_expr
        { $$ = ast_pred_and(g_arena, $1, ast_pred_cmp(g_arena, $3), @2); }
    ;

/* Predicate operands are restricted to primary expressions (paths,
 * literals, calls, constructs) so the where-predicate parser stays
 * unambiguous with the new layered general expression grammar.
 * Users wanting richer expressions inside where bind first
 * (`x = a + b; ... | where x >= 100`) or use any/all. */
pred_cmp_op_expr
    : primary_expr cmp_op primary_expr
        { $$ = ast_cmp(g_arena, $1, (CmpOp)$2, $3, @1); }
    ;

cmp_op
    : LE   { $$ = CMP_LE;  }
    | GE   { $$ = CMP_GE;  }
    | '<'  { $$ = CMP_LT;  }
    | '>'  { $$ = CMP_GT;  }
    | EQEQ { $$ = CMP_EQ;  }
    | NEQ  { $$ = CMP_NEQ; }
    ;


/* ====================================================================
 * Arguments
 *
 * Two forms: explicit `name: expr` and shorthand bare `expr`.
 * Both produce an Arg; in the shorthand case `field` is NULL and
 * the resolver fills it in once it knows the called tool's
 * parameter list. See the design note in the file header.
 * ==================================================================== */

arg_list
    : /* empty */
        {
            $$ = arena_alloc_zero(g_arena, sizeof(AstList));
            ast_list_init($$);
        }
    | arg_list_nonempty
        { $$ = $1; }
    ;

arg_list_nonempty
    : arg
        {
            $$ = arena_alloc_zero(g_arena, sizeof(AstList));
            ast_list_init($$);
            ast_list_push($$, $1);
        }
    | arg_list_nonempty ',' arg
        {
            ast_list_push($1, $3);
            $$ = $1;
        }
    ;

arg
    : IDENT ':' expr
        { $$ = ast_arg(g_arena, $1, $3, @1); }
    | expr
        { $$ = ast_arg(g_arena, NULL, $1, @1); }
    ;


/* ====================================================================
 * Type expressions
 * ==================================================================== */

type
    : KW_STRING        { $$ = ast_type_primitive(g_arena, TYPE_STRING, @1); }
    | KW_INT           { $$ = ast_type_primitive(g_arena, TYPE_INT,    @1); }
    | KW_FLOAT         { $$ = ast_type_primitive(g_arena, TYPE_FLOAT,  @1); }
    | KW_BOOL          { $$ = ast_type_primitive(g_arena, TYPE_BOOL,   @1); }
    | IDENT            { $$ = ast_type_named(g_arena, $1, @1); }
    | '[' type ']'     { $$ = ast_type_list(g_arena, $2, @1); }
    ;


%%


/* ====================================================================
 * yyerror — route bison's syntactic errors into our diagnostic stream
 *
 * yyerror is called once per parse error, before yyparse returns
 * non-zero. bison passes the message text as a C string ("syntax
 * error" or, on newer versions, "syntax error, unexpected ...").
 *
 * We emit E110 ("syntax error") with the current yylloc. The
 * driver later checks diag_error_count() and aborts the pipeline.
 *
 * We do not call exit(); the parser unwinds normally and the
 * driver decides what to do next.
 * ==================================================================== */

void yyerror(const char *msg)
{
    diag_emit(g_diag, yylloc, DIAG_ERROR, "E110", "%s", msg);
}


/* ====================================================================
 * Parser entry points
 *
 * parse_open() seeds the file-scope arena and source filename, then
 * parse_run() invokes yyparse. parse_close() returns the completed
 * Program and clears the static state.
 *
 * Splitting open/run/close mirrors the lexer's lex_open/lex_token/
 * lex_close interface and keeps the bison globals out of api.c.
 * ==================================================================== */

void parse_open(Arena *arena, const char *source_file, DiagStream *diag)
{
    if (arena == NULL || source_file == NULL || diag == NULL) {
        FLOWC_ICE("parse_open called with a NULL argument");
    }
    g_arena       = arena;
    g_source_file = source_file;
    g_diag        = diag;
    g_program     = NULL;
}

int parse_run(void)
{
    /* yyparse returns 0 on a syntactically successful parse, 1 on
     * syntax error, 2 on memory exhaustion. A 0 return does NOT
     * imply "no diagnostics raised": semantic actions can emit an
     * error (e.g. E114 for a chained comparison, in the cmp_expr
     * action) while the parse itself still succeeds and yyparse
     * returns 0. For that reason the level above does not trust this
     * return value as a success signal — it consults the diagnostic
     * stream's error count, which is authoritative. */
    return yyparse();
}

Program *parse_close(void)
{
    Program *p = g_program;
    g_program     = NULL;
    g_source_file = NULL;
    g_arena       = NULL;
    g_diag        = NULL;
    return p;
}
