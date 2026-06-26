/* src/ast.h
 *
 * Abstract syntax tree for the Flow language.
 *
 * The AST is the parser's output and the input to every later phase
 * (resolve, check, emit). It mirrors the grammar closely; where it
 * diverges, the divergence is documented on the affected struct.
 *
 *
 * Design choices
 * --------------
 *
 * Tagged unions, not inheritance. Expressions and pipeline stages
 * are sum types: an expression is exactly one of literal, list
 * literal, path, call, construct, pipeline, match, conditional,
 * try/else, binop, or unop. Each is represented as a struct
 * with a `kind` discriminant and a union of per-kind payloads. This
 * is the same shape every long-lived C compiler uses (cf. tcc, the
 * Plan 9 compilers, lcc) and the only shape that compiles cleanly
 * under -pedantic without extensions.
 *
 * Arena-allocated. Every AST node is allocated from an Arena passed
 * in by the parser. Nodes are never freed individually; the entire
 * tree disappears when the arena is destroyed. This matches the
 * batch-compiler discipline established in util.h.
 *
 * Source locations on every node. Every struct that represents a
 * grammar production has a `loc` field carrying the source location
 * of its first token. This is what the diagnostic stream consumes
 * when reporting errors. Pre-grammar lexemes (the identifier in an
 * IDENT token, the integer value in an INT_LIT) do not appear in
 * the AST directly; they are folded into the nodes that consume
 * them.
 *
 * No backpointers. AST traversal is top-down; nodes do not point
 * to their parents. Traversal terminates without cycle checks,
 * construction is order-independent, and we sidestep the lifetime
 * questions backpointers introduce. The resolver and checker carry
 * their own context stacks during their walks.
 *
 * Identifiers are interned strings, but trivially. The parser copies
 * each identifier lexeme into the arena once (via arena_strndup) and
 * stores the resulting char* on the node. Comparisons use strcmp;
 * a real intern table would not pay for itself at this size.
 *
 *
 * Memory layout
 * -------------
 *
 * Lists of homogeneous children (fields of a record, parameters of
 * a tool, bindings of a flow, stages of a pipeline) are stored as a
 * pointer-plus-count pair: a contiguous array of pointers, allocated
 * once when the count is known. There are no intrusive linked lists.
 * The arena makes the bulk allocation cheap; the contiguous layout
 * makes the resolve/check/emit walks cache-friendly.
 *
 * For variable-length children encountered during parsing (where the
 * count is not known up front), the parser uses an ast_list scratch
 * buffer that doubles as needed and is finalized into the
 * pointer-plus-count layout when the production is complete. The
 * scratch buffer lives outside the AST; only the finalized arrays
 * end up in the tree.
 */

#ifndef FLOWC_AST_H
#define FLOWC_AST_H

#include <stddef.h>

#include "util.h"


/* ====================================================================
 * Forward declarations
 *
 * The AST is heavily self-referential: a Decl contains a Flow,
 * which contains Bindings, which contain Exprs, which contain
 * Calls, whose arguments are Exprs again. Forward-declaring every
 * struct up front lets the rest of the header refer to them in any
 * order.
 * ==================================================================== */

typedef struct Type         Type;         /* type expression */
typedef struct Field        Field;        /* one row inside { ... } */
typedef struct Variant      Variant;      /* one row inside a sum body */
typedef struct Param        Param;        /* one tool/flow parameter */
typedef struct Arg          Arg;          /* one call/construct argument */
typedef struct Predicate    Predicate;    /* boolean expression in `where` */
typedef struct CmpExpr      CmpExpr;      /* leaf of a predicate */
typedef struct Stage        Stage;        /* one pipeline stage */
typedef struct Expr         Expr;         /* binding right-hand side */
typedef struct Binding      Binding;      /* `name = expr` */
typedef struct EffectClause EffectClause; /* `effect <level>(retry: ...)` */
typedef struct TypeDecl     TypeDecl;
typedef struct ToolDecl     ToolDecl;
typedef struct FlowDecl     FlowDecl;
typedef struct Decl         Decl;         /* tagged union over the three */
typedef struct Program      Program;      /* the top-level node */


