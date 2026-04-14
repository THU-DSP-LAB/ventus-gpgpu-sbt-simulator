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
#   tools/regress.sh --preset all --arch sm_89
#   tools/regress.sh --preset e2e --e2e-runner profile
#   tools/regress.sh --preset e2e --e2e-runner ventus-env --jobs 8 --timeout-scale 1.0
#   tools/regress.sh --preset quick --in-place
#   tools/regress.sh --preset quick --workdir /tmp/sbtsim-regress --keep-workdir
#
# 关键参数：
# - --preset {quick|all|e2e}
# - --arch {89|sm_89}            (传给 ptxas 的 -arch)
# - --e2e-runner {profile|ventus-env}
# - --build / --no-build
# - --timeout-scale <float>      (传给端到端 runner)
# - --jobs <int>                 (仅对 ventus-env/regression-test.py 生效)
# - 默认在临时目录运行并自动清理，可用 --in-place/--workdir/--keep-workdir 覆盖
#
# 实现原理/处理步骤
# 1) 解析参数，确定需要运行的 suite/preset。
# 2) 默认切到临时工作目录执行，并在退出时自动清理（除非显式要求保留）。
# 3) 按需执行 cmake configure/build，确保回归依赖的二进制存在。
# 4) 依次调用既有 smoke/gate/e2e 入口（含 custom MMA / non-MMA oracle gate）；任何一步失败立即退出并打印失败点。

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
ORIG_CWD="$(pwd)"

PRESET="quick"
ARCH="${ARCH:-}"
E2E_RUNNER="profile"
DO_BUILD="auto" # auto|yes|no
TIMEOUT_SCALE="1.0"
JOBS=""
MMA_STAGE="full"
WORKDIR_MODE="temp" # temp|inplace|custom
WORKDIR_OPTION_SET="no"
WORKDIR_PATH=""
KEEP_WORKDIR="no"

RUN_DIR=""
TEMP_RUN_DIR=""

