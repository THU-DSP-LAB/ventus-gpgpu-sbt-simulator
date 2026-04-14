#!/usr/bin/env python3
"""
背景
- 需要在隔离目录里做一个最小的“手写 PTX 对照 Spike” probe，用来验证 Ventus 风格
  `fp16 -> fp16 row.col` MMA 在 PTX 侧的寄存器输入/输出形态。
- 本 probe 不修改 sbtsim 主线 emitter / decode / tests；仅通过 CUDA Driver API 直接加载手写 PTX。

需求/作用
- 先覆盖 `m16n8k16 row.col fp16->fp16`，并自动枚举少量候选 operand 排列，避免靠猜。
- 复用 `tools/fp16_mma_spike_cpu_ref.py` 的输入生成与 CPU reference 口径，尽量再拿到 Spike 输出作中间对照。
- 输出每个候选 PTX 变体的结果：是否与 CPU ref 对齐、是否与 Spike 对齐、最大误差是多少。

用法
- `python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx.py --seed 0x20260413`
- `python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx.py --variant canonical`
- `python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx.py --load-mode cubin --keep-workdir`

实现原理/处理步骤
1. 用 `tools/fp16_mma_spike_cpu_ref.py` 生成同一套随机 seed、输入窗口和 CPU reference。
2. 通过 `tools/ventus_ocl_run` 跑一次现有 Spike 路径，拿到 Spike 输出，作为额外对照。
3. 在隔离目录里手写 PTX 模块，使用 CUDA Driver API 直接加载；按候选变体枚举 `mma.sync` operand 排列。
4. 对每个候选变体分别 launch，读回输出并与 CPU ref / Spike 对比。
"""

from __future__ import annotations

import argparse
import ctypes
import importlib.util
import itertools
import math
import os
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
HELPER_PATH = REPO_ROOT / "tools" / "fp16_mma_spike_cpu_ref.py"
DEFAULT_EXE = REPO_ROOT / "build" / "ventus_ocl_run"
DEFAULT_SRC = REPO_ROOT / "testcases" / "ocl_compare" / "custom_mma_kernels.cl"
DEFAULT_ENV_SH = (REPO_ROOT / ".." / "env.sh").resolve()
DEFAULT_ARCH = "sm_89"
DEFAULT_PTX_VERSION = "8.0"
WARP_LANES = 32
M_DIM = 16
N_DIM = 8
K_DIM = 16
LOGICAL_HALF_PER_REG = 2
LOGICAL_HALF_PER_THREAD = N_DIM // 2
MMA_A_REGS = 4
MMA_B_REGS = 2
MMA_C_REGS = 2
MMA_D_REGS = 2
F16_VALUES = (
    0x3C00,  # 1.0
    0xBC00,  # -1.0
    0x3800,  # 0.5
    0x4000,  # 2.0
    0x3400,  # 0.25
    0xC000,  # -2.0
    0x3E00,  # 1.5
    0xB800,  # -0.5
)


def load_helper():
    spec = importlib.util.spec_from_file_location("fp16_mma_spike_cpu_ref", HELPER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load helper module: {HELPER_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


HELPER = load_helper()
FP16_SHAPE = HELPER.SHAPE_M16N8K16


@dataclass(frozen=True)
class Variant:
    a_order: tuple[int, ...]
    b_order: tuple[int, ...]
    c_order: tuple[int, ...]
    d_order: tuple[int, ...]

    @property
    def name(self) -> str:
        return (
            f"a{_fmt_order(self.a_order)}"
            f"_b{_fmt_order(self.b_order)}"
            f"_c{_fmt_order(self.c_order)}"
            f"_d{_fmt_order(self.d_order)}"
        )


@dataclass(frozen=True)
class VariantResult:
    variant: Variant
    nan_pairs: int
    finite_pairs: int
    finite_pass: int
    max_abs_err: float
    max_ulp_err: int
    mismatch_count: int
    spike_nan_pairs: int
    spike_finite_pairs: int
    spike_finite_pass: int
    spike_max_abs_err: float
    spike_max_ulp_err: int
    spike_mismatch_count: int

    @property
    def cpu_pass(self) -> bool:
        return self.mismatch_count == 0

    @property
    def spike_pass(self) -> bool:
        return self.spike_mismatch_count == 0


def _fmt_order(order: Iterable[int]) -> str:
    return "".join(str(i) for i in order)


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Hand-written PTX vs Spike/CPU-ref probe for fp16 MMA.")
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0x20260413)
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC)
    ap.add_argument("--env-sh", type=Path, default=DEFAULT_ENV_SH)
    ap.add_argument("--arch", default=DEFAULT_ARCH)
    ap.add_argument("--ptx-version", default=DEFAULT_PTX_VERSION)
    ap.add_argument("--ulp-tol", type=int, default=1)
    ap.add_argument("--variant", default="all", help="Variant substring to run, or 'all'.")
    ap.add_argument("--load-mode", choices=("ptx", "cubin"), default="ptx")
    ap.add_argument("--keep-workdir", action="store_true")
    ap.add_argument("--skip-spike", action="store_true")
    return ap.parse_args()