/* ====================================================================
 * Types
 *
 * The four primitive types are represented as TypeKind values
 * without payload. User-defined types are represented by name; the
 * resolver later binds the name to a TypeDecl. List types carry a
 * pointer to their element type.
 *
 * A Type's canonical string form (used in IR output and diagnostic
 * messages) is computed on demand by ast.c; it is not stored on the
 * node.
 * ==================================================================== */

typedef enum {
    TYPE_STRING,
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_BOOL,
    TYPE_NAMED,   /* user-defined record, identified by name */
    TYPE_LIST     /* `[T]`, with elem set */
} TypeKind;

struct Type {
    TypeKind    kind;
    SrcLoc      loc;
    const char *name;   /* TYPE_NAMED: the identifier */
    Type       *elem;   /* TYPE_LIST:  the element type */
};


/* ====================================================================
 * Field — one row inside a `{ ... }` block
 *
 * Used in two places: the body of a `type` declaration
 * (`type T { f1: T1, ... }`) and the field rows of a sum-type variant
 * body (`| Variant { f: T }`). The Field is the unit of nominal-record
 * identity: rename a field, every site referencing it is a fresh
 * compile error.
 * ==================================================================== */

struct Field {
    SrcLoc      loc;
    const char *name;
    Type       *type;
};


/* ====================================================================
 * Param — one parameter of a tool or multi-input flow
 *
 * Single-input flows with an implicit `it` parameter use this
 * struct too: the parser synthesizes a real Param named "it" as
 * params[0] (with `implicit_it = true` on the FlowDecl). Multi-input
 * flows and all tools use Param uniformly.
 * ==================================================================== */

struct Param {
    SrcLoc      loc;
    const char *name;
    Type       *type;
};


/* ====================================================================
 * Arg — one argument of a call or struct construction
 *
 * `field` is the parameter name being supplied. `value` is the
 * expression producing the value. Shorthand calls (`f(x)` for
 * `f(x: x)`) are recorded with field == NULL and stay NULL for the
 * rest of the pipeline: no phase ever writes `field` back. Instead
 * the checker (check.c) and the IR emitter (ir.c) each re-derive the
 * shorthand->parameter mapping positionally from the callee's formals
 * at the point of use, by counting shorthand args in source order.
 * ==================================================================== */

struct Arg {
    SrcLoc      loc;
    const char *field;    /* NULL for shorthand args; never filled in later */
    Expr       *value;
};


/* ====================================================================
 * Predicate and CmpExpr — boolean expressions in `where` clauses
 *
 * The general expression grammar DOES include boolean and comparison
 * operators as first-class Expr nodes: `and`/`or`/`not` and the six
 * comparison operators have their own BinopOp/UnopOp opcodes (below)
 * and parse.y productions (or_expr/and_expr/not_expr/cmp_expr). The
 * Predicate/CmpExpr micro-AST is a separate, parallel mechanism used
 * only inside `where` clauses, whose operands are deliberately
 * narrower (primary_expr only) than a full Expr so the predicate
 * grammar stays unambiguous alongside the general expression ladder.
 *
 * A predicate is a chain of CmpExprs joined by KW_AND or KW_OR,
 * with AND binding tighter than OR. The precedence is encoded in
 * the grammar shape (predicate -> and_pred -> cmp), not a table.
 * ==================================================================== */

typedef enum {
    CMP_LE,    /* <= */
    CMP_GE,    /* >= */
    CMP_LT,    /* <  */
    CMP_GT,    /* >  */
    CMP_EQ,    /* == */
    CMP_NEQ    /* != */
} CmpOp;

struct CmpExpr {
    SrcLoc  loc;
    Expr   *lhs;
    CmpOp   op;
    Expr   *rhs;
};

typedef enum {
    PRED_CMP,
    PRED_AND,
    PRED_OR
} PredKind;

struct Predicate {
    PredKind  kind;
    SrcLoc    loc;
    /* PRED_CMP:        cmp set; left and right unused.
     * PRED_AND/PRED_OR: left and right set; cmp unused. */
    CmpExpr   *cmp;
    Predicate *left;
    Predicate *right;
};


