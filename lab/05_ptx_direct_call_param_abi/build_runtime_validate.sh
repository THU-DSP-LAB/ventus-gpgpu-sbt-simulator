#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
OUT_DIR=${1:-"$SCRIPT_DIR/build_runtime_validate"}
mkdir -p "$OUT_DIR"

g++ \
  -std=c++17 \
  -O2 \
  "$SCRIPT_DIR/runtime_validate.cc" \
  -I/usr/local/cuda/include \
  -L/usr/lib/x86_64-linux-gnu \
  -lcuda \
  -o "$OUT_DIR/runtime_validate"

echo "built $OUT_DIR/runtime_validate"
