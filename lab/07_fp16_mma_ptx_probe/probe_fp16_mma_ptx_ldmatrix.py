#!/usr/bin/env python3
"""
背景
- 需要在隔离目录里做一个最小的“手写 PTX 对照 Spike” probe，用来验证 Ventus 风格
  `fp16 -> fp16 row.col` MMA 在 PTX 侧的寄存器输入/输出形态。
- 之前的 direct-lane probe 证明了“把 Ventus lane-major 寄存器窗直接塞给 PTX”并不对，因此这里切换为
  更贴近 PTX 文档的 shared-memory + `ldmatrix` 载入方式，再与 Spike / CPU ref 对照。

需求/作用
- 先覆盖 `m16n8k16 row.col fp16->fp16`。
- 复用 `tools/fp16_mma_spike_cpu_ref.py` 的随机输入与 CPU reference 口径。
- PTX 侧使用 CUDA Driver API 直接加载手写 PTX，不依赖 `sbt_ptx`。
- 输出每个样本的 PTX / Spike / CPU ref 对比结果，容差沿用 `NaN` 分类相等、非 `NaN` <= 1 fp16 ULP。

用法
- `python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_ldmatrix.py --seed 0x20260413`
- `python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_ldmatrix.py --keep-workdir`
- `python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_ldmatrix.py --load-mode cubin`

实现原理/处理步骤
1. 用现有 helper 生成随机 seed，并把 Ventus 语义下的 A/B/C logical tile 以及 CPU ref 计算出来。
2. 通过现有 `ventus_ocl_run` 跑 Spike 侧对照，保留同一套输入生成和比较口径。
3. 在 PTX 中把 A/B 放进 shared memory，再用 `ldmatrix.sync.aligned...trans.shared.b16` 载入 `mma.sync` 需要的碎片。
   这里的关键点是：PTX 侧需要的是 A/B 的“物理转置”shared 布局，而不是直接把 Ventus logical tile 原样平铺进 shared。
4. C/D 仍按 PTX 文档里的 row/col 直接读写 row-major logical tile。
5. 将 PTX 回读的 logical D tile 再 pack 成 Ventus 风格输出，和 Spike / CPU ref 做同样的 ULP 比较。
"""

from __future__ import annotations

import argparse
import ctypes
import importlib.util
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path


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
A_WORDS = M_DIM * K_DIM // 2  # packed u16x2 words
B_WORDS = K_DIM * N_DIM // 2
C_WORDS = M_DIM * N_DIM // 2
D_WORDS = M_DIM * N_DIM // 2
SHARED_A_WORDS = 16 * 16 // 2
SHARED_B_WORDS = 16 * 8 // 2


def load_helper():
    spec = importlib.util.spec_from_file_location("fp16_mma_spike_cpu_ref", HELPER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load helper module: {HELPER_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


HELPER = load_helper()


@dataclass(frozen=True)
class ProbeResult:
    nan_pairs: int
    finite_pairs: int
    finite_pass: int
    max_abs_err: float
    max_ulp_err: int
    mismatch_count: int

    @property
    def passed(self) -> bool:
        return self.mismatch_count == 0


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Hand-written PTX vs Spike/CPU-ref probe for fp16 MMA.")
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0x20260413)
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC)
    ap.add_argument("--env-sh", type=Path, default=DEFAULT_ENV_SH)
    ap.add_argument("--arch", default=DEFAULT_ARCH)
    ap.add_argument("--ptx-version", default=DEFAULT_PTX_VERSION)
    ap.add_argument("--ulp-tol", type=int, default=1)
    ap.add_argument("--load-mode", choices=("ptx", "cubin"), default="ptx")
    ap.add_argument("--keep-workdir", action="store_true")
    return ap.parse_args()


def require_path(path: Path) -> None:
    if not path.exists():
        raise SystemExit(f"missing required path: {path}")


def tile_to_u16_words(tile: list[list[int]]) -> list[int]:
    return [value & 0xFFFF for row in tile for value in row]


def words_to_tile_u16(words: list[int], rows: int, cols: int) -> list[list[int]]:
    if len(words) != rows * cols:
        raise RuntimeError(f"unexpected word count: {len(words)} expected={rows * cols}")
    out = [[0] * cols for _ in range(rows)]
    idx = 0
    for r in range(rows):
        for c in range(cols):
            out[r][c] = words[idx] & 0xFFFF
            idx += 1
    return out


def tile_to_bytes_u16(tile: list[list[int]]) -> bytes:
    return b"".join(int(v & 0xFFFF).to_bytes(2, "little") for row in tile for v in row)