/* ====================================================================
 * Stage — one pipeline stage
 *
 * Seven kinds (see StageKind below), matching the stage signatures:
 *
 *   where    [T] -> [T]      filter by a predicate
 *   rank     [T] -> [T]      sort by a field, asc|desc
 *   select   [T] -> [U]      map each row to an expression
 *   dedupe   [T] -> [T]      `dedupe by <key>`
 *   concat   [T] -> [T]      append another list
 *   take     [T] -> [T]      keep the first <n>
 *   terminal [T] -> T or U   top / pick / count / sum / max / min / any / all
 *
 * The terminal kind is further distinguished by `terminal_kind`
 * (TerminalKind): deterministic `top` vs. model-mediated `pick`, plus
 * the aggregations count/sum/max/min/any/all. This split lives in the
 * AST so downstream phases (and the IR) can tell them apart without
 * re-parsing.
 * ==================================================================== */

typedef enum {
    STAGE_WHERE,
    STAGE_RANK,
    STAGE_SELECT,
    STAGE_DEDUPE,    /* `dedupe by <expr>` */
    STAGE_CONCAT,    /* `concat <expr>` */
    STAGE_TAKE,      /* `take <int_lit>` */
    STAGE_TERMINAL
} StageKind;

typedef enum {
    SORT_ASC,
    SORT_DESC
} SortDir;

typedef enum {
    TERMINAL_TOP,    /* deterministic */
    TERMINAL_PICK,   /* model-mediated; model name in `model` */
    TERMINAL_COUNT,
    TERMINAL_SUM,    /* SUM/MAX/MIN: agg_field/agg_field_loc carry the field */
    TERMINAL_MAX,
    TERMINAL_MIN,
    TERMINAL_ANY,    /* agg_pred carries the predicate expression */
    TERMINAL_ALL
} TerminalKind;

struct Stage {
    StageKind    kind;
    SrcLoc       loc;

    /* STAGE_WHERE */
    Predicate   *predicate;

    /* STAGE_RANK */
    const char  *sort_field;
    SortDir      sort_dir;

    /* STAGE_SELECT, STAGE_DEDUPE, STAGE_CONCAT — `body` is the
     * select expression, the dedupe key expression, or the concat
     * other-list expression, depending on `kind`. */
    Expr        *body;

    /* STAGE_TAKE — parsed as an int literal at the grammar level,
     * stored verbatim. */
    long         take_count;

    /* STAGE_TERMINAL */
    TerminalKind terminal_kind;
    const char  *model;       /* TERMINAL_PICK */
    const char  *agg_field;       /* TERMINAL_SUM/MAX/MIN */
    SrcLoc       agg_field_loc;    /* location of agg_field, for field diagnostics */
    Expr        *agg_pred;    /* TERMINAL_ANY/ALL */
};


/* ====================================================================
 * Expr — the binding right-hand side
 *
 * Expression nodes are heap-arena objects pointed to by other nodes.
 * Inlining the Expr struct would make every Binding, every Arg, and
 * every CmpExpr larger by sizeof(Expr); the indirection costs one
 * pointer per slot and one cache line per dereference, which is the
 * right trade for an AST that lives only as long as the compilation.
 * ==================================================================== */

typedef enum {
    EXPR_LITERAL,
    EXPR_LIST_LITERAL,
    EXPR_PATH,
    EXPR_CALL,
    EXPR_CONSTRUCT,
    EXPR_PIPELINE,
    EXPR_MATCH,
    EXPR_CONDITIONAL,
    EXPR_TRY_ELSE,
    EXPR_BINOP,
    EXPR_UNOP
} ExprKind;

/* Binary operator opcodes. The trailing comment on each gives the
 * source spelling and the operand types the checker accepts; the
 * spelling is what ast_binop_str returns so dump / IR emit render the
 * operator verbatim without a separate table. */
