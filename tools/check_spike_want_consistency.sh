#!/usr/bin/env bash
set -euo pipefail

# 背景
# - sbt_decode/sbt_ptx 的 Ventus patterns 子集在构建期生成并编译进二进制（不再运行期读取 want/encoding 文件）。
# - 当 want/encoding 变更或 build 产物过期时，可能出现“源码输入已变但 build/generated 仍是旧版本”的问题。
#
# 需求/作用
# - 用当前 want + encoding 重新生成 subset header，与 build-time 生成的 header 做 diff，防止“过期构建”导致行为偏差。
# - 同时覆盖一次最小 decode + ptx smoke，确保工具链仍可用。
#
# 用法
#   tools/check_spike_want_consistency.sh
#   BUILD_DIR=/path/to/build tools/check_spike_want_consistency.sh
#   ENCODING_H=/abs/path/to/encoding.h WANT_FILE=/abs/path/to/spike_want.txt tools/check_spike_want_consistency.sh
#
# 实现原理/处理步骤
# 1) 解析参数：默认从 build/CMakeCache.txt 读取 SBT_SPIKE_ENCODING_H，确保与构建配置一致。
# 2) 调用 gen_spike_encoding_subset 生成临时 header。
# 3) diff build/generated/spike_encoding_subset.hpp 与临时 header；不一致则报错提示 rebuild。
# 4) 运行 sbt_decode decode 与 sbt_ptx 的最小 smoke。

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"

WANT_FILE="${WANT_FILE:-$ROOT_DIR/data/spike_want.txt}"
if [[ -z "${ENCODING_H:-}" ]]; then
  ENCODING_H_FROM_CACHE=""
  if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    ENCODING_H_FROM_CACHE="$(
      grep -m1 '^SBT_SPIKE_ENCODING_H:FILEPATH=' "$BUILD_DIR/CMakeCache.txt" \
        | sed 's/^SBT_SPIKE_ENCODING_H:FILEPATH=//'
    )"
  fi
  ENCODING_H="${ENCODING_H_FROM_CACHE:-$ROOT_DIR/../spike/riscv/encoding.h}"
fi

SBT_DECODE="${SBT_DECODE:-$BUILD_DIR/sbt_decode}"
SBT_PTX="${SBT_PTX:-$BUILD_DIR/sbt_ptx}"
GEN_SUBSET="${GEN_SUBSET:-$BUILD_DIR/gen_spike_encoding_subset}"

ELF="${ELF:-$ROOT_DIR/../rodinia/opencl/bfs/object0.riscv}"
FUNC="${FUNC:-BFS_1}"

SUBSET_HEADER="${SUBSET_HEADER:-$BUILD_DIR/generated/spike_encoding_subset.hpp}"
TMP_HEADER="${TMP_HEADER:-$BUILD_DIR/spike_encoding_subset.check.hpp}"

if [[ ! -f "$WANT_FILE" ]]; then
  echo "ERROR: missing WANT_FILE: $WANT_FILE" >&2
  exit 2
fi
if [[ ! -f "$ENCODING_H" ]]; then
  echo "ERROR: missing ENCODING_H: $ENCODING_H" >&2
  exit 2
fi
if [[ ! -x "$GEN_SUBSET" ]]; then
  echo "ERROR: missing GEN_SUBSET exe: $GEN_SUBSET" >&2
  exit 2
fi
if [[ ! -x "$SBT_DECODE" ]]; then
  echo "ERROR: missing SBT_DECODE exe: $SBT_DECODE" >&2
  exit 2
fi
if [[ ! -x "$SBT_PTX" ]]; then
  echo "ERROR: missing SBT_PTX exe: $SBT_PTX" >&2
  exit 2
fi

"$GEN_SUBSET" --encoding-h "$ENCODING_H" --want-file "$WANT_FILE" --out "$TMP_HEADER" >/dev/null

if [[ ! -f "$SUBSET_HEADER" ]]; then
  echo "ERROR: missing build-time subset header: $SUBSET_HEADER" >&2
  echo "Hint: rebuild to generate it (cmake --build \"$BUILD_DIR\")" >&2
  exit 2
fi

if ! diff -u "$SUBSET_HEADER" "$TMP_HEADER"; then
  echo "ERROR: subset header mismatch: build-time header != freshly generated header" >&2
  echo "Hint: your build may be stale; rebuild and retry." >&2
  exit 2
fi

"$SBT_DECODE" decode "$ELF" --func "$FUNC" --require-known >/dev/null
"$SBT_PTX" "$ELF" --func "$FUNC" --require-known >/dev/null

echo "ok want consistency (want=$(basename "$WANT_FILE"))"