def require_path(path: Path) -> None:
    if not path.exists():
        raise SystemExit(f"missing required path: {path}")


def finite_f16_bits(idx: int) -> int:
    return F16_VALUES[idx & (len(F16_VALUES) - 1)]


def sample_lane_index(seed: int, salt: int) -> int:
    return (((seed >> ((salt & 3) * 5)) ^ (salt * 13)) & 7)


def sample_packed_f16(seed: int, salt_lo: int, salt_hi: int) -> int:
    lo = finite_f16_bits(sample_lane_index(seed, salt_lo))
    hi = finite_f16_bits(sample_lane_index(seed, salt_hi))
    return lo | (hi << 16)


def flatten_lane_regs(regs: list[list[int]]) -> list[int]:
    return [word for lane in regs for word in lane]


def build_m16n8_inputs(seed_words: list[int]) -> tuple[list[list[int]], list[list[int]], list[list[int]]]:
    a_regs = [[0] * MMA_A_REGS for _ in range(WARP_LANES)]
    b_regs = [[0] * MMA_B_REGS for _ in range(WARP_LANES)]
    c_regs = [[0] * MMA_C_REGS for _ in range(WARP_LANES)]
    for gid in range(WARP_LANES):
        seed = HELPER.kernel_seed(seed_words[gid], gid, shape=FP16_SHAPE)
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


