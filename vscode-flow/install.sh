#!/usr/bin/env bash
# Install (or uninstall) the Flow VS Code syntax extension.
#
# This is a "user-folder install": the extension is dropped into your editor's
# extensions directory and picked up on the next reload. No build step.

set -euo pipefail

VERSION="0.0.1"                                   # keep in sync with package.json
EXT_NAME="flow-syntax-${VERSION}"
SOURCE_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

MODE="copy"
ACTION="install"
TARGET_DIR=""
WORKSPACE_DIR=""

usage() {
  cat <<'EOF'
Install the Flow VS Code syntax extension.

Usage:
  ./install.sh                  install (copy) into your editor's extensions dir
  ./install.sh --symlink        install via symlink (for active development)
  ./install.sh --uninstall      remove a previously installed copy
  ./install.sh --target DIR     use DIR instead of auto-detecting
                                  ~/.vscode/extensions           VS Code (stock)
                                  ~/.vscode-insiders/extensions  VS Code Insiders
                                  ~/.cursor/extensions           Cursor
                                  ~/.vscode-oss/extensions       VSCodium
  ./install.sh --errors [DIR]   in addition to installing the extension, copy
                                tasks.template.json -> DIR/.vscode/tasks.json
                                so Cmd+Shift+B runs flowc and surfaces real
                                errors in the Problems panel. DIR defaults to
                                the current directory.
  ./install.sh --help           show this help

After install, reload your editor:
  Cmd+Shift+P  ->  Developer: Reload Window
Then open any .flow file to verify highlighting.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --symlink)   MODE="symlink"; shift ;;
    --copy)      MODE="copy"; shift ;;
    --uninstall) ACTION="uninstall"; shift ;;
    --target)    TARGET_DIR="${2:-}"; shift 2 ;;
    --errors)
      # Optional positional arg: workspace dir. Defaults to $PWD if next arg
      # is missing or looks like another flag.
      if [[ $# -ge 2 && "${2:-}" != --* ]]; then
        WORKSPACE_DIR="$2"; shift 2
      else
        WORKSPACE_DIR="$PWD"; shift
      fi
      ;;
    --help|-h)   usage; exit 0 ;;
    *)           echo "unknown argument: $1" >&2; usage; exit 1 ;;
  esac
done

# Auto-detect: prefer stock VS Code, fall back to the most common forks.
if [[ -z "$TARGET_DIR" ]]; then
  for candidate in \
      "$HOME/.vscode/extensions" \
      "$HOME/.vscode-insiders/extensions" \
      "$HOME/.cursor/extensions" \
      "$HOME/.vscode-oss/extensions"; do
    if [[ -d "$candidate" ]]; then
      TARGET_DIR="$candidate"
      break
    fi
  done
fi

if [[ -z "$TARGET_DIR" ]]; then
  echo "error: could not find a VS Code extensions directory." >&2
  echo "       pass one with --target, e.g.  --target ~/.vscode/extensions" >&2
  exit 1
fi

# Expand ~ if the user passed --target ~/something (bash doesn't expand inside
# quoted CLI args).
TARGET_DIR="${TARGET_DIR/#\~/$HOME}"

if [[ ! -d "$TARGET_DIR" ]]; then
  echo "error: $TARGET_DIR is not a directory" >&2
  exit 1
fi

INSTALL_PATH="${TARGET_DIR}/${EXT_NAME}"

if [[ "$ACTION" == "uninstall" ]]; then
  if [[ -e "$INSTALL_PATH" || -L "$INSTALL_PATH" ]]; then
    rm -rf "$INSTALL_PATH"
    echo "removed: $INSTALL_PATH"
  else
    echo "nothing installed at: $INSTALL_PATH"
  fi
  exit 0
fi

# Replace any previous install at this path (could be a stale symlink or copy).
if [[ -e "$INSTALL_PATH" || -L "$INSTALL_PATH" ]]; then
  rm -rf "$INSTALL_PATH"
fi

case "$MODE" in
  symlink)
    ln -s "$SOURCE_DIR" "$INSTALL_PATH"
    echo "symlinked $INSTALL_PATH -> $SOURCE_DIR"
    ;;
  copy)
    mkdir -p "$INSTALL_PATH"
    cp    "$SOURCE_DIR/package.json"                "$INSTALL_PATH/"
    cp    "$SOURCE_DIR/language-configuration.json" "$INSTALL_PATH/"
    cp -R "$SOURCE_DIR/syntaxes"                    "$INSTALL_PATH/"
    echo "installed at $INSTALL_PATH"
    ;;
esac

if [[ -n "$WORKSPACE_DIR" ]]; then
  WORKSPACE_DIR="${WORKSPACE_DIR/#\~/$HOME}"
  if [[ ! -d "$WORKSPACE_DIR" ]]; then
    echo "error: --errors target is not a directory: $WORKSPACE_DIR" >&2
    exit 1
  fi
  TASKS_TARGET="$WORKSPACE_DIR/.vscode/tasks.json"
  mkdir -p "$WORKSPACE_DIR/.vscode"
  if [[ -e "$TASKS_TARGET" ]]; then
    BACKUP="$TASKS_TARGET.bak.$(date +%s)"
    mv "$TASKS_TARGET" "$BACKUP"
    echo "existing $TASKS_TARGET backed up to $BACKUP"
  fi
  cp "$SOURCE_DIR/tasks.template.json" "$TASKS_TARGET"
  echo "wrote $TASKS_TARGET"
fi

echo
echo "next: reload your editor (Cmd+Shift+P -> 'Developer: Reload Window')"
echo "      then open any .flow file to verify highlighting."
if [[ -n "$WORKSPACE_DIR" ]]; then
  echo "      Cmd+Shift+B in that workspace runs flowc and surfaces errors."
fi
