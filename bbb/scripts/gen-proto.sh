#!/usr/bin/env bash
# Regenerate Python code from the shared .proto files. Run from anywhere.
# Requires `protoc` on PATH.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BBB_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROTO_DIR="$(cd "$BBB_DIR/../proto" && pwd)"
# Generated code lands at bbb/hilglebone/v1/ so the absolute imports protoc
# emits (`from hilglebone.v1 import …_pb2`) resolve via the editable install.
OUT_DIR="$BBB_DIR"
PKG_DIR="$OUT_DIR/hilglebone"

if ! command -v protoc >/dev/null; then
    echo "protoc not found on PATH" >&2
    exit 1
fi

rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR/v1"
touch "$PKG_DIR/__init__.py"
touch "$PKG_DIR/v1/__init__.py"

protoc \
    --proto_path="$PROTO_DIR" \
    --python_out="$OUT_DIR" \
    --pyi_out="$OUT_DIR" \
    "$PROTO_DIR"/hilglebone/v1/*.proto

echo "Generated Python proto code in $PKG_DIR"
