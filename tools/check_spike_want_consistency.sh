#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"

WANT_FILE="${WANT_FILE:-$ROOT_DIR/data/spike_want.txt}"
ENCODING_H="${ENCODING_H:-$ROOT_DIR/ventus-env/spike/riscv/encoding.h}"

SBT_DECODE="${SBT_DECODE:-$BUILD_DIR/sbt_decode}"
SBT_PTX="${SBT_PTX:-$BUILD_DIR/sbt_ptx}"
GEN_SUBSET="${GEN_SUBSET:-$BUILD_DIR/gen_spike_encoding_subset}"

ELF="${ELF:-$ROOT_DIR/ventus-env/rodinia/opencl/bfs/object0.riscv}"
FUNC="${FUNC:-BFS_1}"

export GPU_SBT_WANT_FILE="$WANT_FILE"

"$GEN_SUBSET" --encoding-h "$ENCODING_H" --out "$BUILD_DIR/spike_encoding_subset.hpp" >/dev/null
"$SBT_DECODE" decode "$ELF" --func "$FUNC" --require-known >/dev/null
"$SBT_PTX" "$ELF" --func "$FUNC" --require-known >/dev/null

echo "ok want consistency (want=$(basename "$WANT_FILE"))"

