#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PTX_DIR="$ROOT_DIR/ptx"
BUILD_DIR="$ROOT_DIR/build"

ARCH="${ARCH:-sm_89}"
PTXAS_O="${PTXAS_O:--O3}"

mkdir -p "$BUILD_DIR"

compile_one() {
  local ptx_file="$1"
  local base
  base="$(basename "$ptx_file" .ptx)"

  local cubin="$BUILD_DIR/${base}.cubin"
  local sass="$BUILD_DIR/${base}.sass"

  echo "[nvcc] $base ($ARCH)"
  nvcc -arch="$ARCH" -cubin "$ptx_file" -o "$cubin" -lineinfo -Xptxas "$PTXAS_O" 1>"$BUILD_DIR/${base}.nvcc.stdout" 2>"$BUILD_DIR/${base}.nvcc.stderr" || {
    echo "nvcc failed for $ptx_file; see $BUILD_DIR/${base}.nvcc.stderr" >&2
    return 1
  }

  echo "[cuobjdump] $base"
  cuobjdump --dump-sass "$cubin" > "$sass"
}

analyze() {
  local out="$BUILD_DIR/summary.txt"
  : > "$out"

  echo "ARCH=$ARCH" >> "$out"
  echo "PTXAS_O=$PTXAS_O" >> "$out"
  echo "CUDA_NVCC=$(command -v nvcc)" >> "$out"
  echo "" >> "$out"

  for sass in "$BUILD_DIR"/*.sass; do
    local base
    base="$(basename "$sass" .sass)"

    # count UR registers in operands
    local ur_count
    ur_count=$(grep -oE '\bUR[0-9]+' "$sass" | wc -l | tr -d ' ')

    # unique U* mnemonics (heuristic)
    local u_mnems
    u_mnems=$(awk '
      /\/\*[0-9a-fA-F][0-9a-fA-F]*\*\// {
        line=$0
        sub(/^[[:space:]]*\/\*[0-9a-fA-F][0-9a-fA-F]*\*\/[[:space:]]*/, "", line)
        n=split(line, a, /[[:space:]]+/)
        if (n >= 1) {
          mnem=a[1]
          if (mnem ~ /^U[A-Z0-9_.]*/) mnems[mnem]=1
        }
      }
      END {
        first=1
        for (k in mnems) {
          if (!first) printf(" ")
          printf("%s", k)
          first=0
        }
      }
    ' "$sass" | tr ' ' '\n' | sort -u | tr '\n' ' ')

    printf '%-22s  UR_refs=%-5s  U_mnems=%s\n' "$base" "$ur_count" "${u_mnems:-<none>}" >> "$out"
  done

  echo "" >> "$out"
  echo "Tip: 想看某个样例的关键行，可用：" >> "$out"
  echo "  grep -nE '\\bUR[0-9]+|\\bULDC\\b|\\bU[A-Z]' build/<name>.sass | head" >> "$out"
}

main() {
  echo "ROOT_DIR=$ROOT_DIR"
  echo "ARCH=$ARCH"

  shopt -s nullglob
  local ptx_files=("$PTX_DIR"/*.ptx)
  if [[ ${#ptx_files[@]} -eq 0 ]]; then
    echo "No PTX files under $PTX_DIR" >&2
    exit 1
  fi

  for f in "${ptx_files[@]}"; do
    compile_one "$f"
  done

  analyze
  echo "wrote $BUILD_DIR/summary.txt"
}

main "$@"