def bytes_to_tile_u16(blob: bytes, rows: int, cols: int) -> list[list[int]]:
    expected = rows * cols * 2
    if len(blob) != expected:
        raise RuntimeError(f"unexpected byte count: {len(blob)} expected={expected}")
    out = [[0] * cols for _ in range(rows)]
    idx = 0
    for r in range(rows):
        for c in range(cols):
            out[r][c] = int.from_bytes(blob[idx:idx + 2], "little")
            idx += 2
    return out


def transpose_tile(tile: list[list[int]]) -> list[list[int]]:
    rows = len(tile)
    cols = len(tile[0]) if rows else 0
    return [[tile[r][c] for r in range(rows)] for c in range(cols)]


def pack_ventus_output_words(d_tile: list[list[int]]) -> list[int]:
    out: list[int] = []
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


def build_logical_tiles(seed_words: list[int]) -> tuple[list[list[int]], list[list[int]], list[list[int]]]:
    a_regs, b_regs, c_regs = build_m16n8_inputs(seed_words)
    return load_logical_tiles(a_regs, b_regs, c_regs)


def build_m16n8_inputs(seed_words: list[int]) -> tuple[list[list[int]], list[list[int]], list[list[int]]]:
    a_regs = [[0] * 4 for _ in range(WARP_LANES)]
    b_regs = [[0] * 2 for _ in range(WARP_LANES)]
    c_regs = [[0] * 2 for _ in range(WARP_LANES)]
    for gid in range(WARP_LANES):
        seed = HELPER.kernel_seed(seed_words[gid], gid)
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


def compute_cpu_reference_tile(seed_words: list[int]) -> list[list[int]]:
    a_tile, b_tile, c_tile = build_logical_tiles(seed_words)
    d_tile = [[0] * N_DIM for _ in range(M_DIM)]
    for m in range(M_DIM):
        for n in range(N_DIM):
            acc = HELPER.u16_to_f16(c_tile[m][n])
            for k in range(K_DIM):
                acc += HELPER.u16_to_f16(a_tile[m][k]) * HELPER.u16_to_f16(b_tile[n][k])
            d_tile[m][n] = HELPER.f16_to_u16(acc)
    return d_tile


def sample_packed_f16(seed: int, salt_lo: int, salt_hi: int) -> int:
    f16_values = (
        0x3C00, 0xBC00, 0x3800, 0x4000, 0x3400, 0xC000, 0x3E00, 0xB800,
    )
    lo = f16_values[(((seed >> ((salt_lo & 3) * 5)) ^ (salt_lo * 13)) & 7)]
    hi = f16_values[(((seed >> ((salt_hi & 3) * 5)) ^ (salt_hi * 13)) & 7)]
    return lo | (hi << 16)


def compare_words(actual_words: list[int], expected_words: list[int], ulp_tol: int) -> ProbeResult:
    nan_pairs, finite_pairs, finite_pass, max_abs_err, max_ulp_err, failures = HELPER.compare_words(
        actual_words, expected_words, ulp_tol
    )
    return ProbeResult(
        nan_pairs=nan_pairs,
        finite_pairs=finite_pairs,
        finite_pass=finite_pass,
        max_abs_err=max_abs_err,
        max_ulp_err=max_ulp_err,
        mismatch_count=len(failures),
    )


def run_spike(seed_words: list[int], workdir: Path, src: Path, exe: Path, env_sh: Path) -> list[int]:
    spike_dir = workdir / "spike"
    spike_dir.mkdir(parents=True, exist_ok=True)
    input_path = spike_dir / "in.bin"
    output_path = spike_dir / "out.bin"
    input_path.write_bytes(b"".join(int(w & 0xFFFFFFFF).to_bytes(4, "little") for w in seed_words))
    helper_src = spike_dir / "kernel.cl"
    HELPER.materialize_kernel_source(src, helper_src)
    HELPER.run_spike(exe, env_sh, helper_src, spike_dir, input_path, output_path)
    return HELPER.parse_u32_words(output_path)


