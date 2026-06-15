#!/usr/bin/env bash
# Regenerate Go code from the .proto files. Run from any directory.
# Requires `protoc` and `protoc-gen-go` on PATH.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKEND_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROTO_DIR="$(cd "$BACKEND_DIR/../proto" && pwd)"
OUT_DIR="$BACKEND_DIR/gen"

if ! command -v protoc >/dev/null; then
    echo "protoc not found on PATH" >&2
    exit 1
fi
if ! command -v protoc-gen-go >/dev/null; then
    echo "protoc-gen-go not found on PATH (try: go install google.golang.org/protobuf/cmd/protoc-gen-go@latest)" >&2
    exit 1
fi

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

protoc \
    --proto_path="$PROTO_DIR" \
    --go_out="$OUT_DIR" \
    --go_opt=paths=source_relative \
    "$PROTO_DIR"/hilglebone/v1/*.proto

echo "Generated Go code in $OUT_DIR"
