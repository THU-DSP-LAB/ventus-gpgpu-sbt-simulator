#!/usr/bin/env python3
"""
背景
- 当前 MMA 回归已经覆盖首批 landed `row.col` family，但语义验证仍分裂为两条口径：
  非 `fp16 -> fp16` family 主要看 Spike-vs-PTX，`fp16 -> fp16` family 另外维护一份 CPU reference。
- 为了把 current supported MMA family 统一成 `sbtsim / Spike / CPU reference` 三方检查，需要一个
  repository-managed 的共享语义 helper，负责输入生成、CPU 参考计算和容差比较。

需求/作用
- 为当前所有已支持 MMA family 提供共享的 CPU reference 入口：
  - `m16n8k16 row.col f32.f16.f16.f32`
  - `m16n8k16 row.col f32.bf16.bf16.f32`
  - `m16n8k8 row.col f32.tf32.tf32.f32`
  - `m16n16k16 row.col f32.f16.f16.f32`
  - `m16n16k16 row.col f32.bf16.bf16.f32`
  - `m16n16k8 row.col f32.tf32.tf32.f32`
  - `m16n8k16 row.col f16.f16.f16.f16`
  - `m16n16k16 row.col f16.f16.f16.f16`
- 使用可复现随机 seed 生成有限值输入，避免把 NaN/Inf 策略问题混进当前语义 gate。
- 统一输出比较：
  - `fp16`：`NaN` 按分类相等，有限值按 `<= ULP`；
  - `f32`：`NaN` 按分类相等，有限值按 `atol/rtol`。

用法
- 作为模块供 `tools/custom_mma_oracle.py` 与 `tools/fp16_mma_spike_cpu_ref.py` 导入。
- 常用入口：
  - `build_seed_words(n, seed)`
  - `compute_cpu_reference(seed_words, kernel_name=...)`
  - `compare_outputs(actual_words, expected_words, kernel_name=...)`

实现原理/处理步骤
1) 按每个 kernel 的当前 microtest 约定，把 host seed 映射为该 family 的 `A/B/C` 载荷。
2) 在 host 侧重建一个“只关心输入输出语义”的逻辑 MMA 参考模型，计算参考 `D`。
3) 按当前 microtest 的可观察写回规则，把 `D` 投影成和输出 buffer `B` 相同的观测结果。
4) 提供统一的 `fp16` / `f32` 比较统计，供回归脚本打印 PASS/FAIL 明细。
"""

from __future__ import annotations

from dataclasses import dataclass
import math
import random
import struct


WARP_LANES = 32

KERNEL_M16N8K16_F32_F16 = "mt_custom_mma_m16n8k16_row_col_f32_f16_f16_f32"
KERNEL_M16N8K16_F32_BF16 = "mt_custom_mma_m16n8k16_row_col_f32_bf16_bf16_f32"
KERNEL_M16N8K8_F32_TF32 = "mt_custom_mma_m16n8k8_row_col_f32_tf32_tf32_f32"
KERNEL_M16N16K16_F32_F16 = "mt_custom_mma_m16n16k16_row_col_f32_f16_f16_f32"
KERNEL_M16N16K16_F32_BF16 = "mt_custom_mma_m16n16k16_row_col_f32_bf16_bf16_f32"
KERNEL_M16N16K8_F32_TF32 = "mt_custom_mma_m16n16k8_row_col_f32_tf32_tf32_f32"
KERNEL_M16N8K16_F16_F16 = "mt_custom_mma_m16n8k16_row_col_f16_f16_f16_f16"
KERNEL_M16N16K16_F16_F16 = "mt_custom_mma_m16n16k16_row_col_f16_f16_f16_f16"

SHAPE_M16N8K16 = "m16n8k16"
SHAPE_M16N8K8 = "m16n8k8"
SHAPE_M16N16K16 = "m16n16k16"
SHAPE_M16N16K8 = "m16n16k8"

AB_KIND_F16 = "f16"
AB_KIND_BF16 = "bf16"
AB_KIND_TF32 = "tf32"

CD_KIND_FP16 = "fp16"
CD_KIND_FP32 = "fp32"

