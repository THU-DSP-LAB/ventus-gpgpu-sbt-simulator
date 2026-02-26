#!/usr/bin/env bash
set -euo pipefail

# 背景
# 本仓库当前回归入口分散在 tools/ 下多个脚本/可执行文件中（compile-first、PDS、want、一致性、microtest gate、端到端）。
# 为降低“跑回归”成本与避免遗漏，这里提供一个统一的入口，按 preset 聚合调用既有回归。
#
# 需求/作用
# - 提供单命令统一回归入口：可选 quick/all/e2e 等 preset。
# - 不引入“静默兜底”：任何失败应明确报错并以非 0 退出码退出。
# - 复用已有回归脚本/可执行文件，不改动 ventus-env。
#
# 用法
#   tools/regress.sh --preset quick
#   tools/regress.sh --preset all --arch sm_75
#   tools/regress.sh --preset e2e --e2e-runner profile
#   tools/regress.sh --preset e2e --e2e-runner ventus-env --jobs 8 --timeout-scale 1.0
#
# 关键参数：
# - --preset {quick|all|e2e}
# - --arch {75|sm_75|89|sm_89}   (传给 ptxas 的 -arch)
# - --e2e-runner {profile|ventus-env}
# - --build / --no-build
# - --timeout-scale <float>      (传给端到端 runner)
# - --jobs <int>                 (仅对 ventus-env/regression-test.py 生效)
#
# 实现原理/处理步骤
# 1) 解析参数，确定需要运行的 suite/preset。
# 2) 按需执行 cmake configure/build，确保回归依赖的二进制存在。
# 3) 依次调用既有 smoke/gate/e2e 入口；任何一步失败立即退出并打印失败点。

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"

PRESET="quick"
ARCH="${ARCH:-}"
E2E_RUNNER="profile"
DO_BUILD="auto" # auto|yes|no
TIMEOUT_SCALE="1.0"
JOBS=""

usage() {
  cat >&2 <<'EOF'
Usage: tools/regress.sh [options]

Options:
  --preset {quick|all|e2e}         Which regression preset to run (default: quick)
  --arch {75|sm_75|89|sm_89}       ptxas -arch (default: env ARCH or env VENTUS_PTX_SM or 75)
  --e2e-runner {profile|ventus-env}End-to-end runner (default: profile)
  --timeout-scale <float>          Timeout scale for end-to-end runner (default: 1.0)
  --jobs <int>                     Parallel jobs for ventus-env runner (optional)
  --build                          Force cmake configure/build
  --no-build                       Do not build (error if required binaries missing)
  -h, --help                       Show this help
EOF
}

die() {
  echo "ERROR: $*" >&2
  exit 2
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing command: $1"
}