def load_logical_tiles(
    a_regs: list[list[int]], b_regs: list[list[int]], c_regs: list[list[int]]
) -> tuple[list[list[int]], list[list[int]], list[list[int]]]:
    a_tile = [[0] * K_DIM for _ in range(M_DIM)]
    b_tile = [[0] * K_DIM for _ in range(N_DIM)]
    c_tile = [[0] * N_DIM for _ in range(M_DIM)]

    for reg in range(MMA_A_REGS):
        for lane in range(WARP_LANES):
            value = a_regs[lane][reg]
            idx0 = reg * 64 + lane * 2
            idx1 = idx0 + 1
            a_tile[idx0 // K_DIM][idx0 % K_DIM] = value & 0xFFFF
            a_tile[idx1 // K_DIM][idx1 % K_DIM] = (value >> 16) & 0xFFFF

    for reg in range(MMA_B_REGS):
        for lane in range(WARP_LANES):
            value = b_regs[lane][reg]
            idx0 = reg * 64 + lane * 2
            idx1 = idx0 + 1
            b_tile[idx0 % N_DIM][idx0 // N_DIM] = value & 0xFFFF
            b_tile[idx1 % N_DIM][idx1 // N_DIM] = (value >> 16) & 0xFFFF

    for reg in range(MMA_C_REGS):
        for lane in range(WARP_LANES):
            value = c_regs[lane][reg]
            idx0 = reg * 64 + lane * 2
            idx1 = idx0 + 1
            c_tile[idx0 // N_DIM][idx0 % N_DIM] = value & 0xFFFF
            c_tile[idx1 // N_DIM][idx1 % N_DIM] = (value >> 16) & 0xFFFF

    return a_tile, b_tile, c_tile


def compute_cpu_reference(seed_words: list[int]) -> list[int]:
    a_regs, b_regs, c_regs = build_m16n8_inputs(seed_words)
    a_tile, b_tile, c_tile = load_logical_tiles(a_regs, b_regs, c_regs)
    d_tile = [[0] * N_DIM for _ in range(M_DIM)]

    for m in range(M_DIM):
        for n in range(N_DIM):
            acc = HELPER.u16_to_f16(c_tile[m][n])
            for k in range(K_DIM):
                acc += HELPER.u16_to_f16(a_tile[m][k]) * HELPER.u16_to_f16(b_tile[n][k])
            d_tile[m][n] = HELPER.f16_to_u16(acc)

    out: list[int] = []
    for gid in range(WARP_LANES):
        regs = []
        for reg in range(MMA_D_REGS):
            idx0 = reg * 64 + gid * 2
            idx1 = idx0 + 1
            lo = d_tile[idx0 // N_DIM][idx0 % N_DIM]
            hi = d_tile[idx1 // N_DIM][idx1 % N_DIM]
            regs.append(lo | (hi << 16))
        out.append(regs[0] if (gid & 1) == 0 else regs[1])
    return out


def build_variants() -> list[Variant]:
    orders_2 = ((0, 1), (1, 0))
    orders_4 = ((0, 1, 2, 3), (3, 2, 1, 0))
    variants: list[Variant] = []
    for a_order in orders_4:
        for b_order in orders_2:
            for c_order in orders_2:
                for d_order in orders_2:
                    variants.append(Variant(a_order=a_order, b_order=b_order, c_order=c_order, d_order=d_order))
    return variants


def variant_filter(name: str, variants: list[Variant]) -> list[Variant]:
    if name == "all":
        return variants
    selected = [v for v in variants if name in v.name]
    if not selected:
        raise SystemExit(f"no variants matched filter: {name}")
    return selected


def render_ptx_module(arch: str, ptx_version: str, variants: list[Variant]) -> str:
    header = [
        f".version {ptx_version}",
        f".target {arch}",
        ".address_size 64",
        "",
    ]
    bodies = []
    for variant in variants:
        bodies.extend(render_variant_kernel(variant))
        bodies.append("")
    return "\n".join(header + bodies) + "\n"


def render_variant_kernel(variant: Variant) -> list[str]:
    name = f"probe_{variant.name}"
    lines = [
        f".visible .entry {name}(",
        "    .param .u64 a_ptr,",
        "    .param .u64 b_ptr,",
        "    .param .u64 c_ptr,",
        "    .param .u64 d_ptr",
        ")",
        "{",
        "    .reg .pred %p;",
        "    .reg .u32 %r<4>;",
        "    .reg .u64 %rd<16>;",
        "    .reg .b32 %a<4>;",
        "    .reg .b32 %b<2>;",
        "    .reg .b32 %c<2>;",
        "    .reg .b32 %d<2>;",
        "",
        "    ld.param.u64 %rd1, [a_ptr];",
        "    ld.param.u64 %rd2, [b_ptr];",
        "    ld.param.u64 %rd3, [c_ptr];",
        "    ld.param.u64 %rd4, [d_ptr];",
        "    mov.u32 %r0, %tid.x;",
        "    mul.wide.u32 %rd5, %r0, 4;",
    ]
    for idx in range(MMA_A_REGS):
        lines.extend([
            f"    add.u64 %rd6, %rd1, {idx * 4};",
            f"    add.u64 %rd6, %rd6, %rd5;",
            f"    ld.global.b32 %a{idx}, [%rd6];",
        ])
    for idx in range(MMA_B_REGS):
        lines.extend([
            f"    add.u64 %rd6, %rd2, {idx * 4};",
            f"    add.u64 %rd6, %rd6, %rd5;",
            f"    ld.global.b32 %b{idx}, [%rd6];",
        ])
    for idx in range(MMA_C_REGS):
        lines.extend([
            f"    add.u64 %rd6, %rd3, {idx * 4};",
            f"    add.u64 %rd6, %rd6, %rd5;",
            f"    ld.global.b32 %c{idx}, [%rd6];",
        ])
    lines.extend([
        "",
        "    mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16",
        f"        {{{_emit_regs('d', variant.d_order)}}},",
        f"        {{{_emit_regs('a', variant.a_order)}}},",
        f"        {{{_emit_regs('b', variant.b_order)}}},",
        f"        {{{_emit_regs('c', variant.c_order)}}};",
        "",
    ])
    for idx in range(MMA_D_REGS):
        lines.extend([
            f"    add.u64 %rd6, %rd4, {idx * 4};",
            f"    add.u64 %rd6, %rd6, %rd5;",
            f"    st.global.b32 [%rd6], %d{idx};",
        ])
    lines.extend([
        "    ret;",
        "}",
    ])
    return lines


def _emit_regs(prefix: str, order: Iterable[int]) -> str:
    return ", ".join(f"%{prefix}{idx}" for idx in order)


def write_text(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8")


def shell_quote(s: str) -> str:
    return "'" + s.replace("'", "'\\''") + "'"


def run_checked(cmd: list[str], cwd: Path | None = None, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(cmd, cwd=str(cwd) if cwd else None, env=env, text=True, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(
            "command failed\n"
            f"cmd: {' '.join(shell_quote(x) for x in cmd)}\n"
            f"rc: {proc.returncode}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )
    return proc


class CudaDriver:
    def __init__(self) -> None:
        self.lib = ctypes.CDLL("libcuda.so.1")
        self._bind()
        self._check(self.lib.cuInit(0), "cuInit")
        self.dev = ctypes.c_int()
        self._check(self.lib.cuDeviceGet(ctypes.byref(self.dev), 0), "cuDeviceGet")
        self.ctx = ctypes.c_void_p()
        self._check(self.lib.cuDevicePrimaryCtxRetain(ctypes.byref(self.ctx), self.dev), "cuDevicePrimaryCtxRetain")
        self._check(self.lib.cuCtxSetCurrent(self.ctx), "cuCtxSetCurrent")

    def _bind(self) -> None:
        self.lib.cuInit.argtypes = [ctypes.c_uint]
        self.lib.cuInit.restype = ctypes.c_int
        self.lib.cuDeviceGet.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_int]
        self.lib.cuDeviceGet.restype = ctypes.c_int
        self.lib.cuDeviceGetName.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
        self.lib.cuDeviceGetName.restype = ctypes.c_int
        self.lib.cuDevicePrimaryCtxRetain.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_int]
        self.lib.cuDevicePrimaryCtxRetain.restype = ctypes.c_int
        self.lib.cuDevicePrimaryCtxRelease.argtypes = [ctypes.c_int]
        self.lib.cuDevicePrimaryCtxRelease.restype = ctypes.c_int
        self.lib.cuCtxSetCurrent.argtypes = [ctypes.c_void_p]
        self.lib.cuCtxSetCurrent.restype = ctypes.c_int
        self.lib.cuCtxSynchronize.argtypes = []
        self.lib.cuCtxSynchronize.restype = ctypes.c_int
        self.lib.cuModuleLoadDataEx.argtypes = [
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.c_void_p,
            ctypes.c_uint,
            ctypes.POINTER(ctypes.c_uint),
            ctypes.POINTER(ctypes.c_void_p),
        ]
        self.lib.cuModuleLoadDataEx.restype = ctypes.c_int
        self.lib.cuModuleLoadData.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p]
        self.lib.cuModuleLoadData.restype = ctypes.c_int
        self.lib.cuModuleUnload.argtypes = [ctypes.c_void_p]
        self.lib.cuModuleUnload.restype = ctypes.c_int
        self.lib.cuModuleGetFunction.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p, ctypes.c_char_p]
        self.lib.cuModuleGetFunction.restype = ctypes.c_int
        self.lib.cuMemAlloc_v2.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.c_size_t]
        self.lib.cuMemAlloc_v2.restype = ctypes.c_int
        self.lib.cuMemFree_v2.argtypes = [ctypes.c_uint64]
        self.lib.cuMemFree_v2.restype = ctypes.c_int
        self.lib.cuMemcpyHtoD_v2.argtypes = [ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t]
        self.lib.cuMemcpyHtoD_v2.restype = ctypes.c_int
        self.lib.cuMemcpyDtoH_v2.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_size_t]
        self.lib.cuMemcpyDtoH_v2.restype = ctypes.c_int
        self.lib.cuLaunchKernel.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_void_p),
        ]
        self.lib.cuLaunchKernel.restype = ctypes.c_int
        self.lib.cuGetErrorName.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_char_p)]
        self.lib.cuGetErrorName.restype = ctypes.c_int
        self.lib.cuGetErrorString.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_char_p)]
        self.lib.cuGetErrorString.restype = ctypes.c_int

    def _check(self, rc: int, where: str) -> None:
        if rc == 0:
            return
        name = ctypes.c_char_p()
        desc = ctypes.c_char_p()
        self.lib.cuGetErrorName(rc, ctypes.byref(name))
        self.lib.cuGetErrorString(rc, ctypes.byref(desc))
        raise RuntimeError(f"{where} failed rc={rc} name={name.value.decode() if name.value else '?'} desc={desc.value.decode() if desc.value else '?'}")

    def device_name(self) -> str:
        buf = ctypes.create_string_buffer(256)
        self._check(self.lib.cuDeviceGetName(buf, len(buf), self.dev), "cuDeviceGetName")
        return buf.value.decode()

    def alloc(self, size: int) -> int:
        ptr = ctypes.c_uint64()
        self._check(self.lib.cuMemAlloc_v2(ctypes.byref(ptr), size), "cuMemAlloc_v2")
        return int(ptr.value)

    def free(self, ptr: int) -> None:
        self._check(self.lib.cuMemFree_v2(ctypes.c_uint64(ptr)), "cuMemFree_v2")

    def memcpy_htod(self, dst: int, src_bytes: bytes) -> None:
        buf = ctypes.create_string_buffer(src_bytes, len(src_bytes))
        self._check(self.lib.cuMemcpyHtoD_v2(ctypes.c_uint64(dst), ctypes.cast(buf, ctypes.c_void_p), len(src_bytes)), "cuMemcpyHtoD_v2")

    def memcpy_dtoh(self, dst: bytearray, src: int) -> None:
        out_buf = (ctypes.c_ubyte * len(dst)).from_buffer(dst)
        self._check(self.lib.cuMemcpyDtoH_v2(ctypes.cast(out_buf, ctypes.c_void_p), ctypes.c_uint64(src), len(dst)), "cuMemcpyDtoH_v2")

    def load_module_from_ptx(self, ptx_text: str) -> ctypes.c_void_p:
        mod = ctypes.c_void_p()
        ptx_buf = ctypes.create_string_buffer(ptx_text.encode("utf-8") + b"\0")
        err_log = ctypes.create_string_buffer(8192)
        info_log = ctypes.create_string_buffer(8192)
        opts = (ctypes.c_uint * 5)(
            5,  # CU_JIT_ERROR_LOG_BUFFER
            6,  # CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES
            3,  # CU_JIT_INFO_LOG_BUFFER
            4,  # CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES
            8,  # CU_JIT_TARGET_FROM_CUCONTEXT
        )
        vals = (ctypes.c_void_p * 5)(
            ctypes.cast(err_log, ctypes.c_void_p),
            ctypes.c_void_p(ctypes.sizeof(err_log)),
            ctypes.cast(info_log, ctypes.c_void_p),
            ctypes.c_void_p(ctypes.sizeof(info_log)),
            ctypes.c_void_p(1),
        )
        self._check(
            self.lib.cuModuleLoadDataEx(ctypes.byref(mod), ctypes.cast(ptx_buf, ctypes.c_void_p), len(opts), opts, vals),
            f"cuModuleLoadDataEx\ninfo:\n{info_log.value.decode(errors='replace')}\nerror:\n{err_log.value.decode(errors='replace')}",
        )
        return mod

    def load_module_from_cubin(self, cubin: bytes) -> ctypes.c_void_p:
        mod = ctypes.c_void_p()
        cubin_buf = ctypes.create_string_buffer(cubin, len(cubin))
        self._check(self.lib.cuModuleLoadData(ctypes.byref(mod), ctypes.cast(cubin_buf, ctypes.c_void_p)), "cuModuleLoadData")
        return mod

    def get_function(self, mod: ctypes.c_void_p, name: str) -> ctypes.c_void_p:
        fun = ctypes.c_void_p()
        self._check(self.lib.cuModuleGetFunction(ctypes.byref(fun), mod, name.encode("utf-8")), f"cuModuleGetFunction({name})")
        return fun

    def launch(self, fun: ctypes.c_void_p, grid: tuple[int, int, int], block: tuple[int, int, int], kernel_params: list[int]) -> None:
        keepalive = [ctypes.c_uint64(v) for v in kernel_params]
        params = (ctypes.c_void_p * len(keepalive))()
        for i, obj in enumerate(keepalive):
            params[i] = ctypes.cast(ctypes.byref(obj), ctypes.c_void_p)
        self._check(
            self.lib.cuLaunchKernel(
                fun,
                grid[0], grid[1], grid[2],
                block[0], block[1], block[2],
                0,
                None,
                params,
                None,
            ),
            "cuLaunchKernel",
        )
        self._check(self.lib.cuCtxSynchronize(), "cuCtxSynchronize")

    def unload_module(self, mod: ctypes.c_void_p) -> None:
        self._check(self.lib.cuModuleUnload(mod), "cuModuleUnload")

    def close(self) -> None:
        if self.ctx:
            self._check(self.lib.cuDevicePrimaryCtxRelease(self.dev), "cuDevicePrimaryCtxRelease")
            self.ctx = ctypes.c_void_p()