OUT_WIDE_MOD4 = "wide_mod4"
OUT_WIDE_MOD8 = "wide_mod8"
OUT_PACKED_MOD2 = "packed_mod2"
OUT_PACKED_STORE4 = "packed_store4"

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
BF16_VALUES = (
    0x3F80,
    0xBF80,
    0x3F00,
    0x4000,
    0x3E80,
    0xC000,
    0x3FC0,
    0xBF00,
)
TF32_VALUES = (
    0x3F800000,
    0xBF800000,
    0x3F000000,
    0x40000000,
    0x3E800000,
    0xC0000000,
    0x3FC00000,
    0xBF000000,
)
A_PACKED_SALTS = ((0, 1), (2, 3), (4, 5), (6, 7))


@dataclass(frozen=True)
class ShapeSpec:
    name: str
    m_dim: int
    n_dim: int
    k_dim: int


@dataclass(frozen=True)
class PackedRegSpec:
    seed_xor: int
    salt_lo: int
    salt_hi: int


@dataclass(frozen=True)
class FloatRegSpec:
    salt: int
    scale: float


@dataclass(frozen=True)
class KernelSpec:
    kernel_name: str
    shape: str
    ab_kind: str
    cd_kind: str
    output_mode: str
    seed_mul: int
    seed_shift: int
    gid_mul: int
    seed_xor: int
    feature_define: str
    b_packed_regs: tuple[PackedRegSpec, ...] = ()
    b_wide_seed_xors: tuple[int, ...] = ()
    b_wide_salts: tuple[int, ...] = ()
    c_packed_regs: tuple[PackedRegSpec, ...] = ()
    c_float_regs: tuple[FloatRegSpec, ...] = ()


@dataclass(frozen=True)
class CompareStats:
    mode: str
    nan_pairs: int
    finite_pairs: int
    finite_pass: int
    max_abs_err: float
    max_ulp_err: int
    failures: tuple[str, ...]


SHAPE_SPECS: dict[str, ShapeSpec] = {
    SHAPE_M16N8K16: ShapeSpec(SHAPE_M16N8K16, 16, 8, 16),
    SHAPE_M16N8K8: ShapeSpec(SHAPE_M16N8K8, 16, 8, 8),
    SHAPE_M16N16K16: ShapeSpec(SHAPE_M16N16K16, 16, 16, 16),
    SHAPE_M16N16K8: ShapeSpec(SHAPE_M16N16K8, 16, 16, 8),
}