def render_ptx_module(arch: str, ptx_version: str) -> str:
    return "\n".join(
        [
            f".version {ptx_version}",
            f".target {arch}",
            ".address_size 64",
            "",
            ".visible .entry probe_m16n8k16_row_col_f16(",
            "    .param .u64 a_ptr,",
            "    .param .u64 b_ptr,",
            "    .param .u64 c_ptr,",
            "    .param .u64 d_ptr",
            ")",
            "{",
            "    .shared .align 16 .b16 As[256];",
            "    .shared .align 16 .b16 Bs[128];",
            "",
            "    .reg .pred %p;",
            "    .reg .u32 %r<32>;",
            "    .reg .u64 %rd<32>;",
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
            "    mul.wide.u32 %rd5, %r0, 16;",
            "    add.u64 %rd6, %rd1, %rd5;",
            "    mov.u64 %rd20, As;",
            "    add.u64 %rd7, %rd20, %rd5;",
            "    ld.global.b32 %r1, [%rd6+0];",
            "    st.shared.b32 [%rd7+0], %r1;",
            "    ld.global.b32 %r2, [%rd6+4];",
            "    st.shared.b32 [%rd7+4], %r2;",
            "    ld.global.b32 %r3, [%rd6+8];",
            "    st.shared.b32 [%rd7+8], %r3;",
            "    ld.global.b32 %r4, [%rd6+12];",
            "    st.shared.b32 [%rd7+12], %r4;",
            "",
            "    mul.wide.u32 %rd8, %r0, 8;",
            "    add.u64 %rd9, %rd2, %rd8;",
            "    mov.u64 %rd21, Bs;",
            "    add.u64 %rd10, %rd21, %rd8;",
            "    ld.global.b32 %r5, [%rd9+0];",
            "    st.shared.b32 [%rd10+0], %r5;",
            "    ld.global.b32 %r6, [%rd9+4];",
            "    st.shared.b32 [%rd10+4], %r6;",
            "",
            "    bar.sync 0;",
            "",
            "    mov.u32 %r7, %r0;",
            "    shr.u32 %r8, %r7, 3;",
            "    and.b32 %r9, %r7, 7;",
            "    and.b32 %r10, %r8, 1;",
            "    shr.u32 %r11, %r8, 1;",
            "    mul.lo.u32 %r12, %r11, 8;",
            "    add.u32 %r12, %r12, %r9;",
            "    mul.lo.u32 %r12, %r12, 16;",
            "    mul.lo.u32 %r13, %r10, 8;",
            "    add.u32 %r12, %r12, %r13;",
            "    mul.wide.u32 %rd11, %r12, 2;",
            "    add.u64 %rd12, %rd20, %rd11;",
            "    ldmatrix.sync.aligned.m8n8.x4.trans.shared.b16 {%a0,%a1,%a2,%a3}, [%rd12];",
            "",
            "    shr.u32 %r13, %r7, 3;",
            "    and.b32 %r14, %r7, 7;",
            "    and.b32 %r15, %r13, 1;",
            "    shr.u32 %r16, %r13, 1;",
            "    mul.lo.u32 %r17, %r15, 8;",
            "    add.u32 %r17, %r17, %r14;",
            "    mul.lo.u32 %r17, %r17, 8;",
            "    add.u32 %r17, %r17, %r16;",
            "    mul.wide.u32 %rd13, %r17, 2;",
            "    add.u64 %rd14, %rd21, %rd13;",
            "    ldmatrix.sync.aligned.m8n8.x2.trans.shared.b16 {%b0,%b1}, [%rd14];",
            "",
            "    shr.u32 %r18, %r7, 2;",
            "    and.b32 %r19, %r7, 3;",
            "    mul.lo.u32 %r20, %r19, 2;",
            "    mul.lo.u32 %r21, %r18, 8;",
            "    add.u32 %r21, %r21, %r20;",
            "",
            "    mul.wide.u32 %rd15, %r21, 2;",
            "    add.u64 %rd16, %rd3, %rd15;",
            "    ld.global.b32 %c0, [%rd16+0];",
            "    ld.global.b32 %c1, [%rd16+128];",
            "",
            "    mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16",
            "        {%d0,%d1}, {%a0,%a1,%a2,%a3}, {%b0,%b1}, {%c0,%c1};",
            "",
            "    mul.wide.u32 %rd17, %r18, 16;",
            "    mul.wide.u32 %rd18, %r19, 4;",
            "    add.u64 %rd19, %rd4, %rd17;",
            "    add.u64 %rd19, %rd19, %rd18;",
            "    st.global.b32 [%rd19+0], %d0;",
            "    add.u64 %rd22, %rd19, 128;",
            "    st.global.b32 [%rd22+0], %d1;",
            "    ret;",
            "}",
            "",
        ]
    )


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
        raise RuntimeError(
            f"{where} failed rc={rc} "
            f"name={name.value.decode() if name.value else '?'} "
            f"desc={desc.value.decode() if desc.value else '?'}"
        )

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
        opts = (ctypes.c_uint * 5)(5, 6, 3, 4, 8)
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

    def unload_module(self, mod: ctypes.c_void_p) -> None:
        self._check(self.lib.cuModuleUnload(mod), "cuModuleUnload")

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

    def close(self) -> None:
        if self.ctx:
            self._check(self.lib.cuDevicePrimaryCtxRelease(self.dev), "cuDevicePrimaryCtxRelease")
            self.ctx = ctypes.c_void_p()


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
        seed_words = HELPER.build_seed_words(WARP_LANES, args.seed)
        a_tile, b_tile, c_tile = build_logical_tiles(seed_words)
        expected_d_tile = compute_cpu_reference_tile(seed_words)
        expected_words = pack_ventus_output_words(expected_d_tile)

        spike_words = run_spike(seed_words, workdir, args.src.resolve(), args.exe.resolve(), args.env_sh.resolve())

        # PTX `ldmatrix ... trans` 侧需要 A/B 的物理转置 shared 布局。
        # 这里保留 Ventus logical tile 作为 CPU ref 输入，再单独 materialize PTX physical layout。
        a_bytes = tile_to_bytes_u16(transpose_tile(a_tile))
        b_bytes = tile_to_bytes_u16(transpose_tile(b_tile))
        c_bytes = tile_to_bytes_u16(c_tile)

        ptx_text = render_ptx_module(args.arch, args.ptx_version)
        ptx_path = workdir / "probe.ptx"
        ptx_path.write_text(ptx_text, encoding="utf-8")

        if args.load_mode == "cubin":
            cubin_path = workdir / "probe.cubin"
            subprocess.run(
                ["ptxas", "-arch", args.arch, "-o", str(cubin_path), str(ptx_path)],
                cwd=str(workdir),
                text=True,
                check=True,
            )
            cuda = CudaDriver()
            module = cuda.load_module_from_cubin(cubin_path.read_bytes())
        else:
            cuda = CudaDriver()
            module = cuda.load_module_from_ptx(ptx_text)

        try:
            print(f"[INFO] device={cuda.device_name()}")
            print(f"[INFO] seed=0x{args.seed:08x} ulp_tol={args.ulp_tol} load_mode={args.load_mode}")

            fun = cuda.get_function(module, "probe_m16n8k16_row_col_f16")
            a_dev = cuda.alloc(len(a_bytes))
            b_dev = cuda.alloc(len(b_bytes))
            c_dev = cuda.alloc(len(c_bytes))
            d_dev = cuda.alloc(M_DIM * N_DIM * 2)
            try:
                cuda.memcpy_htod(a_dev, a_bytes)
                cuda.memcpy_htod(b_dev, b_bytes)
                cuda.memcpy_htod(c_dev, c_bytes)
                cuda.launch(fun, (1, 1, 1), (WARP_LANES, 1, 1), [a_dev, b_dev, c_dev, d_dev])

                out_bytes = bytearray(M_DIM * N_DIM * 2)
                cuda.memcpy_dtoh(out_bytes, d_dev)
                actual_d_tile = bytes_to_tile_u16(bytes(out_bytes), M_DIM, N_DIM)
                actual_words = pack_ventus_output_words(actual_d_tile)

                cpu_result = compare_words(actual_words, expected_words, args.ulp_tol)
                spike_result = compare_words(actual_words, spike_words, args.ulp_tol)

                print(
                    f"[INFO] cpu nan_pairs={cpu_result.nan_pairs} finite_pairs={cpu_result.finite_pairs} "
                    f"finite_pass={cpu_result.finite_pass} max_abs_err={cpu_result.max_abs_err} "
                    f"max_ulp_err={cpu_result.max_ulp_err}"
                )
                print(
                    f"[INFO] spike nan_pairs={spike_result.nan_pairs} finite_pairs={spike_result.finite_pairs} "
                    f"finite_pass={spike_result.finite_pass} max_abs_err={spike_result.max_abs_err} "
                    f"max_ulp_err={spike_result.max_ulp_err}"
                )

                if not cpu_result.passed or not spike_result.passed:
                    print("[FAIL] PTX result diverged from CPU ref or Spike")
                    return 1

                print("PASS fp16 mma PTX vs Spike vs CPU ref")
                return 0
            finally:
                cuda.free(a_dev)
                cuda.free(b_dev)
                cuda.free(c_dev)
                cuda.free(d_dev)
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
