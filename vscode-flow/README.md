# vscode-flow

A VS Code extension for the Flow agent DSL. Adds syntax highlighting
for `.flow` files and a problem matcher that surfaces `flowc`
diagnostics in the editor. Pure JSON contributions: no extension code,
no `node_modules`, no build step.

This is a tokenizer paired with a build task, not a language server.
Real-time error checking, hover types, and completion are out of scope
here. The `flowc` build task below is how you get real diagnostics.

## Installing

Requires a POSIX bash and a working VS Code, VS Code Insiders, Cursor,
or VSCodium install.

```
./install.sh
```

Drops the extension into the editor's extensions directory and prints a
reload reminder. The script auto-detects stock VS Code first, then
Insiders, Cursor, and VSCodium in that order. Pass `--target DIR` to
force a specific location.

To wire up error squiggles in a workspace at the same time:

```
./install.sh --errors            # uses $PWD as the workspace
./install.sh --errors path/to/ws # or pass a path
```

That additionally writes `.vscode/tasks.json` in the workspace with a
`Flow: Compile current file` build task bound to the `$flowc` problem
matcher. Any existing `tasks.json` is backed up to `.bak.<timestamp>`.

Other modes:

```
./install.sh --symlink           # symlink instead of copy
./install.sh --uninstall         # remove a previously installed copy
./install.sh --help              # full option list
```

After installing, reload the editor (`Cmd+Shift+P → Developer: Reload
Window`) and open any `.flow` file.

## What it contributes

Three things, all declarative.

1. **A TextMate grammar** derived from the productions in
   `flowc/src/parse.y`. Identifiers are colored by context, not by
   case: an identifier after `->` or inside `[ ... ]` is a type, an
   identifier before `(` is a function call, an identifier before `{`
   is a constructor. Brace pairs are balanced via `begin`/`end` blocks
   so a constructor's inner `}` does not terminate the enclosing flow
   body.

2. **A problem matcher** named `flowc`. The regex matches `flowc`'s
   GCC-style diagnostic format:

   ```
   <path>:<line>:<col>: <severity>[<code>]: <message>
   ```

   Severity (`error`, `warning`, `note`) maps to VS Code's diagnostic
   levels. The E-code becomes the diagnostic's `code` field, visible in
   the Problems panel.

3. **A tasks template** at `tasks.template.json` that wires the matcher
   to a build task. The task runs

   ```
   ${workspaceFolder}/flowc/flowc --dump=checked ${file}
   ```

   `--dump=checked` stops the pipeline after type checking, so no IR is
   written and the only output of interest is the diagnostic stream on
   `stderr`. Edit `command` if `flowc` lives elsewhere (for example
   `flowc` if it is on `$PATH` after `make install`).

## Errors

With the task installed, **Cmd+Shift+B** runs `flowc` against the
active file. Any diagnostic appears

- in the **Problems panel** (Cmd+Shift+M) with the E-code visible,
- as an editor **squiggle** at the reported line and column, and
- under VS Code's matching severity (error vs. warning vs. note).

`flowc`'s exit code is not consulted; the matcher reads `stderr`
regardless. Diagnostics use the same line and column numbers `flowc`
itself prints, both 1-based and byte-oriented.

The non-ASCII illegal-byte rule (lexer error E101) is flagged directly
by the grammar via the `invalid.illegal.non-ascii.flow` scope, without
invoking `flowc`. Every other diagnostic — `E102`, `E103`, `E105`,
`E110`, the `E12x` resolver series, the `E14x` and `E15x` checker
series — requires the task.

## Auto-run on save

VS Code does not trigger tasks on save out of the box. The smallest
addition that does:

1. Install [Trigger Task on Save] (`Gruntfuggly.triggertaskonsave`)
   from the marketplace.

2. Add this to the workspace `.vscode/settings.json`:

   ```json
   "triggerTaskOnSave.tasks": {
     "Flow: Compile current file": ["**/*.flow"]
   }
   ```

Every save now re-runs `flowc` and refreshes the squiggles. The latency
is one compile per save, which is fast given `flowc`'s single-pass
arena-allocated pipeline.

[Trigger Task on Save]: https://marketplace.visualstudio.com/items?itemName=Gruntfuggly.triggertaskonsave

## Project layout

```
package.json                 # extension manifest: language, grammar,
                             # and problem-matcher contributions
language-configuration.json  # comment marker, bracket pairs, auto-close
syntaxes/
  flow.tmLanguage.json       # TextMate grammar, ~35 repository entries
tasks.template.json          # copy to <workspace>/.vscode/tasks.json
install.sh                   # POSIX bash installer; --help for options
```

## Scope

The grammar is structural and does not approximate semantics. It does
not, and is not intended to:

- type-check, resolve names, or report unresolved references
- show inferred types on hover
- offer completion for in-scope identifiers, tools, models, or fields
- update diagnostics as you type

Those features require a language server, which this extension does
not include. The `flowc` task is the next-best thing: real, accurate
diagnostics from the actual type checker, surfaced on demand or on save.
