#!/usr/bin/env python3
"""
背景
- MMA 仍处于独立 active change；当前先保留最小可观察 microtest 资产与 decode scaffolding。
- 由于 Ventus LLVM frontend builtin ABI 与 Spike shape/window 口径在 `fp16 -> fp16` 路径上尚未确认，
  sbtsim 当前仅对该受影响路径保持显式 fail-fast；其它已实现的 MMA 路径继续走 compile-first / Spike-vs-PTX gate。

需求/作用
- 执行 `testcases/ocl_compare/custom_mma_kernels.cl` 的最小 MMA microtest family。
- 支持分阶段 gate：
  1) `spike-precheck`：只跑 Spike 路径并验证输出可观察性；
  2) `compile-first`：在 spike-precheck 基础上运行 `sbt_decode --require-known`、`sbt_ptx --require-known` 与 `ptxas`；
  3) `full`：在 compile-first 基础上增加 Spike-vs-PTX 输出比较。
- 不做静默降级：未启用的阶段会显式打印 `SKIP`；启用后任一步失败直接返回非 0。

用法
- `python3 tools/custom_mma_oracle.py`
- `python3 tools/custom_mma_oracle.py --stage compile-first --sm sm_89`
- `python3 tools/custom_mma_oracle.py --stage full --sm 89`

实现原理/处理步骤
1) 通过 `source ../env.sh` + `VENTUS_BACKEND=spike` 跑 MMA kernel，检查输出字节规模与可观察性。
2) 当 stage >= compile-first 时，基于同次编译产物 `object0.riscv` 运行 `sbt_decode --require-known`、`sbt_ptx --require-known` 与 `ptxas`。
3) 当 stage = full 时，再运行 `VENTUS_BACKEND=ptx`，对比 Spike 与 PTX 输出；若未来新增受 `fp16 -> fp16` 临时 fail-fast 影响的 kernel，应对这些 kernel 显式失败或跳过，而不是把全部 MMA 都标成 pending。
"""

from __future__ import annotations

import argparse
import math
import shlex
import struct
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
DEFAULT_EXE = REPO_ROOT / "build/ventus_ocl_run"
DEFAULT_SBT_DECODE = REPO_ROOT / "build/sbt_decode"
DEFAULT_SBT_PTX = REPO_ROOT / "build/sbt_ptx"
DEFAULT_SRC = REPO_ROOT / "testcases/ocl_compare/custom_mma_kernels.cl"
DEFAULT_ENV_SH = (REPO_ROOT / ".." / "env.sh").resolve()
WARP_LANES = 32

STAGE_SPIKE_PRECHECK = "spike-precheck"
STAGE_COMPILE_FIRST = "compile-first"
STAGE_FULL = "full"
ALL_STAGES = (STAGE_SPIKE_PRECHECK, STAGE_COMPILE_FIRST, STAGE_FULL)
def u32_to_f32(x: int) -> float:
    return struct.unpack("<f", struct.pack("<I", x & 0xFFFFFFFF))[0]


