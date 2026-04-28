#!/usr/bin/env python3
"""
背景
- `custom_non_mma_oracle.py` 需要维护一组 current custom non-MMA microtest kernel 及其比较模式。

需求/作用
- 把 kernel 清单与 warp 规模常量从 oracle 主流程中拆出，避免主脚本同时承担数据清单和执行编排。

用法
- 由 `tools/custom_non_mma_oracle.py` 直接 import；不作为独立命令入口使用。

实现原理/处理步骤
1) 定义不可变 `KernelSpec`。
2) 维护 shuffle kernel 集合、warp lane 数和 current non-MMA kernel 列表。
3) 比较模式由 oracle 主流程解释执行。
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class KernelSpec:
    name: str
    mode: str


SHUFFLE_KERNELS = {
    "mt_custom_shuffle_idx",
    "mt_custom_shuffle_up",
    "mt_custom_shuffle_down",
    "mt_custom_shuffle_bfly",
}

WARP_LANES = 32

KERNELS: list[KernelSpec] = [
    KernelSpec("mt_custom_shuffle_idx", "exact_u32"),
    KernelSpec("mt_custom_shuffle_up", "exact_u32"),
    KernelSpec("mt_custom_shuffle_down", "exact_u32"),
    KernelSpec("mt_custom_shuffle_bfly", "exact_u32"),
    KernelSpec("mt_custom_vcvt_fp16_roundtrip", "exact_u32"),
    KernelSpec("mt_custom_vcvt_bf16_roundtrip", "exact_u32"),
    KernelSpec("mt_custom_vadd_f16x2", "exact_u32"),
    KernelSpec("mt_custom_vmul_f16x2", "exact_u32"),
    KernelSpec("mt_custom_vfma_f16x2", "exact_u32"),
    KernelSpec("mt_custom_vadd_bf16x2", "exact_u32"),
    KernelSpec("mt_custom_vmul_bf16x2", "exact_u32"),
    KernelSpec("mt_custom_vfma_bf16x2", "exact_u32"),
    KernelSpec("mt_custom_vex2_f32", "f32_tol"),
    KernelSpec("mt_custom_vlg2_f32", "f32_tol"),
    KernelSpec("mt_custom_vrcp_f32", "f32_tol"),
    KernelSpec("mt_custom_vsqrt_f32", "f32_tol"),
    KernelSpec("mt_custom_vrsqrt_f32", "f32_tol"),
    KernelSpec("mt_custom_vsin_f32", "f32_tol"),
    KernelSpec("mt_custom_vcos_f32", "f32_tol"),
    KernelSpec("mt_custom_vtanh_f32", "f32_tol"),
    KernelSpec("mt_custom_vgelu_f32", "f32_tol"),
    KernelSpec("mt_custom_vsilu_f32", "f32_tol"),
    KernelSpec("mt_custom_vex2_f16x2", "packed_f16_tol"),
    KernelSpec("mt_custom_vrcp_f16x2", "packed_f16_tol"),
    KernelSpec("mt_custom_vsqrt_f16x2", "packed_f16_tol"),
    KernelSpec("mt_custom_vrsqrt_f16x2", "packed_f16_tol"),
    KernelSpec("mt_custom_vtanh_f16x2", "packed_f16_tol"),
    KernelSpec("mt_custom_vgelu_f16x2", "packed_f16_tol"),
    KernelSpec("mt_custom_vsilu_f16x2", "packed_f16_tol"),
    KernelSpec("mt_custom_vex2_bf16x2", "packed_bf16_tol"),
    KernelSpec("mt_custom_vrcp_bf16x2", "packed_bf16_tol"),
    KernelSpec("mt_custom_vsqrt_bf16x2", "packed_bf16_tol"),
    KernelSpec("mt_custom_vrsqrt_bf16x2", "packed_bf16_tol"),
    KernelSpec("mt_custom_vtanh_bf16x2", "packed_bf16_tol"),
    KernelSpec("mt_custom_vgelu_bf16x2", "packed_bf16_tol"),
    KernelSpec("mt_custom_vsilu_bf16x2", "packed_bf16_tol"),
]