typedef enum {
    BINOP_OR,    /* or  — bool */
    BINOP_AND,   /* and — bool */
    BINOP_EQ,    /* ==  — primitive */
    BINOP_NEQ,   /* !=  — primitive */
    BINOP_LT,    /* <   — numeric|string */
    BINOP_GT,    /* >   — numeric|string */
    BINOP_LE,    /* <=  — numeric|string */
    BINOP_GE,    /* >=  — numeric|string */
    BINOP_ADD,   /* +   — numeric */
    BINOP_SUB,   /* -   — numeric */
    BINOP_MUL,   /* *   — numeric */
    BINOP_DIV,   /* /   — numeric */
    BINOP_MOD    /* %   — both operands int; E149 if either is non-int */
} BinopOp;

typedef enum {
    UNOP_NEG,    /* -   — numeric */
    UNOP_NOT     /* not — bool */
} UnopOp;

typedef struct {
    BinopOp op;
    Expr   *lhs;
    Expr   *rhs;
} ExprBinop;

typedef struct {
    UnopOp op;
    Expr  *operand;
} ExprUnop;


/* Pattern in a match arm. Four forms:
 *   PAT_WILDCARD       — `_`
 *   PAT_BIND           — bare lower-case ident: catch-all that also
 *                        binds the whole scrutinee to a name
 *   PAT_VARIANT        — `Name` (no binder)
 *   PAT_VARIANT_BIND   — `Name binder` (binds the variant payload)
 *
 * resolved_variant is meaningful only for the two variant forms and
 * stays NULL until the checker knows the scrutinee's sum type and can
 * bind the name to a Variant. */
typedef enum {
    PAT_WILDCARD,
    PAT_BIND,
    PAT_VARIANT,
    PAT_VARIANT_BIND
} PatternKind;

typedef struct {
    PatternKind     kind;
    SrcLoc          loc;
    const char     *variant_name;     /* PAT_VARIANT[_BIND] */
    const char     *binder_name;      /* PAT_VARIANT_BIND */
    const Variant  *resolved_variant; /* set by checker */
} Pattern;

typedef struct {
    SrcLoc   loc;
    Pattern  pattern;
    Expr    *body;
} MatchArm;

/* EXPR_MATCH payload. The scrutinee is evaluated once; arms are
 * scanned in source order. The resolved sum type is filled by the
 * checker. */
typedef struct {
    Expr           *scrutinee;
    MatchArm      **arms;
    size_t          n_arms;
    const TypeDecl *resolved_sum;     /* set by checker */
} ExprMatch;

/* EXPR_CONDITIONAL payload — a flat `if/else if/.../else` chain.
 * `branches[i]` is one `(cond, consequent)` pair in source order;
 * `else_expr` is the final fallthrough (always required).
 * For a simple `if A then B else C`, branches has one
 * entry. */
typedef struct {
    Expr *cond;
    Expr *consequent;
} CondBranch;

typedef struct {
    CondBranch **branches;
    size_t       n_branches;
    Expr        *else_expr;
} ExprConditional;

/* EXPR_TRY_ELSE payload. Both arms must have the
 * same type; runtime decides which R-codes route to `else_expr`. */
typedef struct {
    Expr *try_expr;
    Expr *else_expr;
} ExprTryElse;

typedef enum {
    LIT_INT,
    LIT_FLOAT,
    LIT_BOOL
} LiteralKind;

/* EXPR_LITERAL payload */
typedef struct {
    LiteralKind kind;
    long        int_val;
    double      float_val;
    int         bool_val;   /* 0 or 1 */
} ExprLiteral;

/* EXPR_PATH payload — `a.b.c` is segments = {"a","b","c"}, n = 3.
 * A bare identifier is a path with one segment. */
typedef struct {
    const char **segments;
    const SrcLoc *seg_locs;   /* source location of each segment, parallel to
                               * segments[]; lets a field diagnostic point its
                               * caret at the offending segment, not the head */
    size_t       n;
} ExprPath;

