#!/usr/bin/env python3
"""
背景
- 之前的 no-`ldmatrix` probe 直接把 PTX fragment window 当成 kernel 输入，已经确认 `C/D` 路径可行，
  但 `A/B` 路径在随机样本上仍不对。
- 进一步排查后发现，PTX direct-lane `mma.sync` 更稳妥的做法不是从 fragment-packed window 直接 load，
  而是从 logical tile 内存按官方 direct-load offset 组装寄存器。
- 本脚本保留隔离实验边界，只在 `lab/07_fp16_mma_ptx_probe/` 下验证这条 no-`ldmatrix` 路径。

需求/作用
- 为 `m16n8k16 row.col f16->f16` 提供一个端到端正确的 no-`ldmatrix` direct-lane PTX probe。
- host 侧复用现有 helper 的随机输入与 CPU reference 口径。
- PTX 侧直接从 logical A/B/C tile 内存按 lane-specific offset 载入寄存器，再执行
  `mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16`。
- 输出同时对比 CPU reference logical tile，以及 Spike / Ventus raw `D` window。

用法
- `python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_direct_logical.py --seed 0x20260413`
- `python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_direct_logical.py --load-mode cubin`
- `python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_direct_logical.py --keep-workdir`

实现原理/处理步骤
1. 用现有 helper 生成 32-lane seed，并重建 Ventus 语义下的 logical `A/B/C` tile。
2. host 将 `A/B/C` 分别平铺成 logical tile 内存：
   - `A`: row-major `16x16`
   - `B`: row-major `8x16`，即 helper 的 `b_tile[n][k]`
   - `C/D`: row-major `16x8`
3. PTX kernel 以 lane=`tid.x`、group=`lane>>2`、thread=`lane&3` 计算 direct-load offset：
   - `A` base=`group*32 + thread*4` bytes，寄存器偏移 `0 / 256 / 16 / 272`
   - `B` base=`group*32 + thread*4` bytes，寄存器偏移 `0 / 16`
   - `C/D` base=`group*16 + thread*4` bytes，寄存器偏移 `0 / 128`
4. 执行 no-`ldmatrix` direct-lane `mma.sync`，读回 logical `D` tile。
5. 将 logical `D` tile 同时：
   - 直接与 CPU reference logical tile 做逐 half 精确比对
   - pack 回 Ventus raw `D` window，与 Spike 输出按现有 ULP 规则比较
"""

from __future__ import annotations

import argparse
import importlib.util
import sys
import tempfile
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
HELPER_PATH = REPO_ROOT / "tools" / "fp16_mma_spike_cpu_ref.py"
DIRECT_PTX_PATH = SCRIPT_DIR / "probe_fp16_mma_ptx.py"
LDMATRIX_PATH = SCRIPT_DIR / "probe_fp16_mma_ptx_ldmatrix.py"
DEFAULT_ARCH = "sm_89"
DEFAULT_PTX_VERSION = "8.0"
WARP_LANES = 32
M_DIM = 16
N_DIM = 8
K_DIM = 16


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


HELPER = load_module(HELPER_PATH, "fp16_mma_spike_cpu_ref")
PTX = load_module(DIRECT_PTX_PATH, "probe_fp16_mma_ptx_direct_base")
LDMATRIX = load_module(LDMATRIX_PATH, "probe_fp16_mma_ptx_ldmatrix_base")


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="No-ldmatrix direct-lane PTX probe over logical A/B/C tiles.")
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0x20260413)
    ap.add_argument("--exe", type=Path, default=HELPER.DEFAULT_EXE)
    ap.add_argument("--src", type=Path, default=HELPER.DEFAULT_SRC)
    ap.add_argument("--env-sh", type=Path, default=HELPER.DEFAULT_ENV_SH)
    ap.add_argument("--arch", default=DEFAULT_ARCH)
    ap.add_argument("--ptx-version", default=DEFAULT_PTX_VERSION)
    ap.add_argument("--ulp-tol", type=int, default=1)
    ap.add_argument("--load-mode", choices=("ptx", "cubin"), default="ptx")
    ap.add_argument("--keep-workdir", action="store_true")
    return ap.parse_args()


