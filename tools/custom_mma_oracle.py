#!/usr/bin/env python3
"""
背景
- custom MMA 首批 `row.col` 子集已经成为 current 行为；当前回归需要把所有已支持 family 统一到同一条
  `sbtsim / Spike / CPU reference` 三方语义检查路径上。
- 旧 gate 把 non-`fp16 -> fp16` family 留在 Spike-vs-PTX 二方比较，并保留了单独的 `spike-precheck`
  stage；这会让 current MMA oracle 强度不一致，也让 sample 多样性不足。

需求/作用
- 执行 `testcases/ocl_compare/custom_mma_kernels.cl` 的 MMA microtest family。
- 支持两档 gate：
  1) `compile-first`：materialize ELF 后运行 `sbt_decode --require-known`、`sbt_ptx --require-known` 与 `ptxas`；
  2) `full`：在 compile-first 基础上，对所有已支持 MMA family 执行 `Spike / sbtsim PTX / CPU reference`
     三方一致性检查，并覆盖至少一个较小样本和一个稍大样本。
- 不做静默降级：任何启用的步骤失败都显式报 `FAIL`；blocked family 必须显式报 `BLOCK`。

用法
- `python3 tools/custom_mma_oracle.py`
- `python3 tools/custom_mma_oracle.py --stage compile-first --sm sm_89`
- `python3 tools/custom_mma_oracle.py --stage full --sizes 32 128 --seed 0x20260414`

实现原理/处理步骤
1) 为每个 kernel materialize 单-kernel OpenCL 源文件，并先通过一次 Spike 运行生成 `object0.riscv`。
2) 对当前 kernel 运行 `sbt_decode --require-known`、`sbt_ptx --require-known` 与 `ptxas`。
3) 若为 supported family 且 stage=`full`，则对多组随机有限值样本分别执行：
   - Spike 输出 vs CPU reference
   - sbtsim PTX 输出 vs CPU reference
4) blocked family 只验证 compile-first 阶段继续显式 blocked。
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile

import mma_cpu_ref as mma_ref


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
DEFAULT_EXE = REPO_ROOT / "build/ventus_ocl_run"
DEFAULT_SBT_DECODE = REPO_ROOT / "build/sbt_decode"
DEFAULT_SBT_PTX = REPO_ROOT / "build/sbt_ptx"
DEFAULT_SRC = REPO_ROOT / "testcases/ocl_compare/custom_mma_kernels.cl"
DEFAULT_ENV_SH = (REPO_ROOT / ".." / "env.sh").resolve()

STAGE_COMPILE_FIRST = "compile-first"
STAGE_FULL = "full"
ALL_STAGES = (STAGE_COMPILE_FIRST, STAGE_FULL)
DEFAULT_SIZES = (32, 128)


@dataclass(frozen=True)
class KernelSpec:
    name: str
    feature_define: str
    compile_expect: str
    block_code: str = ""
    supported: bool = True


@dataclass(frozen=True)
class CaseSpec:
    label: str
    n: int
    seed: int


SUPPORTED_KERNELS = tuple(
    KernelSpec(name=spec.kernel_name, feature_define=spec.feature_define, compile_expect="pass")
    for spec in mma_ref.KERNEL_SPECS.values()
)

BLOCKED_KERNEL = KernelSpec(
    name="mt_custom_mma_m16n8k16_row_row_f16_f16_f16_f16_blocked",
    feature_define="SBT_MMA_ENABLE_FP16_FP16_NONCURRENT_BLOCKED",
    compile_expect="blocked",
    block_code="unsupported.mma.deferred",
    supported=False,
)

KERNELS = (*SUPPORTED_KERNELS, BLOCKED_KERNEL)


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    ap.add_argument("--sbt-decode", type=Path, default=DEFAULT_SBT_DECODE)
    ap.add_argument("--sbt-ptx", type=Path, default=DEFAULT_SBT_PTX)
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC)
    ap.add_argument("--env-sh", type=Path, default=DEFAULT_ENV_SH)
    ap.add_argument("--sm", type=str, default="89")
    ap.add_argument("--ptxas", type=str, default="ptxas")
    ap.add_argument("--stage", choices=ALL_STAGES, default=STAGE_FULL)
    ap.add_argument("--sizes", nargs="+", type=int, default=list(DEFAULT_SIZES))
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0x20260414)
    ap.add_argument("--mma-atol", type=float, default=1e-3)
    ap.add_argument("--mma-rtol", type=float, default=1e-3)
    ap.add_argument("--fp16-ulp-tol", type=int, default=1)
    ap.add_argument(
        "--spike-compat-nested-regext",
        action="store_true",
        help="对本 gate 内部调用的 sbt_decode/sbt_ptx 与 PTX backend 显式打开 Spike-compatible nested regext 兼容模式",
    )
    return ap.parse_args()


def normalize_sm(sm: str) -> int:
    value = sm[3:] if sm.startswith("sm_") else sm
    sm_num = int(value)
    if sm_num <= 0:
        raise ValueError(f"invalid sm: {sm}")
    return sm_num


def run_checked(cmd: str, cwd: Path) -> None:
    proc = subprocess.run(["bash", "-lc", cmd], cwd=str(cwd), text=True, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"command failed rc={proc.returncode}\ncmd: {cmd}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")


def maybe_prefix_nested_regext_env(cmd: str, enabled: bool) -> str:
    if not enabled:
        return cmd
    return f"SBT_COMPAT_SPIKE_NESTED_REGEXT=1 {cmd}"


def materialize_kernel_source(base_src: Path, dst_src: Path, define_name: str) -> Path:
    text = base_src.read_text(encoding="utf-8")
    dst_src.write_text(f"#define {define_name} 1\n{text}", encoding="utf-8")
    return dst_src


def write_u32_words(path: Path, words: list[int]) -> None:
    path.write_bytes(struct.pack("<" + ("I" * len(words)), *words))


def parse_u32_array(blob: bytes) -> list[int]:
    if len(blob) % 4 != 0:
        raise RuntimeError(f"invalid output byte size: {len(blob)}")
    return list(struct.unpack("<" + ("I" * (len(blob) // 4)), blob))


def run_backend(
    *,
    backend: str,
    exe: Path,
    sbt_ptx: Path,
    src: Path,
    env_sh: Path,
    workdir: Path,
    kernel: str,
    n: int,
    out_path: Path,
    sm_num: int,
    in_path: Path,
    compat_nested_regext: bool,
) -> list[int]:
    env_parts = [f"VENTUS_BACKEND={backend}"]
    if backend == "ptx":
        env_parts.append(f"GPU_SBT_PTX={shlex.quote(str(sbt_ptx))}")
        env_parts.append(f"VENTUS_PTX_SM={sm_num}")
        if compat_nested_regext:
            env_parts.append("SBT_COMPAT_SPIKE_NESTED_REGEXT=1")
    cmd = (
        f"source {shlex.quote(str(env_sh))} >/dev/null 2>&1 && "
        + " ".join(env_parts)
        + " "
        + f"{shlex.quote(str(exe))} --src {shlex.quote(str(src))} "
        + f"--kernel {shlex.quote(kernel)} --n {n} "
        + f"--in {shlex.quote(str(in_path))} --out {shlex.quote(str(out_path))}"
    )
    run_checked(cmd, workdir)
    return parse_u32_array(out_path.read_bytes())


def run_compile_first(
    *,
    sbt_decode: Path,
    sbt_ptx: Path,
    ptxas: str,
    workdir: Path,
    kernel_name: str,
    sm_num: int,
    compat_nested_regext: bool,
) -> None:
    elf = workdir / "object0.riscv"
    if not elf.exists():
        raise RuntimeError(f"{kernel_name}: missing generated ELF at {elf}")

    decode_cmd = (
        f"{shlex.quote(str(sbt_decode))} decode {shlex.quote(str(elf))} "
        f"--func {shlex.quote(kernel_name)} --require-known >/dev/null"
    )
    emit_cmd = (
        f"{shlex.quote(str(sbt_ptx))} {shlex.quote(str(elf))} "
        f"--func {shlex.quote(kernel_name)} --require-known --sm {sm_num} "
        f"--out {shlex.quote(str(workdir / (kernel_name + '.ptx')))}"
    )
    ptxas_cmd = (
        f"{shlex.quote(ptxas)} -arch=sm_{sm_num} "
        f"{shlex.quote(str(workdir / (kernel_name + '.ptx')))} "
        f"-o {shlex.quote(str(workdir / (kernel_name + '.cubin')))}"
    )
    run_checked(maybe_prefix_nested_regext_env(decode_cmd, compat_nested_regext), workdir)
    run_checked(maybe_prefix_nested_regext_env(emit_cmd, compat_nested_regext), workdir)
    run_checked(ptxas_cmd, workdir)


def validate_sizes(stage: str, sizes: list[int]) -> list[int]:
    if not sizes:
        raise SystemExit("at least one --sizes entry is required")
    normalized = sorted(set(sizes))
    for n in normalized:
        if n < mma_ref.WARP_LANES:
            raise SystemExit(f"size must be >= {mma_ref.WARP_LANES} for warp-level MMA kernels (got {n})")
        if n % mma_ref.WARP_LANES != 0:
            raise SystemExit(f"size must be a multiple of {mma_ref.WARP_LANES} for warp-scoped MMA validation (got {n})")
    if stage == STAGE_FULL and len(normalized) < 2:
        raise SystemExit(
            "full MMA validation requires at least two distinct --sizes values "
            "(one smaller batch and one larger batch)"
        )
    return normalized


def build_cases(sizes: list[int], base_seed: int) -> list[CaseSpec]:
    labels = []
    for idx, n in enumerate(sizes):
        label = "small" if idx == 0 else "large" if idx == len(sizes) - 1 else f"case{idx + 1}"
        case_seed = (base_seed ^ ((idx + 1) * 0x9E3779B1) ^ (n << 12)) & 0xFFFFFFFF
        labels.append(CaseSpec(label=label, n=n, seed=case_seed))
    return labels


def ensure_required_paths(args: argparse.Namespace) -> tuple[Path, Path, Path, Path, Path]:
    exe = args.exe.resolve()
    sbt_decode = args.sbt_decode.resolve()
    sbt_ptx = args.sbt_ptx.resolve()
    src = args.src.resolve()
    env_sh = args.env_sh.resolve()
    for path in (exe, sbt_decode, sbt_ptx, src, env_sh):
        if not path.exists():
            raise SystemExit(f"missing required path: {path}")
    check = subprocess.run(["bash", "-lc", f"command -v {shlex.quote(args.ptxas)} >/dev/null 2>&1"], check=False)
    if check.returncode != 0:
        raise SystemExit(f"missing command: {args.ptxas}")
    return exe, sbt_decode, sbt_ptx, src, env_sh


def bootstrap_spike_case(
    *,
    kernel_name: str,
    case: CaseSpec,
    exe: Path,
    sbt_ptx: Path,
    src_path: Path,
    env_sh: Path,
    workdir: Path,
    sm_num: int,
    compat_nested_regext: bool,
) -> list[int]:
    input_words = mma_ref.build_seed_words(case.n, case.seed)
    input_path = workdir / f"{kernel_name}.{case.label}.bootstrap.in.bin"
    output_path = workdir / f"{kernel_name}.{case.label}.bootstrap.spike.bin"
    write_u32_words(input_path, input_words)
    return run_backend(
        backend="spike",
        exe=exe,
        sbt_ptx=sbt_ptx,
        src=src_path,
        env_sh=env_sh,
        workdir=workdir,
        kernel=kernel_name,
        n=case.n,
        out_path=output_path,
        sm_num=sm_num,
        in_path=input_path,
        compat_nested_regext=compat_nested_regext,
    )


def run_full_case(
    *,
    kernel: KernelSpec,
    case: CaseSpec,
    exe: Path,
    sbt_ptx: Path,
    src_path: Path,
    env_sh: Path,
    workdir: Path,
    sm_num: int,
    compat_nested_regext: bool,
    fp16_ulp_tol: int,
    f32_atol: float,
    f32_rtol: float,
) -> None:
    input_words = mma_ref.build_seed_words(case.n, case.seed)
    expected_words = mma_ref.compute_cpu_reference(input_words, kernel.name)
    input_path = workdir / f"{kernel.name}.{case.label}.in.bin"
    spike_path = workdir / f"{kernel.name}.{case.label}.spike.bin"
    ptx_path = workdir / f"{kernel.name}.{case.label}.ptx.bin"
    write_u32_words(input_path, input_words)

    spike_words = run_backend(
        backend="spike",
        exe=exe,
        sbt_ptx=sbt_ptx,
        src=src_path,
        env_sh=env_sh,
        workdir=workdir,
        kernel=kernel.name,
        n=case.n,
        out_path=spike_path,
        sm_num=sm_num,
        in_path=input_path,
        compat_nested_regext=compat_nested_regext,
    )
    spike_stats = mma_ref.compare_outputs(
        spike_words,
        expected_words,
        kernel.name,
        fp16_ulp_tol=fp16_ulp_tol,
        f32_atol=f32_atol,
        f32_rtol=f32_rtol,
    )
    if spike_stats.failures:
        raise RuntimeError(
            f"Spike-vs-CPU-ref case={case.label} mismatch_count={len(spike_stats.failures)} first={spike_stats.failures[0]}"
        )
    print(f"PASS spike-vs-cpu-ref kernel={kernel.name} case={case.label} n={case.n} {mma_ref.format_compare_stats(spike_stats)}")

    ptx_words = run_backend(
        backend="ptx",
        exe=exe,
        sbt_ptx=sbt_ptx,
        src=src_path,
        env_sh=env_sh,
        workdir=workdir,
        kernel=kernel.name,
        n=case.n,
        out_path=ptx_path,
        sm_num=sm_num,
        in_path=input_path,
        compat_nested_regext=compat_nested_regext,
    )
    ptx_stats = mma_ref.compare_outputs(
        ptx_words,
        expected_words,
        kernel.name,
        fp16_ulp_tol=fp16_ulp_tol,
        f32_atol=f32_atol,
        f32_rtol=f32_rtol,
    )
    if ptx_stats.failures:
        raise RuntimeError(
            f"sbtsim-vs-CPU-ref case={case.label} mismatch_count={len(ptx_stats.failures)} first={ptx_stats.failures[0]}"
        )
    print(f"PASS sbtsim-vs-cpu-ref kernel={kernel.name} case={case.label} n={case.n} {mma_ref.format_compare_stats(ptx_stats)}")


def main() -> int:
    args = parse_args()
    sizes = validate_sizes(args.stage, args.sizes)
    cases = build_cases(sizes, args.seed)
    exe, sbt_decode, sbt_ptx, src, env_sh = ensure_required_paths(args)
    sm_num = normalize_sm(args.sm)

    print(f"[INFO] MMA oracle stage={args.stage} sm=sm_{sm_num} sizes={','.join(str(case.n) for case in cases)} seed=0x{args.seed:08x}")

    compile_pass = 0
    compile_block = 0
    full_pass = 0
    full_block_skip = 0
    failures: list[str] = []

    with tempfile.TemporaryDirectory(prefix="custom_mma_oracle_") as td:
        workdir = Path(td)
        bootstrap_case = cases[0]

        for kernel in KERNELS:
            src_path = materialize_kernel_source(src, workdir / f"{kernel.name}.cl", kernel.feature_define)
            try:
                bootstrap_spike_case(
                    kernel_name=kernel.name,
                    case=bootstrap_case,
                    exe=exe,
                    sbt_ptx=sbt_ptx,
                    src_path=src_path,
                    env_sh=env_sh,
                    workdir=workdir,
                    sm_num=sm_num,
                    compat_nested_regext=args.spike_compat_nested_regext,
                )
            except Exception as ex:  # pylint: disable=broad-except
                failures.append(f"{kernel.name}: bootstrap spike run failed: {ex}")
                print(f"FAIL bootstrap kernel={kernel.name}: {ex}")
                continue

            try:
                run_compile_first(
                    sbt_decode=sbt_decode,
                    sbt_ptx=sbt_ptx,
                    ptxas=args.ptxas,
                    workdir=workdir,
                    kernel_name=kernel.name,
                    sm_num=sm_num,
                    compat_nested_regext=args.spike_compat_nested_regext,
                )
            except Exception as ex:  # pylint: disable=broad-except
                message = str(ex)
                if kernel.compile_expect == "blocked" and kernel.block_code and kernel.block_code in message:
                    compile_block += 1
                    print(f"BLOCK compile-first kernel={kernel.name} code={kernel.block_code}")
                    if args.stage == STAGE_FULL:
                        full_block_skip += 1
                        print(f"SKIP three-way semantic kernel={kernel.name} reason=compile-first-blocked")
                    continue
                failures.append(f"{kernel.name}: compile-first failed: {message}")
                print(f"FAIL compile-first kernel={kernel.name}: {message}")
                continue

            if kernel.compile_expect == "blocked":
                failures.append(f"{kernel.name}: expected blocked but compile-first succeeded")
                print(f"FAIL compile-first kernel={kernel.name}: expected blocked but passed")
                continue

            compile_pass += 1
            print(f"PASS compile-first kernel={kernel.name} sm=sm_{sm_num}")

            if args.stage != STAGE_FULL:
                continue

            try:
                for case in cases:
                    run_full_case(
                        kernel=kernel,
                        case=case,
                        exe=exe,
                        sbt_ptx=sbt_ptx,
                        src_path=src_path,
                        env_sh=env_sh,
                        workdir=workdir,
                        sm_num=sm_num,
                        compat_nested_regext=args.spike_compat_nested_regext,
                        fp16_ulp_tol=args.fp16_ulp_tol,
                        f32_atol=args.mma_atol,
                        f32_rtol=args.mma_rtol,
                    )
                    full_pass += 1
            except Exception as ex:  # pylint: disable=broad-except
                failures.append(f"{kernel.name}: full compare failed: {ex}")
                print(f"FAIL three-way semantic kernel={kernel.name}: {ex}")

    expected_supported_cases = len(SUPPORTED_KERNELS) * len(cases) if args.stage == STAGE_FULL else 0
    print(
        "[SUMMARY] "
        f"compile_pass={compile_pass}/{len(SUPPORTED_KERNELS)} compile_block={compile_block} "
        f"full_pass={full_pass}/{expected_supported_cases} full_block_skip={full_block_skip} "
        f"failures={len(failures)}"
    )
    if failures:
        print("[SUMMARY] failing kernels:")
        for item in failures:
            print(f"  - {item}")
        raise SystemExit(1)

    print("PASS custom MMA oracle gate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
