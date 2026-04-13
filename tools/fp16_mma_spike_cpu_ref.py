#!/usr/bin/env python3
"""
背景
- sbtsim 当前仍对 `fp16 -> fp16` MMA 的 PTX lowering 保持显式 blocked，但当前 Ventus LLVM + Spike
  软件栈已经具备单独验证该指令 Spike 语义的价值。
- 本脚本先固定一个仅依赖 `VENTUS_BACKEND=spike` 的 pre-support 测例，为后续切换到
  `sbtsim VS CPU ref` 留出同一条输入生成与 CPU reference 口径。

需求/作用
- 对 `m16n8k16 row.col fp16->fp16` 落一个 Spike-vs-CPU-reference 测例。
- 输入由 host 侧随机 `u32` seed 驱动，再由 kernel 映射到有限 `fp16` 值集合，避免把 NaN/Inf payload
  差异误判成 MMA 语义错误。
- 比较策略明确为：
  1) `NaN` 仅按分类相等；
  2) 非 `NaN` 输出按 `fp16` ULP 容差比较，默认 `<= 1 ULP`。

用法
- `python3 tools/fp16_mma_spike_cpu_ref.py`
- `python3 tools/fp16_mma_spike_cpu_ref.py --seed 0x20260413`
- `python3 tools/fp16_mma_spike_cpu_ref.py --ulp-tol 1 --keep-workdir`

实现原理/处理步骤
1) 生成 32 个随机 seed，并 materialize 只暴露目标 kernel 的单-kernel OpenCL 源文件。
2) 用 `VENTUS_BACKEND=spike` + `build/ventus_ocl_run` 执行该 kernel，拿到实际输出。
3) 在 host 侧按同一 seed 规则重建 Ventus `A/B/C` 寄存器窗口与逻辑 tile，执行纯 CPU reference。
4) 逐个 half lane 比较：`NaN` 按分类相等，有限值按 ULP 容差比较；若失败则显式打印 mismatch。
"""

from __future__ import annotations

import argparse
import math
import random
import shlex
import struct
import subprocess
import tempfile
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
DEFAULT_EXE = REPO_ROOT / "build/ventus_ocl_run"
DEFAULT_SRC = REPO_ROOT / "testcases/ocl_compare/custom_mma_kernels.cl"
DEFAULT_ENV_SH = (REPO_ROOT / ".." / "env.sh").resolve()
KERNEL_NAME = "mt_custom_mma_m16n8k16_row_col_f16_f16_f16_f16_cpu_ref"
FEATURE_DEFINE = "SBT_MMA_ENABLE_FP16_FP16_CPU_REF"
WARP_LANES = 32
M_DIM = 16
N_DIM = 8
K_DIM = 16
F16_VALUES = (
    0x3C00,
    0xBC00,
    0x3800,
    0x4000,
    0x3400,
    0xC000,
    0x3E00,
    0xB800,
)


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC)
    ap.add_argument("--env-sh", type=Path, default=DEFAULT_ENV_SH)
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0x20260413)
    ap.add_argument("--n", type=int, default=WARP_LANES)
    ap.add_argument("--ulp-tol", type=int, default=1)
    ap.add_argument("--keep-workdir", action="store_true")
    return ap.parse_args()


def run_checked(cmd: str, cwd: Path) -> None:
    proc = subprocess.run(["bash", "-lc", cmd], cwd=str(cwd), text=True, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"command failed rc={proc.returncode}\ncmd: {cmd}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")


def write_u32_words(path: Path, words: list[int]) -> None:
    path.write_bytes(struct.pack("<" + ("I" * len(words)), *words))


