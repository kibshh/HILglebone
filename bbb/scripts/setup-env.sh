#!/usr/bin/env bash
# Create the BBB agent's local virtualenv, install the package in editable
# mode with dev deps, and activate the venv in the calling shell.
# Idempotent — re-running just re-installs whatever changed in pyproject.toml.
#
# MUST be sourced (so the activate persists in your shell):
#
#   source ./bbb/scripts/setup-env.sh
#
# Editor interpreter: bbb/.venv/bin/python

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BBB_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VENV_DIR="$BBB_DIR/.venv"

if [ ! -d "$VENV_DIR" ]; then
    echo "Creating venv at $VENV_DIR"
    # Created from /tmp so the cwd doesn't get scanned by stray imports.
    (cd /tmp && python3 -m venv "$VENV_DIR") || return 1
fi

echo "Installing hilglebone-bbb[dev] (editable) ..."
"$VENV_DIR/bin/pip" install --upgrade pip >/dev/null || return 1
"$VENV_DIR/bin/pip" install -e "$BBB_DIR[dev]" >/dev/null || return 1

# shellcheck source=/dev/null
source "$VENV_DIR/bin/activate"
echo "Done. venv activated; \`deactivate\` to leave."
