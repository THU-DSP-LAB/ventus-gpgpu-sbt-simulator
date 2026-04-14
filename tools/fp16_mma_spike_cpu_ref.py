#!/usr/bin/env python3
"""
背景
- `fp16 -> fp16` MMA 现已进入 current supported 子集，但这两条 family 仍需要一个仓库内可复用的
  CPU reference 与 Spike 对照入口，避免只靠 PTX 侧现象判断语义是否正确。
- `lab/07_fp16_mma_ptx_probe/` 与 `tools/custom_mma_oracle.py` 都复用本脚本提供的输入生成、逻辑 tile
  重建与 `fp16` 容差比较规则，因此这里需要保持 repository-managed contract。

需求/作用
- 为 current supported `fp16 -> fp16` MMA family 提供 Spike-vs-CPU-reference 校验入口：
  - `m16n8k16 row.col f16->f16`
  - `m16n16k16 row.col f16->f16`
- 输入由 host 侧随机 `u32` seed 驱动，再由 kernel 映射到有限 `fp16` 值集合，避免把 NaN/Inf payload
  差异误判成 MMA 语义错误。
- 比较策略明确为：
  1) `NaN` 仅按分类相等；
  2) 非 `NaN` 输出按 `fp16` ULP 容差比较，默认 `<= 1 ULP`。

用法
- `python3 tools/fp16_mma_spike_cpu_ref.py`
- `python3 tools/fp16_mma_spike_cpu_ref.py --shape m16n16k16`
- `python3 tools/fp16_mma_spike_cpu_ref.py --seed 0x20260413 --ulp-tol 1 --keep-workdir`

实现原理/处理步骤
1) 生成 32 个随机 seed，并 materialize 只暴露目标 kernel 的单-kernel OpenCL 源文件。
2) 用 `VENTUS_BACKEND=spike` + `build/ventus_ocl_run` 执行对应 `fp16 -> fp16` kernel，拿到实际输出。
3) 在 host 侧按同一 shape contract 重建 Ventus `A/B/C` 寄存器窗口与逻辑 tile，执行纯 CPU reference。
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
from dataclasses import dataclass
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
DEFAULT_EXE = REPO_ROOT / "build/ventus_ocl_run"
DEFAULT_SRC = REPO_ROOT / "testcases/ocl_compare/custom_mma_kernels.cl"
DEFAULT_ENV_SH = (REPO_ROOT / ".." / "env.sh").resolve()
WARP_LANES = 32
SHAPE_M16N8K16 = "m16n8k16"
SHAPE_M16N16K16 = "m16n16k16"
ALL_SHAPES = (SHAPE_M16N8K16, SHAPE_M16N16K16)
M_DIM = 16
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


@dataclass(frozen=True)
class ShapeSpec:
    shape: str
    kernel_name: str
    feature_define: str
    n_dim: int
    seed_multiplier: int
    seed_xor: int
    b_seed_xors: tuple[int, ...]
    b_salts: tuple[tuple[int, int], ...]
    c_seed_xors: tuple[int, ...]
    c_salts: tuple[tuple[int, int], ...]
    observed_reg_count: int


A_SALTS = ((0, 1), (2, 3), (4, 5), (6, 7))

SHAPE_SPECS: dict[str, ShapeSpec] = {
    SHAPE_M16N8K16: ShapeSpec(
        shape=SHAPE_M16N8K16,
        kernel_name="mt_custom_mma_m16n8k16_row_col_f16_f16_f16_f16",
        feature_define="SBT_MMA_ENABLE_FP16_FP16_M16N8K16",
        n_dim=8,
        seed_multiplier=1,
        seed_xor=0x5BD1E995,
        b_seed_xors=(0x13579BDF, 0x2468ACE0),
        b_salts=((1, 3), (5, 7)),
        c_seed_xors=(0xA5A5A5A5, 0x5A5A5A5A),
        c_salts=((0, 2), (4, 6)),
        observed_reg_count=2,
    ),
    SHAPE_M16N16K16: ShapeSpec(
        shape=SHAPE_M16N16K16,
        kernel_name="mt_custom_mma_m16n16k16_row_col_f16_f16_f16_f16",
        feature_define="SBT_MMA_ENABLE_FP16_FP16_M16N16K16",
        n_dim=16,
        seed_multiplier=31,
        seed_xor=0x6A09E667,
        b_seed_xors=(0x11111111, 0x22222222, 0x33333333, 0x44444444),
        b_salts=((1, 2), (3, 4), (5, 6), (7, 0)),
        c_seed_xors=(0xA5A5A5A5, 0x5A5A5A5A, 0x0F0F0F0F, 0xF0F0F0F0),
        c_salts=((0, 2), (4, 6), (1, 5), (3, 7)),
        observed_reg_count=4,
    ),
}

DEFAULT_SHAPE = SHAPE_M16N8K16
KERNEL_NAME = SHAPE_SPECS[DEFAULT_SHAPE].kernel_name
FEATURE_DEFINE = SHAPE_SPECS[DEFAULT_SHAPE].feature_define


def shape_spec(shape: str) -> ShapeSpec:
    try:
        return SHAPE_SPECS[shape]
    except KeyError as exc:
        raise ValueError(f"unsupported fp16 mma shape: {shape}") from exc


def kernel_name_for_shape(shape: str) -> str:
    return shape_spec(shape).kernel_name


def feature_define_for_shape(shape: str) -> str:
    return shape_spec(shape).feature_define


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC)
    ap.add_argument("--env-sh", type=Path, default=DEFAULT_ENV_SH)
    ap.add_argument("--shape", choices=ALL_SHAPES, default=DEFAULT_SHAPE)
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


def materialize_kernel_source(base_src: Path, dst_src: Path, *, shape: str) -> Path:
    text = base_src.read_text(encoding="utf-8")
    dst_src.write_text(f"#define {feature_define_for_shape(shape)} 1\n{text}", encoding="utf-8")
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


def kernel_seed(input_word: int, gid: int, *, shape: str = DEFAULT_SHAPE) -> int:
    spec = shape_spec(shape)
    return (((input_word * spec.seed_multiplier) & 0xFFFFFFFF) ^ ((gid * 0x9E3779B9) & 0xFFFFFFFF) ^ spec.seed_xor) & 0xFFFFFFFF


def sample_register_words(seed: int, seed_xors: tuple[int, ...], salts: tuple[tuple[int, int], ...]) -> list[int]:
    out: list[int] = []
    for seed_xor, (salt_lo, salt_hi) in zip(seed_xors, salts):
        out.append(sample_packed_f16(seed ^ seed_xor, salt_lo, salt_hi))
    return out


def build_register_windows(
    seed_words: list[int], *, shape: str = DEFAULT_SHAPE, gid_base: int = 0
) -> tuple[list[list[int]], list[list[int]], list[list[int]]]:
    spec = shape_spec(shape)
    a_regs = [[0] * 4 for _ in range(WARP_LANES)]
    b_regs = [[0] * len(spec.b_seed_xors) for _ in range(WARP_LANES)]
    c_regs = [[0] * len(spec.c_seed_xors) for _ in range(WARP_LANES)]
    for lane in range(WARP_LANES):
        gid = gid_base + lane
        seed = kernel_seed(seed_words[lane], gid, shape=shape)
        a_regs[lane] = [sample_packed_f16(seed, salt_lo, salt_hi) for (salt_lo, salt_hi) in A_SALTS]
        b_regs[lane] = sample_register_words(seed, spec.b_seed_xors, spec.b_salts)
        c_regs[lane] = sample_register_words(seed, spec.c_seed_xors, spec.c_salts)
    return a_regs, b_regs, c_regs


def load_logical_tiles(
    a_regs: list[list[int]], b_regs: list[list[int]], c_regs: list[list[int]], *, shape: str = DEFAULT_SHAPE
) -> tuple[list[list[int]], list[list[int]], list[list[int]]]:
    spec = shape_spec(shape)
    n_dim = spec.n_dim
    a_tile = [[0] * K_DIM for _ in range(M_DIM)]
    b_tile = [[0] * K_DIM for _ in range(n_dim)]
    c_tile = [[0] * n_dim for _ in range(M_DIM)]

    for reg in range(4):
        for lane in range(WARP_LANES):
            value = a_regs[lane][reg]
            idx0 = reg * 64 + lane * 2
            idx1 = idx0 + 1
            a_tile[idx0 // K_DIM][idx0 % K_DIM] = value & 0xFFFF
            a_tile[idx1 // K_DIM][idx1 % K_DIM] = (value >> 16) & 0xFFFF

    for reg in range(len(b_regs[0])):
        for lane in range(WARP_LANES):
            value = b_regs[lane][reg]
            idx0 = reg * 64 + lane * 2
            idx1 = idx0 + 1
            b_tile[idx0 % n_dim][idx0 // n_dim] = value & 0xFFFF
            b_tile[idx1 % n_dim][idx1 // n_dim] = (value >> 16) & 0xFFFF

    for reg in range(len(c_regs[0])):
        for lane in range(WARP_LANES):
            value = c_regs[lane][reg]
            idx0 = reg * 64 + lane * 2
            idx1 = idx0 + 1
            c_tile[idx0 // n_dim][idx0 % n_dim] = value & 0xFFFF
            c_tile[idx1 // n_dim][idx1 % n_dim] = (value >> 16) & 0xFFFF

    return a_tile, b_tile, c_tile


def select_observed_word(regs: list[int], gid: int) -> int:
    return regs[gid & (len(regs) - 1)]


def compute_cpu_reference_for_warp(seed_words: list[int], *, shape: str = DEFAULT_SHAPE, gid_base: int = 0) -> list[int]:
    if len(seed_words) != WARP_LANES:
        raise ValueError(f"fp16 mma cpu reference is warp-scoped: expected {WARP_LANES} seeds, got {len(seed_words)}")
    spec = shape_spec(shape)
    n_dim = spec.n_dim
    a_regs, b_regs, c_regs = build_register_windows(seed_words, shape=shape, gid_base=gid_base)
    a_tile, b_tile, c_tile = load_logical_tiles(a_regs, b_regs, c_regs, shape=shape)
    d_tile = [[0] * n_dim for _ in range(M_DIM)]

    for m in range(M_DIM):
        for n in range(n_dim):
            acc = u16_to_f16(c_tile[m][n])
            for k in range(K_DIM):
                acc += u16_to_f16(a_tile[m][k]) * u16_to_f16(b_tile[n][k])
            d_tile[m][n] = f16_to_u16(acc)

    if shape == SHAPE_M16N16K16:
        out: list[int] = []
        for gid in range(0, WARP_LANES, 4):
            regs = []
            for reg in range(spec.observed_reg_count):
                idx0 = reg * 64 + gid * 2
                idx1 = idx0 + 1
                lo = d_tile[idx0 // n_dim][idx0 % n_dim]
                hi = d_tile[idx1 // n_dim][idx1 % n_dim]
                regs.append(lo | (hi << 16))
            out.extend(regs)
        return out

    out = []
    for gid in range(WARP_LANES):
        regs = []
        for reg in range(spec.observed_reg_count):
            idx0 = reg * 64 + gid * 2
            idx1 = idx0 + 1
            lo = d_tile[idx0 // n_dim][idx0 % n_dim]
            hi = d_tile[idx1 // n_dim][idx1 % n_dim]
            regs.append(lo | (hi << 16))
        out.append(select_observed_word(regs, gid))
    return out


def compute_cpu_reference(seed_words: list[int], *, shape: str = DEFAULT_SHAPE) -> list[int]:
    if len(seed_words) % WARP_LANES != 0:
        raise ValueError(f"fp16 mma cpu reference requires warp-multiple seeds: got {len(seed_words)}")

    out: list[int] = []
    for warp_base in range(0, len(seed_words), WARP_LANES):
        out.extend(compute_cpu_reference_for_warp(seed_words[warp_base:warp_base + WARP_LANES], shape=shape, gid_base=warp_base))
    return out


def compare_words(actual_words: list[int], expected_words: list[int], ulp_tol: int) -> tuple[int, int, int, float, int, list[str]]:
    if len(actual_words) != len(expected_words):
        raise ValueError(f"fp16 compare size mismatch: got={len(actual_words)} expected={len(expected_words)}")

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


def run_spike(exe: Path, env_sh: Path, src: Path, workdir: Path, input_path: Path, output_path: Path, *, shape: str) -> None:
    cmd = (
        f"source {shlex.quote(str(env_sh))} >/dev/null 2>&1 && "
        f"VENTUS_BACKEND=spike {shlex.quote(str(exe))} "
        f"--src {shlex.quote(str(src))} --kernel {shlex.quote(kernel_name_for_shape(shape))} "
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
        src_path = materialize_kernel_source(src, workdir / "kernel.cl", shape=args.shape)
        write_u32_words(input_path, seed_words)

        run_spike(exe, env_sh, src_path, workdir, input_path, output_path, shape=args.shape)

        actual_words = parse_u32_words(output_path)
        expected_words = compute_cpu_reference(seed_words, shape=args.shape)
        nan_pairs, finite_pairs, finite_pass, max_abs_err, max_ulp_err, failures = compare_words(
            actual_words, expected_words, args.ulp_tol
        )

        print(f"[INFO] kernel={kernel_name_for_shape(args.shape)} shape={args.shape} seed=0x{args.seed:08x} n={args.n}")
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
