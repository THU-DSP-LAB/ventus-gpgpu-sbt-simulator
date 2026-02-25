#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"

EXE="${EXE:-$BUILD_DIR/ventus_ocl_run}"
SBT_DECODE="${SBT_DECODE:-$BUILD_DIR/sbt_decode}"
SRC="${SRC:-$ROOT_DIR/testcases/ocl_compare/kernels.cl}"
EXCEPTIONS="${EXCEPTIONS:-$ROOT_DIR/data/inst_exceptions.txt}"

# Minimal regression guard: prevent reintroducing explicit dead-only “coverage-only” blocks.
if command -v rg >/dev/null 2>&1; then
  if rg -n "coverage-only|never taken|included in the binary but never taken" "$SRC" >/dev/null; then
    echo "FAIL: dead-only coverage markers found in $SRC" >&2
    rg -n "coverage-only|never taken|included in the binary but never taken" "$SRC" >&2 || true
    exit 1
  fi
fi

# This script is intended as a regression gate entry-point.
python3 "$ROOT_DIR/tools/ventus_ocl_compare.py" \
  --exe "$EXE" \
  --sbt-decode "$SBT_DECODE" \
  --src "$SRC" \
  --n 32 \
  --coverage \
  --exceptions "$EXCEPTIONS" \
  --require-full \
  "$@"