def require_path(path: Path) -> None:
    if not path.exists():
        raise SystemExit(f"missing required path: {path}")


def tile_to_bytes_u16(tile: list[list[int]]) -> bytes:
    return b"".join(int(value & 0xFFFF).to_bytes(2, "little") for row in tile for value in row)


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


def compare_tile_exact(actual: list[list[int]], expected: list[list[int]]) -> None:
    for r in range(len(expected)):
        for c in range(len(expected[r])):
            if actual[r][c] != expected[r][c]:
                raise RuntimeError(
                    f"logical D mismatch at ({r},{c}) actual=0x{actual[r][c]:04x} expected=0x{expected[r][c]:04x}"
                )


def render_ptx_module(arch: str, ptx_version: str) -> str:
    return "\n".join(
        [
            f".version {ptx_version}",
            f".target {arch}",
            ".address_size 64",
            "",
            ".visible .entry probe_direct_logical(",
            "    .param .u64 a_ptr,",
            "    .param .u64 b_ptr,",
            "    .param .u64 c_ptr,",
            "    .param .u64 d_ptr",
            ")",
            "{",
            "    .reg .u32 %r<8>;",
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
            "",
            "    shr.u32 %r1, %r0, 2;",
            "    and.b32 %r2, %r0, 3;",
            "",
            "    mul.lo.u32 %r3, %r1, 32;",
            "    mul.lo.u32 %r4, %r2, 4;",
            "    add.u32 %r5, %r3, %r4;",
            "    mul.wide.u32 %rd5, %r5, 1;",
            "",
            "    add.u64 %rd6, %rd1, %rd5;",
            "    ld.global.b32 %a0, [%rd6+0];",
            "    ld.global.b32 %a1, [%rd6+256];",
            "    ld.global.b32 %a2, [%rd6+16];",
            "    ld.global.b32 %a3, [%rd6+272];",
            "",
            "    add.u64 %rd7, %rd2, %rd5;",
            "    ld.global.b32 %b0, [%rd7+0];",
            "    ld.global.b32 %b1, [%rd7+16];",
            "",
            "    mul.lo.u32 %r6, %r1, 16;",
            "    add.u32 %r7, %r6, %r4;",
            "    mul.wide.u32 %rd8, %r7, 1;",
            "",
            "    add.u64 %rd9, %rd3, %rd8;",
            "    ld.global.b32 %c0, [%rd9+0];",
            "    ld.global.b32 %c1, [%rd9+128];",
            "",
            "    mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16",
            "        {%d0,%d1}, {%a0,%a1,%a2,%a3}, {%b0,%b1}, {%c0,%c1};",
            "",
            "    add.u64 %rd10, %rd4, %rd8;",
            "    st.global.b32 [%rd10+0], %d0;",
            "    st.global.b32 [%rd10+128], %d1;",
            "    ret;",
            "}",
            "",
        ]
    )