usage() {
  cat >&2 <<'EOF'
Usage: tools/regress.sh [options]

Options:
  --preset {quick|all|e2e}         Which regression preset to run (default: quick)
  --arch {89|sm_89}                 ptxas -arch (default: env ARCH or env VENTUS_PTX_SM or 89)
  --e2e-runner {profile|ventus-env}End-to-end runner (default: profile)
  --timeout-scale <float>          Timeout scale for end-to-end runner (default: 1.0)
  --jobs <int>                     Parallel jobs for ventus-env runner (optional)
  --mma-stage {spike-precheck|compile-first|full}
                                   MMA gate stage (default: full)
  --build                          Force cmake configure/build
  --no-build                       Do not build (error if required binaries missing)
  --in-place                       Run in current directory (legacy behavior)
  --workdir <path>                 Run in given directory (auto create, do not auto delete)
  --keep-workdir                   Keep temp directory after run (only for default temp mode)
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

run_step_in_root() {
  local name="$1"
  shift
  echo "[RUN] $name (cwd=${ROOT_DIR})"
  (
    cd "${ROOT_DIR}"
    "$@"
  )
}

set_workdir_mode() {
  local mode="$1"
  local flag_name="$2"
  if [[ "${WORKDIR_OPTION_SET}" == "yes" && "${WORKDIR_MODE}" != "${mode}" ]]; then
    die "conflicting workdir mode options (current=${WORKDIR_MODE}, new=${flag_name})"
  fi
  WORKDIR_MODE="${mode}"
  WORKDIR_OPTION_SET="yes"
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
      --mma-stage)
        [[ $# -ge 2 ]] || die "--mma-stage requires a value"
        MMA_STAGE="$2"
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
      --in-place)
        set_workdir_mode "inplace" "--in-place"
        shift
        ;;
      --workdir)
        [[ $# -ge 2 ]] || die "--workdir requires a value"
        set_workdir_mode "custom" "--workdir"
        WORKDIR_PATH="$2"
        shift 2
        ;;
      --keep-workdir)
        KEEP_WORKDIR="yes"
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
    ARCH="89"
  fi

  if [[ "${ARCH}" =~ ^[0-9]+$ ]]; then
    ARCH="sm_${ARCH}"
  fi
  [[ "${ARCH}" =~ ^sm_[0-9]+$ ]] || die "invalid --arch: ${ARCH} (expect 89 or sm_89)"
}

setup_workdir() {
  case "${WORKDIR_MODE}" in
    temp)
      RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/sbtsim-regress.XXXXXX")"
      TEMP_RUN_DIR="${RUN_DIR}"
      ;;
    inplace)
      RUN_DIR="${ORIG_CWD}"
      ;;
    custom)
      [[ -n "${WORKDIR_PATH}" ]] || die "--workdir mode selected but path is empty"
      if [[ "${WORKDIR_PATH}" != /* ]]; then
        WORKDIR_PATH="${ORIG_CWD}/${WORKDIR_PATH}"
      fi
      mkdir -p "${WORKDIR_PATH}"
      RUN_DIR="$(cd "${WORKDIR_PATH}" && pwd)"
      ;;
    *)
      die "invalid workdir mode: ${WORKDIR_MODE}"
      ;;
  esac

  if [[ "${KEEP_WORKDIR}" == "yes" && "${WORKDIR_MODE}" != "temp" ]]; then
    echo "[INFO] --keep-workdir is ignored when mode=${WORKDIR_MODE}"
  fi

  echo "[INFO] workdir mode=${WORKDIR_MODE} path=${RUN_DIR}"
  cd "${RUN_DIR}"
}

cleanup_workdir() {
  local rc=$?
  local cleanup_rc=0

  if ! cd "${ORIG_CWD}" >/dev/null 2>&1; then
    echo "ERROR: failed to restore cwd: ${ORIG_CWD}" >&2
    cleanup_rc=1
  fi

  if [[ -n "${TEMP_RUN_DIR}" ]]; then
    if [[ "${KEEP_WORKDIR}" == "yes" ]]; then
      echo "[INFO] kept temp workdir: ${TEMP_RUN_DIR}"
    else
      if rm -rf "${TEMP_RUN_DIR}"; then
        echo "[INFO] removed temp workdir: ${TEMP_RUN_DIR}"
      else
        echo "ERROR: failed to remove temp workdir: ${TEMP_RUN_DIR}" >&2
        cleanup_rc=1
      fi
    fi
  fi

  trap - EXIT
  if [[ "${rc}" -ne 0 ]]; then
    exit "${rc}"
  fi
  exit "${cleanup_rc}"
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
  # regext_bundle_test currently uses default ../spike/riscv/encoding.h relative to cwd.
  run_step_in_root "regext_bundle_test" "${BUILD_DIR}/regext_bundle_test"
}

run_custom_unit_tests() {
  run_step_in_root "custom_decode_test" "${BUILD_DIR}/custom_decode_test"
  run_step_in_root "mma_decode_test" "${BUILD_DIR}/mma_decode_test"
  run_step_in_root "custom_ptx_emit_test" "${BUILD_DIR}/custom_ptx_emit_test"
  run_step_in_root "mma_ptx_emit_test" "${BUILD_DIR}/mma_ptx_emit_test"
}

run_custom_non_mma_oracle_gate() {
  need_cmd python3
  need_cmd ptxas
  run_step "custom non-MMA oracle gate (ARCH=${ARCH})" \
    python3 "${ROOT_DIR}/tools/custom_non_mma_oracle.py" --sm "${ARCH}" --spike-compat-nested-regext
}

run_custom_mma_oracle_gate() {
  need_cmd python3
  # custom_mma_oracle.py reports per-kernel PASS/BLOCK/FAIL and exits non-zero on any real failure.
  run_step "custom MMA oracle gate (ARCH=${ARCH}, stage=${MMA_STAGE})" \
    python3 "${ROOT_DIR}/tools/custom_mma_oracle.py" --sm "${ARCH}" --stage "${MMA_STAGE}" --spike-compat-nested-regext
}

run_want_consistency() {
  # check_spike_want_consistency.sh regenerates the build-time subset header and diffs it with build outputs.
  run_step_in_root "want consistency smoke" bash "${ROOT_DIR}/tools/check_spike_want_consistency.sh"
}

run_compile_first_smoke() {
  need_cmd ptxas
  # rodinia_ptx_smoke.sh relies on sbt_ptx default encoding-h and relative Rodinia ELF paths.
  run_step_in_root "Rodinia compile-first smoke (ARCH=${ARCH})" env ARCH="${ARCH}" bash "${ROOT_DIR}/tools/rodinia_ptx_smoke.sh"
}

run_pds_smoke() {
  need_cmd ptxas
  # pds_ptx_smoke.sh relies on sbt_decode/sbt_ptx default encoding-h and relative Rodinia ELF paths.
  run_step_in_root "PDS smoke (ARCH=${ARCH})" env ARCH="${ARCH}" bash "${ROOT_DIR}/tools/pds_ptx_smoke.sh"
}

run_microtest_gate() {
  run_step "microtest coverage gate (Spike vs PTX)" bash "${ROOT_DIR}/tools/microtest_coverage_gate.sh"
}

run_e2e_profile() {
  need_cmd python3
  [[ -x "${BUILD_DIR}/sbt_ptx" ]] || die "missing current-tree sbt_ptx: ${BUILD_DIR}/sbt_ptx"
  run_step "end-to-end (tools/ventus_regression_profile.py)" \
    env GPU_SBT_PTX="${BUILD_DIR}/sbt_ptx" python3 "${ROOT_DIR}/tools/ventus_regression_profile.py" \
      --ventus-root "${ROOT_DIR}/.." \
      --clean \
      --timeout-scale "${TIMEOUT_SCALE}"
}

run_e2e_ventus_env() {
  need_cmd python3
  [[ -x "${BUILD_DIR}/sbt_ptx" ]] || die "missing current-tree sbt_ptx: ${BUILD_DIR}/sbt_ptx"
  local args=()
  if [[ -n "${JOBS}" ]]; then
    args+=("--jobs" "${JOBS}")
  fi
  local ventus_root="${ROOT_DIR}/.."
  if [[ ! -f "${ventus_root}/regression-test.py" ]]; then
    die "ventus-env regression-test.py not found: ${ventus_root}/regression-test.py (expected ventus-env at ..)"
  fi
  echo "[RUN] end-to-end (ventus-env/regression-test.py)"
  # ventus-env runner uses -t for timeout scale
  (
    cd "${ventus_root}"
    GPU_SBT_PTX="${BUILD_DIR}/sbt_ptx" VENTUS_BACKEND="${VENTUS_BACKEND:-ptx}" python3 regression-test.py -t "${TIMEOUT_SCALE}" "${args[@]}"
  )
}

main() {
  parse_args "$@"
  normalize_arch
  trap cleanup_workdir EXIT

  case "${PRESET}" in
    quick|all|e2e) ;;
    *) die "invalid --preset: ${PRESET} (expect quick|all|e2e)" ;;
  esac

  case "${E2E_RUNNER}" in
    profile|ventus-env) ;;
    *) die "invalid --e2e-runner: ${E2E_RUNNER} (expect profile|ventus-env)" ;;
  esac
  case "${MMA_STAGE}" in
    spike-precheck|compile-first|full) ;;
    *) die "invalid --mma-stage: ${MMA_STAGE} (expect spike-precheck|compile-first|full)" ;;
  esac

  setup_workdir

  # Build dependencies for all presets except a pure e2e run (which can be used to validate toolchain/runtime only).
  if [[ "${PRESET}" == "e2e" ]]; then
    if [[ "${DO_BUILD}" == "yes" ]]; then
      cmake_build_if_needed \
        "${BUILD_DIR}/sbt_decode" \
        "${BUILD_DIR}/sbt_ptx" \
        "${BUILD_DIR}/gen_spike_encoding_subset" \
        "${BUILD_DIR}/regext_bundle_test" \
        "${BUILD_DIR}/custom_decode_test" \
        "${BUILD_DIR}/mma_decode_test" \
        "${BUILD_DIR}/custom_ptx_emit_test" \
        "${BUILD_DIR}/mma_ptx_emit_test" \
        "${BUILD_DIR}/ventus_ocl_run"
    fi
  else
    cmake_build_if_needed \
      "${BUILD_DIR}/sbt_decode" \
      "${BUILD_DIR}/sbt_ptx" \
      "${BUILD_DIR}/gen_spike_encoding_subset" \
      "${BUILD_DIR}/regext_bundle_test" \
      "${BUILD_DIR}/custom_decode_test" \
      "${BUILD_DIR}/mma_decode_test" \
      "${BUILD_DIR}/custom_ptx_emit_test" \
      "${BUILD_DIR}/mma_ptx_emit_test" \
      "${BUILD_DIR}/ventus_ocl_run"
  fi

  if [[ "${PRESET}" == "quick" || "${PRESET}" == "all" ]]; then
    run_regext_bundle_test
    run_custom_unit_tests
    run_custom_mma_oracle_gate
    run_custom_non_mma_oracle_gate
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