KERNEL_SPECS: dict[str, KernelSpec] = {
    KERNEL_M16N8K16_F32_F16: KernelSpec(
        kernel_name=KERNEL_M16N8K16_F32_F16,
        shape=SHAPE_M16N8K16,
        ab_kind=AB_KIND_F16,
        cd_kind=CD_KIND_FP32,
        output_mode=OUT_WIDE_MOD4,
        seed_mul=1,
        seed_shift=0,
        gid_mul=0x9E3779B9,
        seed_xor=0,
        feature_define="SBT_MMA_ENABLE_F16_M16N8K16",
        b_packed_regs=(
            PackedRegSpec(0x13579BDF, 1, 3),
            PackedRegSpec(0x2468ACE0, 5, 7),
        ),
        c_float_regs=(
            FloatRegSpec(0x11, 0.0625),
            FloatRegSpec(0x22, 0.03125),
            FloatRegSpec(0x33, 0.015625),
            FloatRegSpec(0x44, 0.125),
        ),
    ),
    KERNEL_M16N8K16_F32_BF16: KernelSpec(
        kernel_name=KERNEL_M16N8K16_F32_BF16,
        shape=SHAPE_M16N8K16,
        ab_kind=AB_KIND_BF16,
        cd_kind=CD_KIND_FP32,
        output_mode=OUT_WIDE_MOD4,
        seed_mul=1,
        seed_shift=1,
        gid_mul=0x85EBCA6B,
        seed_xor=0,
        feature_define="SBT_MMA_ENABLE_BF16_M16N8K16",
        b_packed_regs=(
            PackedRegSpec(0x55AA55AA, 1, 3),
            PackedRegSpec(0xAA55AA55, 5, 7),
        ),
        c_float_regs=(
            FloatRegSpec(0x51, 0.0625),
            FloatRegSpec(0x62, 0.03125),
            FloatRegSpec(0x73, 0.015625),
            FloatRegSpec(0x84, 0.125),
        ),
    ),
    KERNEL_M16N8K8_F32_TF32: KernelSpec(
        kernel_name=KERNEL_M16N8K8_F32_TF32,
        shape=SHAPE_M16N8K8,
        ab_kind=AB_KIND_TF32,
        cd_kind=CD_KIND_FP32,
        output_mode=OUT_WIDE_MOD4,
        seed_mul=17,
        seed_shift=0,
        gid_mul=0xC2B2AE35,
        seed_xor=0,
        feature_define="SBT_MMA_ENABLE_TF32_M16N8K8",
        b_wide_seed_xors=(0x31415926, 0x27182818),
        b_wide_salts=(4, 5),
        c_float_regs=(
            FloatRegSpec(0x91, 0.0625),
            FloatRegSpec(0xA2, 0.03125),
            FloatRegSpec(0xB3, 0.015625),
            FloatRegSpec(0xC4, 0.125),
        ),
    ),
    KERNEL_M16N16K16_F32_F16: KernelSpec(
        kernel_name=KERNEL_M16N16K16_F32_F16,
        shape=SHAPE_M16N16K16,
        ab_kind=AB_KIND_F16,
        cd_kind=CD_KIND_FP32,
        output_mode=OUT_WIDE_MOD8,
        seed_mul=31,
        seed_shift=0,
        gid_mul=0x9E3779B9,
        seed_xor=0,
        feature_define="SBT_MMA_ENABLE_F16_M16N16K16",
        b_packed_regs=(
            PackedRegSpec(0x11111111, 1, 2),
            PackedRegSpec(0x22222222, 3, 4),
            PackedRegSpec(0x33333333, 5, 6),
            PackedRegSpec(0x44444444, 7, 0),
        ),
        c_float_regs=(
            FloatRegSpec(0x12, 0.0625),
            FloatRegSpec(0x23, 0.03125),
            FloatRegSpec(0x34, 0.015625),
            FloatRegSpec(0x45, 0.125),
            FloatRegSpec(0x56, 0.0625),
            FloatRegSpec(0x67, 0.03125),
            FloatRegSpec(0x78, 0.015625),
            FloatRegSpec(0x89, 0.125),
        ),
    ),
    KERNEL_M16N16K16_F32_BF16: KernelSpec(
        kernel_name=KERNEL_M16N16K16_F32_BF16,
        shape=SHAPE_M16N16K16,
        ab_kind=AB_KIND_BF16,
        cd_kind=CD_KIND_FP32,
        output_mode=OUT_WIDE_MOD8,
        seed_mul=13,
        seed_shift=0,
        gid_mul=0x165667B1,
        seed_xor=0,
        feature_define="SBT_MMA_ENABLE_BF16_M16N16K16",
        b_packed_regs=(
            PackedRegSpec(0x0F0F0F0F, 1, 2),
            PackedRegSpec(0xF0F0F0F0, 3, 4),
            PackedRegSpec(0x55AA55AA, 5, 6),
            PackedRegSpec(0xAA55AA55, 7, 0),
        ),
        c_float_regs=(
            FloatRegSpec(0x1F, 0.0625),
            FloatRegSpec(0x2E, 0.03125),
            FloatRegSpec(0x3D, 0.015625),
            FloatRegSpec(0x4C, 0.125),
            FloatRegSpec(0x5B, 0.0625),
            FloatRegSpec(0x6A, 0.03125),
            FloatRegSpec(0x79, 0.015625),
            FloatRegSpec(0x88, 0.125),
        ),
    ),
    KERNEL_M16N16K8_F32_TF32: KernelSpec(
        kernel_name=KERNEL_M16N16K8_F32_TF32,
        shape=SHAPE_M16N16K8,
        ab_kind=AB_KIND_TF32,
        cd_kind=CD_KIND_FP32,
        output_mode=OUT_WIDE_MOD8,
        seed_mul=7,
        seed_shift=0,
        gid_mul=0x27D4EB2F,
        seed_xor=0,
        feature_define="SBT_MMA_ENABLE_TF32_M16N16K8",
        b_wide_seed_xors=(0x01020304, 0x11121314, 0x21222324, 0x31323334),
        b_wide_salts=(4, 5, 6, 7),
        c_float_regs=(
            FloatRegSpec(0x16, 0.0625),
            FloatRegSpec(0x27, 0.03125),
            FloatRegSpec(0x38, 0.015625),
            FloatRegSpec(0x49, 0.125),
            FloatRegSpec(0x5A, 0.0625),
            FloatRegSpec(0x6B, 0.03125),
            FloatRegSpec(0x7C, 0.015625),
            FloatRegSpec(0x8D, 0.125),
        ),
    ),
    KERNEL_M16N8K16_F16_F16: KernelSpec(
        kernel_name=KERNEL_M16N8K16_F16_F16,
        shape=SHAPE_M16N8K16,
        ab_kind=AB_KIND_F16,
        cd_kind=CD_KIND_FP16,
        output_mode=OUT_PACKED_MOD2,
        seed_mul=1,
        seed_shift=0,
        gid_mul=0x9E3779B9,
        seed_xor=0x5BD1E995,
        feature_define="SBT_MMA_ENABLE_FP16_FP16_M16N8K16",
        b_packed_regs=(
            PackedRegSpec(0x13579BDF, 1, 3),
            PackedRegSpec(0x2468ACE0, 5, 7),
        ),
        c_packed_regs=(
            PackedRegSpec(0xA5A5A5A5, 0, 2),
            PackedRegSpec(0x5A5A5A5A, 4, 6),
        ),
    ),
    KERNEL_M16N16K16_F16_F16: KernelSpec(
        kernel_name=KERNEL_M16N16K16_F16_F16,
        shape=SHAPE_M16N16K16,
        ab_kind=AB_KIND_F16,
        cd_kind=CD_KIND_FP16,
        output_mode=OUT_PACKED_STORE4,
        seed_mul=31,
        seed_shift=0,
        gid_mul=0x9E3779B9,
        seed_xor=0x6A09E667,
        feature_define="SBT_MMA_ENABLE_FP16_FP16_M16N16K16",
        b_packed_regs=(
            PackedRegSpec(0x11111111, 1, 2),
            PackedRegSpec(0x22222222, 3, 4),
            PackedRegSpec(0x33333333, 5, 6),
            PackedRegSpec(0x44444444, 7, 0),
        ),
        c_packed_regs=(
            PackedRegSpec(0xA5A5A5A5, 0, 2),
            PackedRegSpec(0x5A5A5A5A, 4, 6),
            PackedRegSpec(0x0F0F0F0F, 1, 5),
            PackedRegSpec(0xF0F0F0F0, 3, 7),
        ),
    ),
}