def run_probe(
    a_tile: list[list[int]],
    b_tile: list[list[int]],
    c_tile: list[list[int]],
    arch: str,
    ptx_version: str,
    load_mode: str,
) -> list[list[int]]:
    ptx_text = render_ptx_module(arch, ptx_version)
    cuda = PTX.CudaDriver()
    module = None
    try:
        if load_mode == "cubin":
            tempdir = tempfile.TemporaryDirectory(prefix="fp16_mma_ptx_direct_logical_cubin_")
            try:
                ptx_path = Path(tempdir.name) / "probe.ptx"
                cubin_path = Path(tempdir.name) / "probe.cubin"
                ptx_path.write_text(ptx_text, encoding="utf-8")
                PTX.compile_cubin(ptx_path, cubin_path, arch)
                module = cuda.load_module_from_cubin(cubin_path.read_bytes())
            finally:
                tempdir.cleanup()
        else:
            module = cuda.load_module_from_ptx(ptx_text)

        fun = cuda.get_function(module, "probe_direct_logical")
        a_bytes = tile_to_bytes_u16(a_tile)
        b_bytes = tile_to_bytes_u16(b_tile)
        c_bytes = tile_to_bytes_u16(c_tile)
        d_bytes = bytearray(M_DIM * N_DIM * 2)

        a_dev = cuda.alloc(len(a_bytes))
        b_dev = cuda.alloc(len(b_bytes))
        c_dev = cuda.alloc(len(c_bytes))
        d_dev = cuda.alloc(len(d_bytes))
        try:
            cuda.memcpy_htod(a_dev, a_bytes)
            cuda.memcpy_htod(b_dev, b_bytes)
            cuda.memcpy_htod(c_dev, c_bytes)
            cuda.launch(fun, (1, 1, 1), (WARP_LANES, 1, 1), [a_dev, b_dev, c_dev, d_dev])
            cuda.memcpy_dtoh(d_bytes, d_dev)
            return bytes_to_tile_u16(bytes(d_bytes), M_DIM, N_DIM)
        finally:
            cuda.free(a_dev)
            cuda.free(b_dev)
            cuda.free(c_dev)
            cuda.free(d_dev)
    finally:
        if module is not None:
            cuda.unload_module(module)
        cuda.close()


def main() -> int:
    args = parse_args()
    for path in (args.exe, args.src, args.env_sh, HELPER_PATH, DIRECT_PTX_PATH, LDMATRIX_PATH):
        require_path(path.resolve())

    tempdir = tempfile.TemporaryDirectory(prefix="fp16_mma_ptx_direct_logical_")
    workdir = Path(tempdir.name)
    if args.keep_workdir:
        print(f"[INFO] workdir={workdir}")

    try:
        seed_words = HELPER.build_seed_words(WARP_LANES, args.seed)
        a_regs, b_regs, c_regs = PTX.build_m16n8_inputs(seed_words)
        a_tile, b_tile, c_tile = PTX.load_logical_tiles(a_regs, b_regs, c_regs)
        expected_d_tile = LDMATRIX.compute_cpu_reference_tile(seed_words)
        actual_d_tile = run_probe(a_tile, b_tile, c_tile, args.arch, args.ptx_version, args.load_mode)

        compare_tile_exact(actual_d_tile, expected_d_tile)
        actual_words = LDMATRIX.pack_ventus_output_words(actual_d_tile)
        expected_words = HELPER.compute_cpu_reference(seed_words)
        cpu_metrics = HELPER.compare_words(actual_words, expected_words, args.ulp_tol)
        cpu_failures = cpu_metrics[-1]
        if cpu_failures:
            raise RuntimeError(f"PTX vs CPU ref mismatch: {cpu_failures[0]}")

        spike_words = PTX.run_spike(seed_words, workdir, args.src.resolve(), args.exe.resolve(), args.env_sh.resolve())
        spike_metrics = HELPER.compare_words(actual_words, spike_words, args.ulp_tol)
        spike_failures = spike_metrics[-1]
        if spike_failures:
            raise RuntimeError(f"PTX vs Spike mismatch: {spike_failures[0]}")

        print(f"[INFO] seed=0x{args.seed:08x} load_mode={args.load_mode}")
        print("[INFO] direct-load layout:")
        print("  A row-major 16x16: base=group*32+thread*4, offsets=0/256/16/272")
        print("  B row-major 8x16:  base=group*32+thread*4, offsets=0/16")
        print("  C row-major 16x8:  base=group*16+thread*4, offsets=0/128")
        print("[INFO] logical D tile matched CPU ref exactly")
        print("PASS fp16 mma PTX direct-lane logical-load vs Spike vs CPU ref")
        return 0
    finally:
        if args.keep_workdir:
            print(f"[INFO] kept workdir={workdir}")
        else:
            tempdir.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