/* EXPR_CALL and EXPR_CONSTRUCT share a payload shape because they
 * parse identically (`name ( args )` vs. `name { args }`); the
 * distinction is recorded by which Expr kind the parser sets.
 *
 * For EXPR_CONSTRUCT the resolver decides whether `name` is a
 * record-type construct or a variant construct (a single bare
 * variant of some sum type). On a variant match it sets resolved_sum
 * and resolved_variant so the checker and IR emitter can dispatch
 * without re-running the lookup. Both fields are NULL for record
 * constructs and for EXPR_CALL nodes. */
typedef struct {
    const char     *name;
    Arg           **args;            /* array of pointers; never NULL */
    size_t          n_args;
    const TypeDecl *resolved_sum;
    const Variant  *resolved_variant;
    /* EXPR_CALL: when set, the resolver matched `name` as a flow
     * rather than a tool. The IR emitter routes via "subflow_call"
     * and the checker uses the flow's param/return signature. */
    const FlowDecl *resolved_flow;
} ExprApply;

/* EXPR_PIPELINE payload — a source expression and a sequence of
 * stages applied to it. The grammar guarantees n_stages >= 1; a
 * bare source expression is parsed as a non-pipeline Expr. */
typedef struct {
    Expr    *source;
    Stage  **stages;
    size_t   n_stages;
} ExprPipeline;

/* EXPR_LIST_LITERAL payload — `[e1, e2, ..., en]`. The element type
 * is resolved by the checker (unified across elements, or borrowed
 * from declared context for the empty case). */
typedef struct {
    Expr      **elements;
    size_t      n_elements;
    const Type *element_type;   /* set by checker */
} ExprListLiteral;

struct Expr {
    ExprKind kind;
    SrcLoc   loc;
    union {
        ExprLiteral     literal;
        ExprListLiteral list_literal;
        ExprPath        path;
        ExprApply       apply;     /* both EXPR_CALL and EXPR_CONSTRUCT */
        ExprPipeline    pipeline;
        ExprMatch       match;
        ExprConditional conditional;
        ExprTryElse     try_else;
        ExprBinop       binop;
        ExprUnop        unop;
    } as;
};


/* ====================================================================
 * Binding — `name = expr` inside a flow body
 *
 * The name is in scope for the rest of the enclosing flow; the value
 * lives until the arena is destroyed. The Binding owns neither.
 * ==================================================================== */

struct Binding {
    SrcLoc      loc;
    const char *name;
    Expr       *value;
};


/* ====================================================================
 * Top-level declarations
 *
 * Three kinds: type, tool, flow. Each is represented by its own
 * struct; Decl is the tagged union that the Program holds an array
 * of. This is the same layout pattern as Expr, applied to a smaller
 * sum type.
 * ==================================================================== */

/* A sum-type variant: a record body with a tag (the variant name).
 * Variants are stored in source order on the parent TypeDecl. */
struct Variant {
    SrcLoc       loc;
    const char  *name;
    Field      **fields;
    size_t       n_fields;
};

typedef enum {
    TYPE_DECL_RECORD,  /* `type T = { ... }` — fields[] is valid */
    TYPE_DECL_SUM      /* `type T = | A {...} | B {...}` — variants[] is valid */
} TypeDeclKind;

struct TypeDecl {
    SrcLoc        loc;
    const char   *name;
    TypeDeclKind  kind;
    /* TYPE_DECL_RECORD */
    Field       **fields;
    size_t        n_fields;
    /* TYPE_DECL_SUM */
    Variant     **variants;
    size_t        n_variants;
};

/* ====================================================================
 * EffectClause — `effect <level>(retry: ...)` on a tool declaration
 *
 * Every tool in v1 carries an effect clause naming its runtime
 * effect level. The clause governs how
 * the runtime treats the tool at execution time — caching, retry,
 * parallelism, replay — and is recorded in the IR so the
 * runtime can apply per-level policy without re-parsing.
 *
 * The AST stores the clause structurally rather than as text so
 * later phases can read level / model_name / retry without
 * re-tokenizing. A NULL EffectClause on a ToolDecl means the source
 * lacked the clause — the resolver emits E198 and the IR is not
 * emitted.
 * ==================================================================== */

