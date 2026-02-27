#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"

SBT_PTX="${SBT_PTX:-$BUILD_DIR/sbt_ptx}"
PTXAS="${PTXAS:-ptxas}"

# Accept ARCH=89 or ARCH=sm_89. Default aligns with CUDA 13.x ptxas minimum.
ARCH="${ARCH:-75}"
if [[ "$ARCH" =~ ^[0-9]+$ ]]; then
  ARCH="sm_${ARCH}"
fi

OUT_DIR="${OUT_DIR:-$ROOT_DIR/build/ptx}"
mkdir -p "$OUT_DIR"

declare -a kernels=(
  "../rodinia/opencl/b+tree/object0.riscv findRangeK"
  "../rodinia/opencl/b+tree/object1.riscv findK"
  "../rodinia/opencl/backprop/object0.riscv bpnn_layerforward_ocl"
  "../rodinia/opencl/backprop/object0.riscv bpnn_adjust_weights_ocl"
  "../rodinia/opencl/bfs/object0.riscv BFS_1"
  "../rodinia/opencl/bfs/object0.riscv BFS_2"
  "../rodinia/opencl/gaussian/object0.riscv Fan1"
  "../rodinia/opencl/gaussian/object0.riscv Fan2"
  "../rodinia/opencl/kmeans/object0.riscv kmeans_kernel_c"
  "../rodinia/opencl/kmeans/object0.riscv kmeans_swap"
  "../rodinia/opencl/nn/object0.riscv NearestNeighbor"
)

for entry in "${kernels[@]}"; do
  elf="$(echo "$entry" | awk '{print $1}')"
  fn="$(echo "$entry" | awk '{print $2}')"

  ptx="$OUT_DIR/${fn}.ptx"
  cubin="$OUT_DIR/${fn}.cubin"

  "$SBT_PTX" "$elf" --func "$fn" --require-known --out "$ptx" >/dev/null
  "$PTXAS" -arch="$ARCH" "$ptx" -o "$cubin" >/dev/null
  echo "ok $fn"
done

echo "ALL OK (ARCH=$ARCH, OUT_DIR=$OUT_DIR)"