def make_input_buffers(seed: int) -> tuple[list[int], list[int], list[int], list[int], list[int]]:
    seed_words = HELPER.build_seed_words(WARP_LANES, seed)
    a_regs, b_regs, c_regs = build_m16n8_inputs(seed_words)
    a_words = flatten_lane_regs(a_regs)
    b_words = flatten_lane_regs(b_regs)
    c_words = flatten_lane_regs(c_regs)
    expected_words = HELPER.compute_cpu_reference(seed_words, shape=FP16_SHAPE)
    return seed_words, a_words, b_words, c_words, expected_words


def write_u32_words(path: Path, words: list[int]) -> None:
    path.write_bytes(b"".join(int(w & 0xFFFFFFFF).to_bytes(4, "little") for w in words))


def compare_words(actual_words: list[int], expected_words: list[int], ulp_tol: int):
    return HELPER.compare_words(actual_words, expected_words, ulp_tol)


def run_spike(seed_words: list[int], workdir: Path, src: Path, exe: Path, env_sh: Path) -> list[int]:
    spike_dir = workdir / "spike"
    spike_dir.mkdir(parents=True, exist_ok=True)
    input_path = spike_dir / "in.bin"
    output_path = spike_dir / "out.bin"
    write_u32_words(input_path, seed_words)
    helper_src = spike_dir / "kernel.cl"
    HELPER.materialize_kernel_source(src, helper_src, shape=FP16_SHAPE)
    HELPER.run_spike(exe, env_sh, helper_src, spike_dir, input_path, output_path, shape=FP16_SHAPE)
    return HELPER.parse_u32_words(output_path)


