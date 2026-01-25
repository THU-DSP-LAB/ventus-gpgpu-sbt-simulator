#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$ROOT_DIR/build"
PTX_IN="$ROOT_DIR/ptx/vecadd.ptx"

ARCH="${ARCH:-sm_89}"

mkdir -p "$OUT_DIR"

# Compile PTX to a cubin (so we can load it reliably via cuModuleLoadData)
/usr/local/cuda/bin/nvcc -arch="$ARCH" -cubin "$PTX_IN" -o "$OUT_DIR/vecadd.cubin" -lineinfo

/usr/local/cuda/bin/nvcc --version | head -n 5 > "$OUT_DIR/toolchain.txt"
/usr/local/cuda/bin/ptxas --version | head -n 5 >> "$OUT_DIR/toolchain.txt"

# Build host runner (uses CUDA Driver API)
/usr/local/cuda/bin/nvcc -arch="$ARCH" -O2 "$ROOT_DIR/run_vecadd_ptx.cc" -o "$OUT_DIR/run_vecadd_ptx" -lcuda

echo "Built: $OUT_DIR/run_vecadd_ptx"