def kernel_spec(kernel_name: str) -> KernelSpec:
    try:
        return KERNEL_SPECS[kernel_name]
    except KeyError as exc:
        raise ValueError(f"unsupported mma kernel: {kernel_name}") from exc


def shape_spec(shape_name: str) -> ShapeSpec:
    try:
        return SHAPE_SPECS[shape_name]
    except KeyError as exc:
        raise ValueError(f"unsupported mma shape: {shape_name}") from exc


def kernel_name_for_shape(shape_name: str) -> str:
    if shape_name == SHAPE_M16N8K16:
        return KERNEL_M16N8K16_F16_F16
    if shape_name == SHAPE_M16N16K16:
        return KERNEL_M16N16K16_F16_F16
    raise ValueError(f"no fp16 wrapper kernel for shape: {shape_name}")


def feature_define_for_shape(shape_name: str) -> str:
    return kernel_spec(kernel_name_for_shape(shape_name)).feature_define


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


def u32_to_f32(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


def f32_to_u32(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", float(value)))[0]


def bf16_to_f32(bits: int) -> float:
    return u32_to_f32((bits & 0xFFFF) << 16)


def ordered_half(bits: int) -> int:
    bits &= 0xFFFF
    return (~bits & 0xFFFF) if (bits & 0x8000) else (bits | 0x8000)


def ulp_diff(a_bits: int, b_bits: int) -> int:
    return abs(ordered_half(a_bits) - ordered_half(b_bits))


def sample_lane_index(seed: int, salt: int) -> int:
    return (((seed >> ((salt & 3) * 5)) ^ (salt * 13)) & 7)


def finite_f16_bits(idx: int) -> int:
    return F16_VALUES[idx & 7]


def finite_bf16_bits(idx: int) -> int:
    return BF16_VALUES[idx & 7]


def finite_tf32_word(idx: int) -> int:
    return TF32_VALUES[idx & 7]


def pack_u16x2(lo: int, hi: int) -> int:
    return (lo & 0xFFFF) | ((hi & 0xFFFF) << 16)


def sample_packed_f16(seed: int, salt_lo: int, salt_hi: int) -> int:
    lo = finite_f16_bits(sample_lane_index(seed, salt_lo))
    hi = finite_f16_bits(sample_lane_index(seed, salt_hi))
    return pack_u16x2(lo, hi)


def sample_packed_bf16(seed: int, salt_lo: int, salt_hi: int) -> int:
    lo = finite_bf16_bits(sample_lane_index(seed, salt_lo))
    hi = finite_bf16_bits(sample_lane_index(seed, salt_hi))
    return pack_u16x2(lo, hi)


def sample_tf32(seed: int, salt: int) -> int:
    return finite_tf32_word(sample_lane_index(seed, salt))


def signed_sample(gid: int, salt: int, scale: float) -> float:
    signed = ((gid * 37) ^ salt) & 0x7F
    return float(signed - 64) * scale


def round_f32(value: float) -> float:
    return u32_to_f32(f32_to_u32(value))


def kernel_seed(input_word: int, gid: int, spec: KernelSpec) -> int:
    seeded_input = (input_word * spec.seed_mul) & 0xFFFFFFFF
    seeded_input = (seeded_input << spec.seed_shift) & 0xFFFFFFFF
    gid_part = (gid * spec.gid_mul) & 0xFFFFFFFF
    return (seeded_input ^ gid_part ^ spec.seed_xor) & 0xFFFFFFFF


def sample_a_regs(seed: int, spec: KernelSpec) -> list[int]:
    if spec.ab_kind == AB_KIND_TF32:
        return [sample_tf32(seed, salt) for salt in range(4)]
    sampler = sample_packed_f16 if spec.ab_kind == AB_KIND_F16 else sample_packed_bf16
    return [sampler(seed, salt_lo, salt_hi) for salt_lo, salt_hi in A_PACKED_SALTS]


def sample_b_regs(seed: int, spec: KernelSpec) -> list[int]:
    if spec.ab_kind == AB_KIND_TF32:
        return [sample_tf32(seed ^ seed_xor, salt) for seed_xor, salt in zip(spec.b_wide_seed_xors, spec.b_wide_salts)]
    sampler = sample_packed_f16 if spec.ab_kind == AB_KIND_F16 else sample_packed_bf16
    return [sampler(seed ^ reg.seed_xor, reg.salt_lo, reg.salt_hi) for reg in spec.b_packed_regs]


def sample_c_regs(seed: int, gid: int, spec: KernelSpec) -> list[int]:
    if spec.cd_kind == CD_KIND_FP16:
        return [sample_packed_f16(seed ^ reg.seed_xor, reg.salt_lo, reg.salt_hi) for reg in spec.c_packed_regs]
    return [f32_to_u32(signed_sample(gid, reg.salt, reg.scale)) for reg in spec.c_float_regs]


def build_register_windows(seed_words: list[int], spec: KernelSpec, gid_base: int) -> tuple[list[list[int]], list[list[int]], list[list[int]]]:
    a_regs: list[list[int]] = []
    b_regs: list[list[int]] = []
    c_regs: list[list[int]] = []
    for lane in range(WARP_LANES):
        gid = gid_base + lane
        seed = kernel_seed(seed_words[lane], gid, spec)
        a_regs.append(sample_a_regs(seed, spec))
        b_regs.append(sample_b_regs(seed, spec))
        c_regs.append(sample_c_regs(seed, gid, spec))
    return a_regs, b_regs, c_regs


def packed_ab_coords(role: str, shape: ShapeSpec, linear_idx: int) -> tuple[int, int]:
    if role == "A":
        return linear_idx // shape.k_dim, linear_idx % shape.k_dim
    return linear_idx % shape.n_dim, linear_idx // shape.n_dim


def wide_ab_coords(role: str, shape: ShapeSpec, linear_idx: int) -> tuple[int, int]:
    if role == "A":
        return linear_idx // shape.k_dim, linear_idx % shape.k_dim
    return linear_idx % shape.n_dim, linear_idx // shape.n_dim


def load_packed_ab_tile(regs: list[list[int]], role: str, shape: ShapeSpec) -> list[list[int]]:
    outer_dim = shape.m_dim if role == "A" else shape.n_dim
    inner_dim = shape.k_dim
    tile = [[0] * inner_dim for _ in range(outer_dim)]
    for reg_idx in range(len(regs[0])):
        for lane in range(WARP_LANES):
            value = regs[lane][reg_idx]
            idx0 = reg_idx * 64 + lane * 2
            coords = (
                packed_ab_coords(role, shape, idx0),
                packed_ab_coords(role, shape, idx0 + 1),
            )
            payloads = (value & 0xFFFF, (value >> 16) & 0xFFFF)
            for (row, col), payload in zip(coords, payloads):
                tile[row][col] = payload
    return tile


def load_wide_ab_tile(regs: list[list[int]], role: str, shape: ShapeSpec) -> list[list[int]]:
    outer_dim = shape.m_dim if role == "A" else shape.n_dim
    inner_dim = shape.k_dim
    tile = [[0] * inner_dim for _ in range(outer_dim)]
    for reg_idx in range(len(regs[0])):
        for lane in range(WARP_LANES):
            idx = reg_idx * WARP_LANES + lane
            row, col = wide_ab_coords(role, shape, idx)
            tile[row][col] = regs[lane][reg_idx]
    return tile


def load_c_tile(c_regs: list[list[int]], spec: KernelSpec) -> list[list[int]]:
    shape = shape_spec(spec.shape)
    if spec.cd_kind == CD_KIND_FP16:
        tile = [[0] * shape.n_dim for _ in range(shape.m_dim)]
        for reg_idx in range(len(c_regs[0])):
            for lane in range(WARP_LANES):
                value = c_regs[lane][reg_idx]
                idx0 = reg_idx * 64 + lane * 2
                coords = (
                    (idx0 // shape.n_dim, idx0 % shape.n_dim),
                    ((idx0 + 1) // shape.n_dim, (idx0 + 1) % shape.n_dim),
                )
                payloads = (value & 0xFFFF, (value >> 16) & 0xFFFF)
                for (row, col), payload in zip(coords, payloads):
                    tile[row][col] = payload
        return tile
    tile = [[0] * shape.n_dim for _ in range(shape.m_dim)]
    for reg_idx in range(len(c_regs[0])):
        for lane in range(WARP_LANES):
            idx = reg_idx * WARP_LANES + lane
            tile[idx // shape.n_dim][idx % shape.n_dim] = c_regs[lane][reg_idx]
    return tile


def load_ab_tiles(a_regs: list[list[int]], b_regs: list[list[int]], spec: KernelSpec) -> tuple[list[list[int]], list[list[int]]]:
    shape = shape_spec(spec.shape)
    if spec.ab_kind == AB_KIND_TF32:
        return load_wide_ab_tile(a_regs, "A", shape), load_wide_ab_tile(b_regs, "B", shape)
    return load_packed_ab_tile(a_regs, "A", shape), load_packed_ab_tile(b_regs, "B", shape)


def input_value_to_float(kind: str, payload: int) -> float:
    if kind == AB_KIND_F16:
        return u16_to_f16(payload)
    if kind == AB_KIND_BF16:
        return bf16_to_f32(payload)
    return u32_to_f32(payload)


def compute_fp16_output_tile(
    a_tile: list[list[int]],
    b_tile: list[list[int]],
    c_tile: list[list[int]],
    shape: ShapeSpec,
    ab_kind: str,
) -> list[list[int]]:
    out = [[0] * shape.n_dim for _ in range(shape.m_dim)]
    for m_idx in range(shape.m_dim):
        for n_idx in range(shape.n_dim):
            acc = u16_to_f16(c_tile[m_idx][n_idx])
            for k_idx in range(shape.k_dim):
                lhs = input_value_to_float(ab_kind, a_tile[m_idx][k_idx])
                rhs = input_value_to_float(ab_kind, b_tile[n_idx][k_idx])
                acc += lhs * rhs
            out[m_idx][n_idx] = f16_to_u16(acc)
    return out


def compute_fp32_output_tile(
    a_tile: list[list[int]],
    b_tile: list[list[int]],
    c_tile: list[list[int]],
    shape: ShapeSpec,
    ab_kind: str,
) -> list[list[int]]:
    out = [[0] * shape.n_dim for _ in range(shape.m_dim)]
    for m_idx in range(shape.m_dim):
        for n_idx in range(shape.n_dim):
            acc = round_f32(u32_to_f32(c_tile[m_idx][n_idx]))
            for k_idx in range(shape.k_dim):
                lhs = input_value_to_float(ab_kind, a_tile[m_idx][k_idx])
                rhs = input_value_to_float(ab_kind, b_tile[n_idx][k_idx])
                acc = round_f32(acc + round_f32(lhs * rhs))
            out[m_idx][n_idx] = f32_to_u32(acc)
    return out


def compute_output_tile(a_tile: list[list[int]], b_tile: list[list[int]], c_tile: list[list[int]], spec: KernelSpec) -> list[list[int]]:
    shape = shape_spec(spec.shape)
    if spec.cd_kind == CD_KIND_FP16:
        return compute_fp16_output_tile(a_tile, b_tile, c_tile, shape, spec.ab_kind)
    return compute_fp32_output_tile(a_tile, b_tile, c_tile, shape, spec.ab_kind)


def lane_output_regs(d_tile: list[list[int]], spec: KernelSpec, lane: int) -> list[int]:
    shape = shape_spec(spec.shape)
    regs: list[int] = []
    if spec.cd_kind == CD_KIND_FP16:
        reg_count = 2 if spec.shape == SHAPE_M16N8K16 else 4
        for reg_idx in range(reg_count):
            idx0 = reg_idx * 64 + lane * 2
            lo = d_tile[idx0 // shape.n_dim][idx0 % shape.n_dim]
            hi = d_tile[(idx0 + 1) // shape.n_dim][(idx0 + 1) % shape.n_dim]
            regs.append(pack_u16x2(lo, hi))
        return regs
    reg_count = 4 if spec.shape in (SHAPE_M16N8K16, SHAPE_M16N8K8) else 8
    for reg_idx in range(reg_count):
        idx = reg_idx * WARP_LANES + lane
        regs.append(d_tile[idx // shape.n_dim][idx % shape.n_dim])
    return regs


def project_observed_outputs(d_tile: list[list[int]], spec: KernelSpec) -> list[int]:
    out: list[int] = []
    if spec.output_mode == OUT_PACKED_MOD2:
        for lane in range(WARP_LANES):
            regs = lane_output_regs(d_tile, spec, lane)
            out.append(regs[lane & 1])
        return out
    if spec.output_mode == OUT_PACKED_STORE4:
        for lane in range(0, WARP_LANES, 4):
            out.extend(lane_output_regs(d_tile, spec, lane))
        return out
    if spec.output_mode == OUT_WIDE_MOD4:
        for lane in range(WARP_LANES):
            regs = lane_output_regs(d_tile, spec, lane)
            out.append(regs[lane & 3])
        return out
    if spec.output_mode == OUT_WIDE_MOD8:
        for lane in range(WARP_LANES):
            regs = lane_output_regs(d_tile, spec, lane)
            out.append(regs[lane & 7])
        return out
    raise ValueError(f"unsupported output mode: {spec.output_mode}")


def compute_cpu_reference_for_warp(seed_words: list[int], kernel_name: str, gid_base: int = 0) -> list[int]:
    if len(seed_words) != WARP_LANES:
        raise ValueError(f"mma cpu reference is warp-scoped: expected {WARP_LANES} seeds, got {len(seed_words)}")
    spec = kernel_spec(kernel_name)
    a_regs, b_regs, c_regs = build_register_windows(seed_words, spec, gid_base)
    a_tile, b_tile = load_ab_tiles(a_regs, b_regs, spec)
    c_tile = load_c_tile(c_regs, spec)
    d_tile = compute_output_tile(a_tile, b_tile, c_tile, spec)
    return project_observed_outputs(d_tile, spec)


def compute_cpu_reference(seed_words: list[int], kernel_name: str) -> list[int]:
    if len(seed_words) % WARP_LANES != 0:
        raise ValueError(f"mma cpu reference requires warp-multiple seeds: got {len(seed_words)}")
    out: list[int] = []
    for warp_base in range(0, len(seed_words), WARP_LANES):
        warp_seed_words = seed_words[warp_base:warp_base + WARP_LANES]
        out.extend(compute_cpu_reference_for_warp(warp_seed_words, kernel_name, warp_base))
    return out


def compare_fp16_outputs(actual_words: list[int], expected_words: list[int], ulp_tol: int) -> CompareStats:
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
    return CompareStats("fp16", nan_pairs, finite_pairs, finite_pass, max_abs_err, max_ulp_err, tuple(failures))


def approx_equal_f32(actual: float, expected: float, atol: float, rtol: float) -> bool:
    if actual == expected:
        return True
    if math.isnan(actual) and math.isnan(expected):
        return True
    if math.isnan(actual) or math.isnan(expected):
        return False
    if math.isinf(actual) or math.isinf(expected):
        return False
    return abs(actual - expected) <= atol + rtol * abs(expected)


def compare_fp32_outputs(actual_words: list[int], expected_words: list[int], atol: float, rtol: float) -> CompareStats:
    if len(actual_words) != len(expected_words):
        raise ValueError(f"fp32 compare size mismatch: got={len(actual_words)} expected={len(expected_words)}")
    nan_pairs = 0
    finite_pairs = 0
    finite_pass = 0
    max_abs_err = 0.0
    failures: list[str] = []
    for idx, (actual_bits, expected_bits) in enumerate(zip(actual_words, expected_words)):
        actual_val = u32_to_f32(actual_bits)
        expected_val = u32_to_f32(expected_bits)
        if math.isnan(actual_val) or math.isnan(expected_val):
            if math.isnan(actual_val) and math.isnan(expected_val):
                nan_pairs += 1
                continue
            failures.append(
                f"word={idx} actual=0x{actual_bits:08x} expected=0x{expected_bits:08x} reason=nan-class-mismatch"
            )
            continue
        finite_pairs += 1
        abs_err = abs(actual_val - expected_val)
        max_abs_err = max(max_abs_err, abs_err)
        if approx_equal_f32(actual_val, expected_val, atol, rtol):
            finite_pass += 1
            continue
        failures.append(
            f"word={idx} actual=0x{actual_bits:08x} expected=0x{expected_bits:08x} "
            f"actual_val={actual_val} expected_val={expected_val} abs_err={abs_err} atol={atol} rtol={rtol}"
        )
    return CompareStats("fp32", nan_pairs, finite_pairs, finite_pass, max_abs_err, 0, tuple(failures))


def compare_outputs(
    actual_words: list[int],
    expected_words: list[int],
    kernel_name: str,
    *,
    fp16_ulp_tol: int,
    f32_atol: float,
    f32_rtol: float,
) -> CompareStats:
    spec = kernel_spec(kernel_name)
    if spec.cd_kind == CD_KIND_FP16:
        return compare_fp16_outputs(actual_words, expected_words, fp16_ulp_tol)
    return compare_fp32_outputs(actual_words, expected_words, f32_atol, f32_rtol)


def format_compare_stats(stats: CompareStats) -> str:
    if stats.mode == "fp16":
        return (
            f"nan_pairs={stats.nan_pairs} finite_pairs={stats.finite_pairs} finite_pass={stats.finite_pass} "
            f"max_abs_err={stats.max_abs_err} max_ulp_err={stats.max_ulp_err}"
        )
    return (
        f"nan_pairs={stats.nan_pairs} finite_pairs={stats.finite_pairs} finite_pass={stats.finite_pass} "
        f"max_abs_err={stats.max_abs_err}"
    )