typedef enum {
    EFFECT_PURE,
    EFFECT_DETERMINISTIC,
    EFFECT_MODEL,         /* model_name carries the model identifier */
    EFFECT_MUTATION
} EffectLevel;

typedef enum {
    RETRY_DEFAULT,    /* no `(retry: ...)` clause — runtime uses per-level default */
    RETRY_FOREVER,
    RETRY_COUNT,
    RETRY_BACKOFF
} RetryKind;

/* All numeric fields use `long` to match the lexer's int_val type. */
typedef struct {
    RetryKind kind;
    long      count;             /* RETRY_COUNT: attempt count */
    long      backoff_initial;   /* RETRY_BACKOFF: initial delay (ms) */
    long      backoff_max;       /* RETRY_BACKOFF: max delay (ms) */
    long      backoff_factor;    /* RETRY_BACKOFF: multiplier */
} RetryPolicy;

struct EffectClause {
    SrcLoc       loc;
    EffectLevel  level;
    /* EFFECT_MODEL: the model identifier string from `model("...")`,
     * arena-allocated. NULL for the other levels. */
    const char  *model_name;
    RetryPolicy  retry;
};


struct ToolDecl {
    SrcLoc        loc;
    const char   *name;
    Param       **params;
    size_t        n_params;
    Type         *return_type;
    /* Mandatory in v1, but the grammar accepts a missing clause so
     * the resolver can emit a single targeted E198 instead of a
     * raw E110 syntax error. NULL means no clause was present. */
    EffectClause *effect;
};

struct FlowDecl {
    SrcLoc       loc;
    const char  *name;

    /* Parameters. If implicit_it is true, n_params is 1 and
     * params[0]->name is "it". The parser sets the implicit name
     * explicitly so that later phases can treat all flows uniformly:
     * there is exactly one place where the name "it" gets bound,
     * and it is here. */
    int          implicit_it;
    Param      **params;
    size_t       n_params;

    Type        *return_type;

    Binding    **bindings;
    size_t       n_bindings;

    /* The final expression in the flow body, which is the return
     * value. This is required; the parser enforces it. */
    Expr        *return_expr;
};

typedef enum {
    DECL_TYPE,
    DECL_TOOL,
    DECL_FLOW
} DeclKind;

struct Decl {
    DeclKind kind;
    union {
        TypeDecl *type_decl;
        ToolDecl *tool_decl;
        FlowDecl *flow_decl;
    } as;
};


/* ====================================================================
 * Program — the AST root
 *
 * A flat list of declarations in source order, plus the file from
 * which they were parsed (for diagnostics on program-level issues
 * such as an empty file or duplicate top-level names).
 * ==================================================================== */

struct Program {
    const char  *source_file;
    Decl       **decls;
    size_t       n_decls;
};


/* ====================================================================
 * Constructors
 *
 * One constructor per node kind. Each takes an Arena*, all the
 * required fields, and returns a fully-initialized pointer. None
 * of these can fail in a way the caller can recover from: allocation
 * failure goes through FLOWC_ICE, and every other failure mode is
 * the caller's responsibility (e.g., the parser is responsible for
 * not building a CmpExpr with a NULL operand).
 *
 * Collection types (Field**, Param**, etc.) are built incrementally
 * by the parser using ast_list_* helpers below, then finalized into
 * the contiguous arrays the constructors expect.
 * ==================================================================== */

/* Types */
Type *ast_type_primitive(Arena *a, TypeKind kind, SrcLoc loc);
Type *ast_type_named    (Arena *a, const char *name, SrcLoc loc);
Type *ast_type_list     (Arena *a, Type *elem, SrcLoc loc);

/* Records and sum variants */
Field   *ast_field  (Arena *a, const char *name, Type *type, SrcLoc loc);
Variant *ast_variant(Arena *a, const char *name, Field **fields, size_t n,
                     SrcLoc loc);
Param   *ast_param  (Arena *a, const char *name, Type *type, SrcLoc loc);
Arg     *ast_arg    (Arena *a, const char *field /*NULL for shorthand; stays NULL*/, Expr *value, SrcLoc loc);