def parse_u32_words(path: Path) -> list[int]:
    blob = path.read_bytes()
    if len(blob) % 4 != 0:
        raise RuntimeError(f"invalid output size: {len(blob)}")
    return list(struct.unpack("<" + ("I" * (len(blob) // 4)), blob))


def materialize_kernel_source(base_src: Path, dst_src: Path) -> Path:
    text = base_src.read_text(encoding="utf-8")
    dst_src.write_text(f"#define {FEATURE_DEFINE} 1\n{text}", encoding="utf-8")
    return dst_src


def build_seed_words(n: int, seed: int) -> list[int]:
    rng = random.Random(seed)
    return [rng.getrandbits(32) for _ in range(n)]


def u16_to_f16(bits: int) -> float:
    return struct.unpack("<e", struct.pack("<H", bits & 0xFFFF))[0]


def f16_to_u16(value: float) -> int:
    if math.isnan(value):
        return 0x7E00
    if math.isinf(value):
        return 0xFC00 if value < 0 else 0x7C00
    try:
        return struct.unpack("<H", struct.pack("<e", float(value)))[0]
    except OverflowError:
        return 0xFC00 if value < 0 else 0x7C00


def ordered_half(bits: int) -> int:
    bits &= 0xFFFF
    return (~bits & 0xFFFF) if (bits & 0x8000) else (bits | 0x8000)


def ulp_diff(a_bits: int, b_bits: int) -> int:
    return abs(ordered_half(a_bits) - ordered_half(b_bits))


def sample_lane_index(seed: int, salt: int) -> int:
    return (((seed >> ((salt & 3) * 5)) ^ (salt * 13)) & 7)


def finite_f16_bits(idx: int) -> int:
    return F16_VALUES[idx & 7]


def sample_packed_f16(seed: int, salt_lo: int, salt_hi: int) -> int:
    lo = finite_f16_bits(sample_lane_index(seed, salt_lo))
    hi = finite_f16_bits(sample_lane_index(seed, salt_hi))
    return lo | (hi << 16)


def kernel_seed(input_word: int, gid: int) -> int:
    return (input_word ^ ((gid * 0x9E3779B9) & 0xFFFFFFFF) ^ 0x5BD1E995) & 0xFFFFFFFF


def build_register_windows(seed_words: list[int]) -> tuple[list[list[int]], list[list[int]], list[list[int]]]:
    a_regs = [[0] * 4 for _ in range(WARP_LANES)]
    b_regs = [[0] * 2 for _ in range(WARP_LANES)]
    c_regs = [[0] * 2 for _ in range(WARP_LANES)]
    for gid in range(WARP_LANES):
        seed = kernel_seed(seed_words[gid], gid)
        a_regs[gid] = [
            sample_packed_f16(seed, 0, 1),
            sample_packed_f16(seed, 2, 3),
            sample_packed_f16(seed, 4, 5),
            sample_packed_f16(seed, 6, 7),
        ]
        b_regs[gid] = [
            sample_packed_f16(seed ^ 0x13579BDF, 1, 3),
            sample_packed_f16(seed ^ 0x2468ACE0, 5, 7),
        ]
        c_regs[gid] = [
            sample_packed_f16(seed ^ 0xA5A5A5A5, 0, 2),
            sample_packed_f16(seed ^ 0x5A5A5A5A, 4, 6),
        ]
    return a_regs, b_regs, c_regs


def load_logical_tiles(a_regs: list[list[int]], b_regs: list[list[int]], c_regs: list[list[int]]) -> tuple[list[list[int]], list[list[int]], list[list[int]]]:
    a_tile = [[0] * K_DIM for _ in range(M_DIM)]
    b_tile = [[0] * K_DIM for _ in range(N_DIM)]
    c_tile = [[0] * N_DIM for _ in range(M_DIM)]

    for reg in range(4):
        for lane in range(WARP_LANES):
            value = a_regs[lane][reg]
            idx0 = reg * 64 + lane * 2
            idx1 = idx0 + 1
            a_tile[idx0 // K_DIM][idx0 % K_DIM] = value & 0xFFFF
            a_tile[idx1 // K_DIM][idx1 % K_DIM] = (value >> 16) & 0xFFFF

    for reg in range(2):
        for lane in range(WARP_LANES):
            value = b_regs[lane][reg]
            idx0 = reg * 64 + lane * 2
            idx1 = idx0 + 1
            b_tile[idx0 % N_DIM][idx0 // N_DIM] = value & 0xFFFF
            b_tile[idx1 % N_DIM][idx1 // N_DIM] = (value >> 16) & 0xFFFF

    for reg in range(2):
        for lane in range(WARP_LANES):
            value = c_regs[lane][reg]
            idx0 = reg * 64 + lane * 2
            idx1 = idx0 + 1
            c_tile[idx0 // N_DIM][idx0 % N_DIM] = value & 0xFFFF
            c_tile[idx1 // N_DIM][idx1 % N_DIM] = (value >> 16) & 0xFFFF

    return a_tile, b_tile, c_tile


def compute_cpu_reference(seed_words: list[int]) -> list[int]:
    a_regs, b_regs, c_regs = build_register_windows(seed_words)
    a_tile, b_tile, c_tile = load_logical_tiles(a_regs, b_regs, c_regs)
    d_tile = [[0] * N_DIM for _ in range(M_DIM)]

    for m in range(M_DIM):
        for n in range(N_DIM):
            acc = u16_to_f16(c_tile[m][n])
            for k in range(K_DIM):
                acc += u16_to_f16(a_tile[m][k]) * u16_to_f16(b_tile[n][k])
            d_tile[m][n] = f16_to_u16(acc)

    out = []
    for gid in range(WARP_LANES):
        regs = []
        for reg in range(2):
            idx0 = reg * 64 + gid * 2
            idx1 = idx0 + 1
            lo = d_tile[idx0 // N_DIM][idx0 % N_DIM]
            hi = d_tile[idx1 // N_DIM][idx1 % N_DIM]
            regs.append(lo | (hi << 16))
        out.append(regs[0] if (gid & 1) == 0 else regs[1])
    return out


def compare_words(actual_words: list[int], expected_words: list[int], ulp_tol: int) -> tuple[int, int, int, float, int, list[str]]:
    nan_pairs = 0
    finite_pairs = 0
    finite_pass = 0
    max_abs_err = 0.0
    max_ulp_err = 0
    failures: list[str] = []

    for word_idx, (actual, expected) in enumerate(zip(actual_words, expected_words)):
        for half_idx in range(2):
            actual_bits = (actual >> (half_idx * 16)) & 0xFFFF
            expected_bits = (expected >> (half_idx * 16)) & 0xFFFF
            actual_val = u16_to_f16(actual_bits)
            expected_val = u16_to_f16(expected_bits)

            if math.isnan(actual_val) or math.isnan(expected_val):
                if math.isnan(actual_val) and math.isnan(expected_val):
                    nan_pairs += 1
                    continue
                failures.append(
                    f"word={word_idx} half={half_idx} actual=0x{actual_bits:04x} expected=0x{expected_bits:04x} reason=nan-class-mismatch"
                )
                continue

            finite_pairs += 1
            abs_err = abs(actual_val - expected_val)
            ulp_err = ulp_diff(actual_bits, expected_bits)
            max_abs_err = max(max_abs_err, abs_err)
            max_ulp_err = max(max_ulp_err, ulp_err)
            if ulp_err <= ulp_tol:
                finite_pass += 1
                continue
            failures.append(
                f"word={word_idx} half={half_idx} actual=0x{actual_bits:04x} expected=0x{expected_bits:04x} "
                f"actual_val={actual_val} expected_val={expected_val} abs_err={abs_err} ulp_err={ulp_err}"
            )

    return nan_pairs, finite_pairs, finite_pass, max_abs_err, max_ulp_err, failures


def run_spike(exe: Path, env_sh: Path, src: Path, workdir: Path, input_path: Path, output_path: Path) -> None:
    cmd = (
        f"source {shlex.quote(str(env_sh))} >/dev/null 2>&1 && "
        f"VENTUS_BACKEND=spike {shlex.quote(str(exe))} "
        f"--src {shlex.quote(str(src))} --kernel {shlex.quote(KERNEL_NAME)} "
        f"--n {WARP_LANES} --in {shlex.quote(str(input_path))} --out {shlex.quote(str(output_path))}"
    )
    run_checked(cmd, workdir)


def main() -> int:
    args = parse_args()
    if args.n != WARP_LANES:
        raise SystemExit(f"--n must be exactly {WARP_LANES} for this warp-scoped fp16 MMA test")

    exe = args.exe.resolve()
    src = args.src.resolve()
    env_sh = args.env_sh.resolve()
    for path in (exe, src, env_sh):
        if not path.exists():
            raise SystemExit(f"missing required path: {path}")

    if args.keep_workdir:
        workdir = Path(tempfile.mkdtemp(prefix="fp16_mma_spike_cpu_ref_"))
        cleanup = False
    else:
        tempdir = tempfile.TemporaryDirectory(prefix="fp16_mma_spike_cpu_ref_")
        workdir = Path(tempdir.name)
        cleanup = True

    try:
        seed_words = build_seed_words(args.n, args.seed)
        input_path = workdir / "in.bin"
        output_path = workdir / "out.bin"
        src_path = materialize_kernel_source(src, workdir / "kernel.cl")
        write_u32_words(input_path, seed_words)

        run_spike(exe, env_sh, src_path, workdir, input_path, output_path)

        actual_words = parse_u32_words(output_path)
        expected_words = compute_cpu_reference(seed_words)
        nan_pairs, finite_pairs, finite_pass, max_abs_err, max_ulp_err, failures = compare_words(
            actual_words, expected_words, args.ulp_tol
        )

        print(f"[INFO] kernel={KERNEL_NAME} seed=0x{args.seed:08x} n={args.n}")
        print(f"[INFO] policy=NaN classify-equal; non-NaN <= {args.ulp_tol} fp16 ULP")
        print(f"[INFO] nan_pairs={nan_pairs} finite_pairs={finite_pairs} finite_pass={finite_pass}")
        print(f"[INFO] max_abs_err_non_nan={max_abs_err} max_ulp_err_non_nan={max_ulp_err}")

        if failures:
            print(f"[FAIL] mismatch_count={len(failures)}")
            for item in failures[:12]:
                print(f"  {item}")
            return 1

        print("PASS fp16 mma spike vs cpu ref")
        return 0
    finally:
        if args.keep_workdir:
            print(f"[INFO] kept workdir={workdir}")
        elif cleanup:
            tempdir.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