def compile_cubin(ptx_path: Path, cubin_path: Path, arch: str) -> None:
    cmd = [
        "ptxas",
        "-arch",
        arch,
        "-o",
        str(cubin_path),
        str(ptx_path),
    ]
    run_checked(cmd)


def main() -> int:
    args = parse_args()
    for path in (args.exe, args.src, args.env_sh, HELPER_PATH):
        require_path(path.resolve())

    tempdir = tempfile.TemporaryDirectory(prefix="fp16_mma_ptx_probe_")
    workdir = Path(tempdir.name)
    if args.keep_workdir:
        print(f"[INFO] workdir={workdir}")

    cuda: CudaDriver | None = None
    module: ctypes.c_void_p | None = None
    try:
        seed_words, a_words, b_words, c_words, expected_words = make_input_buffers(args.seed)
        variants = variant_filter(args.variant, build_variants())
        ptx_path = workdir / "probe.ptx"
        ptx_text = render_ptx_module(args.arch, args.ptx_version, variants)
        write_text(ptx_path, ptx_text)

        spike_words = None
        if not args.skip_spike:
            spike_words = run_spike(seed_words, workdir, args.src.resolve(), args.exe.resolve(), args.env_sh.resolve())

        if args.load_mode == "cubin":
            cubin_path = workdir / "probe.cubin"
            compile_cubin(ptx_path, cubin_path, args.arch)
            cuda = CudaDriver()
            module = cuda.load_module_from_cubin(cubin_path.read_bytes())
        else:
            cuda = CudaDriver()
            module = cuda.load_module_from_ptx(ptx_text)

        try:
            results: list[VariantResult] = []
            print(f"[INFO] device={cuda.device_name()}")
            print(f"[INFO] seed=0x{args.seed:08x} ulp_tol={args.ulp_tol} load_mode={args.load_mode}")
            print(f"[INFO] variants={len(variants)}")

            for variant in variants:
                kernel_name = f"probe_{variant.name}"
                fun = cuda.get_function(module, kernel_name)
                a_dev = cuda.alloc(len(a_words) * 4)
                b_dev = cuda.alloc(len(b_words) * 4)
                c_dev = cuda.alloc(len(c_words) * 4)
                d_dev = cuda.alloc(len(expected_words) * 4)
                try:
                    cuda.memcpy_htod(a_dev, b"".join(int(w).to_bytes(4, "little") for w in a_words))
                    cuda.memcpy_htod(b_dev, b"".join(int(w).to_bytes(4, "little") for w in b_words))
                    cuda.memcpy_htod(c_dev, b"".join(int(w).to_bytes(4, "little") for w in c_words))
                    cuda.launch(fun, (1, 1, 1), (WARP_LANES, 1, 1), [a_dev, b_dev, c_dev, d_dev])

                    out_bytes = bytearray(len(expected_words) * 4)
                    cuda.memcpy_dtoh(out_bytes, d_dev)
                    actual_words = [int.from_bytes(out_bytes[i:i + 4], "little") for i in range(0, len(out_bytes), 4)]
                    metrics = compare_words(actual_words, expected_words, args.ulp_tol)
                    nan_pairs, finite_pairs, finite_pass, max_abs_err, max_ulp_err, failures = metrics
                    spike_nan_pairs = spike_finite_pairs = spike_finite_pass = spike_max_abs_err = spike_max_ulp_err = spike_mismatch_count = 0
                    if spike_words is not None:
                        spike_metrics = compare_words(actual_words, spike_words, args.ulp_tol)
                        spike_nan_pairs, spike_finite_pairs, spike_finite_pass, spike_max_abs_err, spike_max_ulp_err, spike_failures = spike_metrics
                        spike_mismatch_count = len(spike_failures)
                    result = VariantResult(
                        variant=variant,
                        nan_pairs=nan_pairs,
                        finite_pairs=finite_pairs,
                        finite_pass=finite_pass,
                        max_abs_err=max_abs_err,
                        max_ulp_err=max_ulp_err,
                        mismatch_count=len(failures),
                        spike_nan_pairs=spike_nan_pairs,
                        spike_finite_pairs=spike_finite_pairs,
                        spike_finite_pass=spike_finite_pass,
                        spike_max_abs_err=spike_max_abs_err,
                        spike_max_ulp_err=spike_max_ulp_err,
                        spike_mismatch_count=spike_mismatch_count,
                    )
                    results.append(result)
                    status = "PASS" if result.cpu_pass and (spike_words is None or result.spike_pass) else "FAIL"
                    print(
                        f"[{status}] {variant.name} cpu_pass={result.cpu_pass} spike_pass={result.spike_pass if spike_words is not None else 'n/a'} "
                        f"nan_pairs={nan_pairs} finite_pass={finite_pass}/{finite_pairs} max_ulp={max_ulp_err}"
                    )
                    if not result.cpu_pass:
                        for item in failures[:8]:
                            print(f"  cpu {item}")
                    if spike_words is not None and not result.spike_pass:
                        for item in spike_failures[:8]:
                            print(f"  spike {item}")
                finally:
                    cuda.free(a_dev)
                    cuda.free(b_dev)
                    cuda.free(c_dev)
                    cuda.free(d_dev)

            passed = [r for r in results if r.cpu_pass and (spike_words is None or r.spike_pass)]
            if passed:
                print(f"[SUMMARY] matched_variants={len(passed)}/{len(results)}")
                for r in passed:
                    print(f"  {r.variant.name}")
                return 0
            print("[SUMMARY] no variant matched both CPU ref and Spike")
            return 1
        finally:
            if cuda is not None and module is not None:
                cuda.unload_module(module)
            if cuda is not None:
                cuda.close()
    finally:
        if args.keep_workdir:
            print(f"[INFO] kept workdir={workdir}")
        else:
            tempdir.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