/* Predicates */
CmpExpr   *ast_cmp       (Arena *a, Expr *lhs, CmpOp op, Expr *rhs, SrcLoc loc);
Predicate *ast_pred_cmp  (Arena *a, CmpExpr *cmp);
Predicate *ast_pred_and  (Arena *a, Predicate *left, Predicate *right, SrcLoc loc);
Predicate *ast_pred_or   (Arena *a, Predicate *left, Predicate *right, SrcLoc loc);

/* Pipeline stages */
Stage *ast_stage_where    (Arena *a, Predicate *p, SrcLoc loc);
Stage *ast_stage_rank     (Arena *a, const char *field, SortDir dir, SrcLoc loc);
Stage *ast_stage_select   (Arena *a, Expr *body, SrcLoc loc);
Stage *ast_stage_dedupe   (Arena *a, Expr *key,  SrcLoc loc);
Stage *ast_stage_concat   (Arena *a, Expr *other,SrcLoc loc);
Stage *ast_stage_take     (Arena *a, long n, SrcLoc loc);
Stage *ast_stage_top      (Arena *a, SrcLoc loc);
Stage *ast_stage_pick     (Arena *a, const char *model, SrcLoc loc);
Stage *ast_stage_count    (Arena *a, SrcLoc loc);
Stage *ast_stage_sum      (Arena *a, const char *field, SrcLoc field_loc, SrcLoc loc);
Stage *ast_stage_max      (Arena *a, const char *field, SrcLoc field_loc, SrcLoc loc);
Stage *ast_stage_min      (Arena *a, const char *field, SrcLoc field_loc, SrcLoc loc);
Stage *ast_stage_any      (Arena *a, Expr *pred, SrcLoc loc);
Stage *ast_stage_all      (Arena *a, Expr *pred, SrcLoc loc);

/* Expressions */
Expr *ast_expr_int          (Arena *a, long v,   SrcLoc loc);
Expr *ast_expr_float        (Arena *a, double v, SrcLoc loc);
Expr *ast_expr_bool         (Arena *a, int v,    SrcLoc loc);
Expr *ast_expr_list_literal (Arena *a, Expr **elements, size_t n, SrcLoc loc);
Expr *ast_expr_path      (Arena *a, const char **segments,
                          const SrcLoc *seg_locs, size_t n, SrcLoc loc);
Expr *ast_expr_call      (Arena *a, const char *name, Arg **args, size_t n, SrcLoc loc);
Expr *ast_expr_construct (Arena *a, const char *name, Arg **args, size_t n, SrcLoc loc);
Expr *ast_expr_pipeline  (Arena *a, Expr *source, Stage **stages, size_t n, SrcLoc loc);
Expr *ast_expr_match     (Arena *a, Expr *scrutinee, MatchArm **arms, size_t n,
                          SrcLoc loc);
Expr *ast_expr_conditional(Arena *a, CondBranch **branches, size_t n,
                           Expr *else_expr, SrcLoc loc);
Expr *ast_expr_try_else  (Arena *a, Expr *try_expr, Expr *else_expr,
                          SrcLoc loc);
Expr *ast_expr_binop     (Arena *a, BinopOp op, Expr *lhs, Expr *rhs,
                          SrcLoc loc);
Expr *ast_expr_unop      (Arena *a, UnopOp op, Expr *operand, SrcLoc loc);

const char *ast_binop_str(BinopOp op);
const char *ast_unop_str (UnopOp op);

CondBranch *ast_cond_branch(Arena *a, Expr *cond, Expr *consequent);

MatchArm *ast_match_arm  (Arena *a, Pattern pattern, Expr *body, SrcLoc loc);
Pattern   ast_pat_wildcard    (SrcLoc loc);
Pattern   ast_pat_bind        (const char *binder, SrcLoc loc);
Pattern   ast_pat_variant     (const char *name, SrcLoc loc);
Pattern   ast_pat_variant_bind(const char *name, const char *binder, SrcLoc loc);

/* Bindings */
Binding *ast_binding(Arena *a, const char *name, Expr *value, SrcLoc loc);

