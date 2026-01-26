#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

"$ROOT_DIR/build_vecadd.sh"

"$ROOT_DIR/build/run_vecadd_ptx" "$ROOT_DIR/build/vecadd.cubin" ventus_start