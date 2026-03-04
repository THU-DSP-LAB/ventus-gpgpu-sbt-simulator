#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"

SBT_DECODE="${SBT_DECODE:-$BUILD_DIR/sbt_decode}"
SBT_PTX="${SBT_PTX:-$BUILD_DIR/sbt_ptx}"
PTXAS="${PTXAS:-ptxas}"

# Accept ARCH=89 or ARCH=sm_89. Default aligns with CUDA 13.x ptxas minimum.
ARCH="${ARCH:-75}"
if [[ "$ARCH" =~ ^[0-9]+$ ]]; then
  ARCH="sm_${ARCH}"
fi

OUT_DIR="${OUT_DIR:-$ROOT_DIR/build/ptx}"
mkdir -p "$OUT_DIR"

declare -a candidates=(
  "../rodinia/opencl/nn/object0.riscv NearestNeighbor"
  "../rodinia/opencl/backprop/object0.riscv bpnn_layerforward_ocl"
  "../rodinia/opencl/b+tree/object0.riscv findRangeK"
)

picked_elf=""
picked_fn=""
for entry in "${candidates[@]}"; do
  elf="$(echo "$entry" | awk '{print $1}')"
  fn="$(echo "$entry" | awk '{print $2}')"
  if "$SBT_DECODE" pretty "$elf" --func "$fn" --require-known 2>/dev/null | rg -q "\\b(vlw\\.v|vsw\\.v)\\b"; then
    picked_elf="$elf"
    picked_fn="$fn"
    break
  fi
done

if [[ -z "$picked_elf" || -z "$picked_fn" ]]; then
  echo "ERROR: no candidate kernel contains vlw.v/vsw.v (update tools/pds_ptx_smoke.sh candidates)" >&2
  exit 2
fi

ptx="$OUT_DIR/${picked_fn}.ptx"
cubin="$OUT_DIR/${picked_fn}.cubin"

"$SBT_PTX" "$picked_elf" --func "$picked_fn" --require-known --out "$ptx" >/dev/null

if rg -q "\\.local .*__sbt_pds" "$ptx"; then
  echo "ERROR: unexpected .local __sbt_pds backing in generated PTX: $ptx" >&2
  exit 3
fi

rg -q "\\.param \\.u32 pds_base_vaddr," "$ptx"
rg -q "\\.param \\.u32 pds_size_per_thread," "$ptx"
rg -q "\\.param \\.u32 pds_bitmap_base_vaddr," "$ptx"
rg -q "\\.param \\.u32 pds_pool_num_blocks" "$ptx"

"$PTXAS" -arch="$ARCH" "$ptx" -o "$cubin" >/dev/null
echo "ok PDS smoke: $picked_fn (ARCH=$ARCH)"