/* Effect clauses */
EffectClause *ast_effect_clause(Arena *a, EffectLevel level,
                                const char *model_name /* nullable */,
                                RetryPolicy retry, SrcLoc loc);

/* Declarations */
TypeDecl *ast_type_decl_record(Arena *a, const char *name,
                               Field **fields, size_t n, SrcLoc loc);
TypeDecl *ast_type_decl_sum   (Arena *a, const char *name,
                               Variant **variants, size_t n, SrcLoc loc);
ToolDecl *ast_tool_decl(Arena *a, const char *name, Param **params, size_t n,
                        Type *return_type,
                        EffectClause *effect /* nullable; resolver checks */,
                        SrcLoc loc);
FlowDecl *ast_flow_decl(Arena *a, const char *name,
                        int implicit_it, Param **params, size_t n_params,
                        Type *return_type,
                        Binding **bindings, size_t n_bindings,
                        Expr *return_expr,
                        SrcLoc loc);

Decl *ast_decl_type(Arena *a, TypeDecl *t);
Decl *ast_decl_tool(Arena *a, ToolDecl *t);
Decl *ast_decl_flow(Arena *a, FlowDecl *f);

Program *ast_program(Arena *a, const char *source_file,
                     Decl **decls, size_t n);


/* ====================================================================
 * Scratch lists
 *
 * The parser builds variable-length child arrays incrementally as it
 * shifts tokens (it does not know n until it sees the closing
 * bracket). The ast_list family provides a small growable
 * pointer-array backed by malloc(3), not the arena, because the
 * scratch lifetime is short and the geometric growth would waste
 * arena space.
 *
 * Pattern:
 *
 *     AstList xs;
 *     ast_list_init(&xs);
 *     while (...) {
 *         ast_list_push(&xs, item);
 *     }
 *     SomeNode *node = ast_xyz(arena, ..., (Item **)xs.items, xs.n, ...);
 *     ast_list_finalize(&xs, arena);
 *
 * ast_list_finalize copies the items into the arena and frees the
 * malloc'd backing store. The result is a stable pointer that lives
 * with the arena. If a parse error aborts before finalize, call
 * ast_list_free to release the scratch without writing to the
 * arena.
 * ==================================================================== */

typedef struct {
    void   **items;
    size_t   n;
    size_t   cap;
} AstList;

void   ast_list_init    (AstList *xs);
void   ast_list_push    (AstList *xs, void *item);
void **ast_list_finalize(AstList *xs, Arena *a);   /* returns arena copy */
void   ast_list_free    (AstList *xs);             /* discards w/o copy */


/* ====================================================================
 * Type printing
 *
 * Canonical textual form of a Type expression, used in diagnostic
 * messages and in the IR emitter. The result is arena-allocated.
 * Format:
 *
 *     primitives:   "string" | "int" | "float" | "bool"
 *     named:        the identifier
 *     list:         "[" + element + "]"
 * ==================================================================== */

const char *ast_type_to_string(Arena *a, const Type *t);


/* ====================================================================
 * Operator names
 *
 * Small helpers used by the resolver, checker, and IR emitter so they
 * agree on the textual spelling of operators. Strings are static and
 * do not need to be freed.
 * ==================================================================== */

const char *ast_cmp_op_str  (CmpOp op);    /* "<=", ">=", "<", ">", "==", "!=" */
const char *ast_sort_dir_str(SortDir d);   /* "asc" | "desc" */

/* Upper bound on the number of match binders that may appear in
 * nested match arms inside `e`. Resolver and checker use this to
 * size their scope arrays so binder push/pop never overflows.
 * Returning a sum across the whole tree is a generous upper bound;
 * actual live-at-once count is bounded by the deepest nesting. */
size_t ast_count_match_binders(const Expr *e);

/* Worst-case match-binder count across a flow body: the sum of
 * ast_count_match_binders over every binding value and the return
 * expression. Resolver and checker add this to their scope capacity
 * so binder push/pop never overflows. */
size_t ast_count_flow_binders(const FlowDecl *f);

#endif /* FLOWC_AST_H */