def parse_u32_array(blob: bytes) -> list[int]:
    if len(blob) % 4 != 0:
        raise RuntimeError(f"invalid output byte size: {len(blob)}")
    return list(struct.unpack("<" + ("I" * (len(blob) // 4)), blob))


def write_u32_words(path: Path, words: list[int]) -> None:
    path.write_bytes(struct.pack("<" + ("I" * len(words)), *words))


def normalize_sm(sm: str) -> int:
    s = sm.strip()
    if s.startswith("sm_"):
        s = s[3:]
    v = int(s)
    if v <= 0:
        raise ValueError(f"invalid sm: {sm}")
    return v


def approx_equal(a: float, b: float, *, atol: float, rtol: float) -> bool:
    if a == b:
        return True
    if math.isinf(a) or math.isinf(b):
        return False
    if a != a and b != b:
        return True
    if a != a or b != b:
        return False
    diff = abs(a - b)
    limit = atol + rtol * abs(b)
    return diff <= limit


def run_checked(cmd: str, cwd: Path) -> subprocess.CompletedProcess[str]:
    p = subprocess.run(["bash", "-lc", cmd], cwd=str(cwd), text=True, capture_output=True)
    if p.returncode != 0:
        raise RuntimeError(f"command failed rc={p.returncode}\ncmd: {cmd}\nstdout:\n{p.stdout}\nstderr:\n{p.stderr}")
    return p


def maybe_prefix_nested_regext_env(cmd: str, enabled: bool) -> str:
    if not enabled:
        return cmd
    return f"SBT_COMPAT_SPIKE_NESTED_REGEXT=1 {cmd}"


@dataclass(frozen=True)
class KernelSpec:
    name: str
    mode: str
    compile_expect: str
    block_code: str = ""
    source_define: str = ""


KERNELS: list[KernelSpec] = [
    KernelSpec("mt_custom_mma_m16n8k16_row_col_f32_f16_f16_f32", "f32_tol", "pass", "", ""),
    KernelSpec("mt_custom_mma_m16n8k16_row_col_f32_bf16_bf16_f32", "f32_tol", "pass", "", "SBT_MMA_ENABLE_BF16_M16N8K16"),
    KernelSpec("mt_custom_mma_m16n8k8_row_col_f32_tf32_tf32_f32", "f32_tol", "pass", "", "SBT_MMA_ENABLE_TF32_M16N8K8"),
    KernelSpec("mt_custom_mma_m16n16k16_row_col_f32_f16_f16_f32", "f32_tol", "pass", "", "SBT_MMA_ENABLE_F16_M16N16K16"),
    KernelSpec("mt_custom_mma_m16n16k16_row_col_f32_bf16_bf16_f32", "f32_tol", "pass", "", "SBT_MMA_ENABLE_BF16_M16N16K16"),
    KernelSpec("mt_custom_mma_m16n16k8_row_col_f32_tf32_tf32_f32", "f32_tol", "pass", "", "SBT_MMA_ENABLE_TF32_M16N16K8"),
    KernelSpec(
        "mt_custom_mma_m16n8k16_row_col_f16_f16_f16_f16_blocked",
        "u32_exact",
        "blocked",
        "unsupported.mma.fp16_fp16_contract_pending",
        "SBT_MMA_ENABLE_FP16_FP16_BLOCKED",
    ),
]


def build_input_words(n: int) -> list[int]:
    out: list[int] = []
    for gid in range(n):
        out.append(((gid * 2654435761) ^ 0x6A09E667) & 0xFFFFFFFF)
    return out


def materialize_kernel_source(base_src: Path, dst_src: Path, define_name: str) -> Path:
    if not define_name:
        return base_src
    text = base_src.read_text(encoding="utf-8")
    dst_src.write_text(f"#define {define_name} 1\n{text}", encoding="utf-8")
    return dst_src


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
    in_path: Path | None,
    compat_nested_regext: bool,
) -> None:
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
        + (f"--in {shlex.quote(str(in_path))} " if in_path is not None else "")
        + f"--out {shlex.quote(str(out_path))}"
    )
    run_checked(cmd, workdir)


def check_spike_observable(name: str, words: list[int]) -> None:
    if not words:
        raise RuntimeError(f"{name}: empty output")
    if all(v == 0 for v in words):
        raise RuntimeError(f"{name}: output is all zeros, observable contract is suspicious")
    uniq = len(set(words))
    if uniq < 2:
        raise RuntimeError(f"{name}: output has no variance (unique={uniq})")


def compare_f32_bits_tol(name: str, got: list[int], exp: list[int], atol: float, rtol: float) -> None:
    if len(got) != len(exp):
        raise RuntimeError(f"{name}: size mismatch got={len(got)} expected={len(exp)}")
    for i, (g, e) in enumerate(zip(got, exp)):
        gf = u32_to_f32(g)
        ef = u32_to_f32(e)
        if not approx_equal(gf, ef, atol=atol, rtol=rtol):
            raise RuntimeError(f"{name}: mismatch i={i} got={gf} expected={ef} atol={atol} rtol={rtol}")


def compare_u32_exact(name: str, got: list[int], exp: list[int]) -> None:
    if len(got) != len(exp):
        raise RuntimeError(f"{name}: size mismatch got={len(got)} expected={len(exp)}")
    for i, (g, e) in enumerate(zip(got, exp)):
        if g != e:
            raise RuntimeError(f"{name}: mismatch i={i} got=0x{g:08x} expected=0x{e:08x}")


def stage_requires_compile_first(stage: str) -> bool:
    return stage in (STAGE_COMPILE_FIRST, STAGE_FULL)


def stage_requires_ptx_compare(stage: str) -> bool:
    return stage == STAGE_FULL


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    ap.add_argument("--sbt-decode", type=Path, default=DEFAULT_SBT_DECODE)
    ap.add_argument("--sbt-ptx", type=Path, default=DEFAULT_SBT_PTX)
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC)
    ap.add_argument("--env-sh", type=Path, default=DEFAULT_ENV_SH)
    ap.add_argument("--n", type=int, default=64)
    ap.add_argument("--sm", type=str, default="89")
    ap.add_argument("--ptxas", type=str, default="ptxas")
    ap.add_argument("--mma-atol", type=float, default=1e-3)
    ap.add_argument("--mma-rtol", type=float, default=1e-3)
    ap.add_argument("--stage", choices=ALL_STAGES, default=STAGE_SPIKE_PRECHECK)
    ap.add_argument(
        "--spike-compat-nested-regext",
        action="store_true",
        help="对本 gate 内部调用的 sbt_decode/sbt_ptx 与 PTX backend 显式打开 Spike-compatible nested regext 兼容模式",
    )
    args = ap.parse_args()

    if args.n < WARP_LANES:
        raise SystemExit(f"--n must be >= {WARP_LANES} for warp-level MMA kernels (got {args.n})")

    exe = args.exe.resolve()
    sbt_decode = args.sbt_decode.resolve()
    sbt_ptx = args.sbt_ptx.resolve()
    src = args.src.resolve()
    env_sh = args.env_sh.resolve()
    sm_num = normalize_sm(args.sm)

    required_paths = [exe, src, env_sh]
    if stage_requires_compile_first(args.stage):
        required_paths.extend([sbt_decode, sbt_ptx])
    for p in required_paths:
        if not p.exists():
            raise SystemExit(f"missing required path: {p}")

    if stage_requires_compile_first(args.stage) or stage_requires_ptx_compare(args.stage):
        p = subprocess.run(["bash", "-lc", f"command -v {shlex.quote(args.ptxas)} >/dev/null 2>&1"], check=False)
        if p.returncode != 0:
            raise SystemExit(f"missing command: {args.ptxas}")

    print(f"[INFO] MMA oracle stage={args.stage} sm=sm_{sm_num} n={args.n}")
    if not stage_requires_compile_first(args.stage):
        print("[SKIP] compile-first stage disabled (use --stage compile-first/full)")
    if not stage_requires_ptx_compare(args.stage):
        print("[SKIP] Spike-vs-PTX compare stage disabled (use --stage full)")

    spike_pass = 0
    spike_block = 0
    compile_pass = 0
    compile_block = 0
    full_pass = 0
    full_block_skip = 0
    failures: list[str] = []

    with tempfile.TemporaryDirectory(prefix="custom_mma_oracle_") as td:
        workdir = Path(td)
        input_words = build_input_words(args.n)

        for spec in KERNELS:
            out_spike = workdir / f"{spec.name}.spike.bin"
            out_ptx = workdir / f"{spec.name}.ptx.bin"
            ptx_path = workdir / f"{spec.name}.ptx"
            cubin_path = workdir / f"{spec.name}.cubin"
            input_path = workdir / f"{spec.name}.in.bin"
            src_path = materialize_kernel_source(src, workdir / f"{spec.name}.cl", spec.source_define)
            write_u32_words(input_path, input_words)

            blocked_compile = False
            blocked_by_precheck = False
            spike_words: list[int] = []

            try:
                run_backend(
                    backend="spike",
                    exe=exe,
                    sbt_ptx=sbt_ptx,
                    src=src_path,
                    env_sh=env_sh,
                    workdir=workdir,
                    kernel=spec.name,
                    n=args.n,
                    out_path=out_spike,
                    sm_num=sm_num,
                    in_path=input_path,
                    compat_nested_regext=args.spike_compat_nested_regext,
                )
                spike_words = parse_u32_array(out_spike.read_bytes())
                check_spike_observable(spec.name, spike_words)
                spike_pass += 1
                print(f"PASS spike observable kernel={spec.name} words={len(spike_words)}")
            except Exception as ex:  # pylint: disable=broad-except
                if spec.compile_expect == "blocked":
                    blocked_compile = True
                    blocked_by_precheck = True
                    spike_block += 1
                    print(f"BLOCK spike observable kernel={spec.name} reason=blocked-path {ex}")
                else:
                    failures.append(f"{spec.name}: spike-precheck failed: {ex}")
                    print(f"FAIL spike observable kernel={spec.name}: {ex}")
                    continue

            if blocked_by_precheck:
                if stage_requires_compile_first(args.stage):
                    compile_block += 1
                    print(f"BLOCK compile-first kernel={spec.name} reason=spike-precheck-blocked")
                if stage_requires_ptx_compare(args.stage):
                    full_block_skip += 1
                    print(f"SKIP spike-vs-ptx kernel={spec.name} reason=spike-precheck-blocked")
                continue

            if stage_requires_compile_first(args.stage):
                try:
                    elf = workdir / "object0.riscv"
                    if not elf.exists():
                        raise RuntimeError(f"{spec.name}: missing generated ELF at {elf}")

                    decode_cmd = (
                        f"{shlex.quote(str(sbt_decode))} decode {shlex.quote(str(elf))} "
                        f"--func {shlex.quote(spec.name)} --require-known >/dev/null"
                    )
                    decode_cmd = maybe_prefix_nested_regext_env(decode_cmd, args.spike_compat_nested_regext)
                    run_checked(decode_cmd, workdir)

                    emit_cmd = (
                        f"{shlex.quote(str(sbt_ptx))} {shlex.quote(str(elf))} "
                        f"--func {shlex.quote(spec.name)} --require-known --sm {sm_num} --out {shlex.quote(str(ptx_path))}"
                    )
                    emit_cmd = maybe_prefix_nested_regext_env(emit_cmd, args.spike_compat_nested_regext)
                    run_checked(emit_cmd, workdir)

                    ptxas_cmd = (
                        f"{shlex.quote(args.ptxas)} -arch=sm_{sm_num} "
                        f"{shlex.quote(str(ptx_path))} -o {shlex.quote(str(cubin_path))}"
                    )
                    run_checked(ptxas_cmd, workdir)
                except Exception as ex:  # pylint: disable=broad-except
                    msg = str(ex)
                    if spec.compile_expect == "blocked" and spec.block_code and spec.block_code in msg:
                        blocked_compile = True
                        compile_block += 1
                        print(f"BLOCK compile-first kernel={spec.name} code={spec.block_code}")
                    else:
                        failures.append(f"{spec.name}: compile-first failed: {msg}")
                        print(f"FAIL compile-first kernel={spec.name}: {msg}")
                        continue
                else:
                    if spec.compile_expect == "blocked":
                        failures.append(f"{spec.name}: expected blocked but compile-first succeeded")
                        print(f"FAIL compile-first kernel={spec.name}: expected blocked but passed")
                        continue
                    compile_pass += 1
                    print(f"PASS compile-first kernel={spec.name} sm=sm_{sm_num}")

            if stage_requires_ptx_compare(args.stage):
                if blocked_compile:
                    full_block_skip += 1
                    print(f"SKIP spike-vs-ptx kernel={spec.name} reason=compile-first-blocked")
                    continue
                try:
                    run_backend(
                        backend="ptx",
                        exe=exe,
                        sbt_ptx=sbt_ptx,
                        src=src_path,
                        env_sh=env_sh,
                        workdir=workdir,
                        kernel=spec.name,
                        n=args.n,
                        out_path=out_ptx,
                        sm_num=sm_num,
                        in_path=input_path,
                        compat_nested_regext=args.spike_compat_nested_regext,
                    )
                    ptx_words = parse_u32_array(out_ptx.read_bytes())
                    if spec.mode == "f32_tol":
                        compare_f32_bits_tol(spec.name, ptx_words, spike_words, atol=args.mma_atol, rtol=args.mma_rtol)
                    elif spec.mode == "u32_exact":
                        compare_u32_exact(spec.name, ptx_words, spike_words)
                    else:
                        raise RuntimeError(f"unknown compare mode: {spec.mode}")
                    full_pass += 1
                    print(f"PASS spike-vs-ptx kernel={spec.name} mode={spec.mode}")
                except Exception as ex:  # pylint: disable=broad-except
                    failures.append(f"{spec.name}: full compare failed: {ex}")
                    print(f"FAIL spike-vs-ptx kernel={spec.name}: {ex}")

    print(
        "[SUMMARY] "
        f"spike_pass={spike_pass}/{len(KERNELS)} spike_block={spike_block} "
        f"compile_pass={compile_pass} compile_block={compile_block} "
        f"full_pass={full_pass} full_block_skip={full_block_skip} "
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
