#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$ROOT_DIR/build"

"$ROOT_DIR/build_simple.sh"

"$OUT_DIR/run_simple_ptx" "$OUT_DIR/simple.cubin" vecadd_simple