run_step() {
  local name="$1"
  shift
  echo "[RUN] $name"
  "$@"
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --preset)
        [[ $# -ge 2 ]] || die "--preset requires a value"
        PRESET="$2"
        shift 2
        ;;
      --arch)
        [[ $# -ge 2 ]] || die "--arch requires a value"
        ARCH="$2"
        shift 2
        ;;
      --e2e-runner)
        [[ $# -ge 2 ]] || die "--e2e-runner requires a value"
        E2E_RUNNER="$2"
        shift 2
        ;;
      --timeout-scale)
        [[ $# -ge 2 ]] || die "--timeout-scale requires a value"
        TIMEOUT_SCALE="$2"
        shift 2
        ;;
      --jobs)
        [[ $# -ge 2 ]] || die "--jobs requires a value"
        JOBS="$2"
        shift 2
        ;;
      --build)
        DO_BUILD="yes"
        shift
        ;;
      --no-build)
        DO_BUILD="no"
        shift
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        die "unknown argument: $1"
        ;;
    esac
  done
}

normalize_arch() {
  if [[ -n "${ARCH}" ]]; then
    :
  elif [[ -n "${VENTUS_PTX_SM:-}" ]]; then
    ARCH="${VENTUS_PTX_SM}"
  else
    ARCH="75"
  fi

  if [[ "${ARCH}" =~ ^[0-9]+$ ]]; then
    ARCH="sm_${ARCH}"
  fi
  [[ "${ARCH}" =~ ^sm_[0-9]+$ ]] || die "invalid --arch: ${ARCH} (expect 75 or sm_75)"
}

cmake_build_if_needed() {
  local need_bins=("$@")
  local missing=()
  local b
  for b in "${need_bins[@]}"; do
    if [[ ! -x "${b}" ]]; then
      missing+=("${b}")
    fi
  done

  if [[ "${DO_BUILD}" == "no" ]]; then
    if [[ ${#missing[@]} -ne 0 ]]; then
      die "required binaries missing (use --build): ${missing[*]}"
    fi
    return 0
  fi

  if [[ "${DO_BUILD}" == "auto" && ${#missing[@]} -eq 0 ]]; then
    return 0
  fi

  need_cmd cmake
  local jobs
  jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

  if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    run_step "cmake configure" cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
  fi
  run_step "cmake build" cmake --build "${BUILD_DIR}" -j "${jobs}"

  local still_missing=()
  for b in "${need_bins[@]}"; do
    if [[ ! -x "${b}" ]]; then
      still_missing+=("${b}")
    fi
  done
  if [[ ${#still_missing[@]} -ne 0 ]]; then
    die "required binaries still missing after build: ${still_missing[*]}"
  fi
}

run_regext_bundle_test() {
  run_step "regext_bundle_test" "${BUILD_DIR}/regext_bundle_test"
}

run_want_consistency() {
  run_step "want consistency smoke" bash "${ROOT_DIR}/tools/check_spike_want_consistency.sh"
}

run_compile_first_smoke() {
  need_cmd ptxas
  run_step "Rodinia compile-first smoke (ARCH=${ARCH})" env ARCH="${ARCH}" bash "${ROOT_DIR}/tools/rodinia_ptx_smoke.sh"
}

run_pds_smoke() {
  need_cmd ptxas
  run_step "PDS smoke (ARCH=${ARCH})" env ARCH="${ARCH}" bash "${ROOT_DIR}/tools/pds_ptx_smoke.sh"
}

run_microtest_gate() {
  run_step "microtest coverage gate (Spike vs PTX)" bash "${ROOT_DIR}/tools/microtest_coverage_gate.sh"
}

run_e2e_profile() {
  need_cmd python3
  run_step "end-to-end (tools/ventus_regression_profile.py)" \
    python3 "${ROOT_DIR}/tools/ventus_regression_profile.py" --clean --timeout-scale "${TIMEOUT_SCALE}"
}

run_e2e_ventus_env() {
  need_cmd python3
  local args=()
  if [[ -n "${JOBS}" ]]; then
    args+=("--jobs" "${JOBS}")
  fi
  echo "[RUN] end-to-end (ventus-env/regression-test.py)"
  # ventus-env runner uses -t for timeout scale
  (
    cd "${ROOT_DIR}/ventus-env"
    VENTUS_BACKEND="${VENTUS_BACKEND:-ptx}" python3 regression-test.py -t "${TIMEOUT_SCALE}" "${args[@]}"
  )
}

main() {
  parse_args "$@"
  normalize_arch

  case "${PRESET}" in
    quick|all|e2e) ;;
    *) die "invalid --preset: ${PRESET} (expect quick|all|e2e)" ;;
  esac

  case "${E2E_RUNNER}" in
    profile|ventus-env) ;;
    *) die "invalid --e2e-runner: ${E2E_RUNNER} (expect profile|ventus-env)" ;;
  esac

  # Build dependencies for all presets except a pure e2e run (which can be used to validate toolchain/runtime only).
  if [[ "${PRESET}" == "e2e" ]]; then
    if [[ "${DO_BUILD}" == "yes" ]]; then
      cmake_build_if_needed \
        "${BUILD_DIR}/sbt_decode" \
        "${BUILD_DIR}/sbt_ptx" \
        "${BUILD_DIR}/gen_spike_encoding_subset" \
        "${BUILD_DIR}/regext_bundle_test" \
        "${BUILD_DIR}/ventus_ocl_run"
    fi
  else
    cmake_build_if_needed \
      "${BUILD_DIR}/sbt_decode" \
      "${BUILD_DIR}/sbt_ptx" \
      "${BUILD_DIR}/gen_spike_encoding_subset" \
      "${BUILD_DIR}/regext_bundle_test" \
      "${BUILD_DIR}/ventus_ocl_run"
  fi

  if [[ "${PRESET}" == "quick" || "${PRESET}" == "all" ]]; then
    run_regext_bundle_test
    run_want_consistency
    run_compile_first_smoke
    run_pds_smoke
    run_microtest_gate
  fi

  if [[ "${PRESET}" == "all" || "${PRESET}" == "e2e" ]]; then
    case "${E2E_RUNNER}" in
      profile) run_e2e_profile ;;
      ventus-env) run_e2e_ventus_env ;;
    esac
  fi

  echo "ALL OK (preset=${PRESET})"
  return 0
}

main "$@"
