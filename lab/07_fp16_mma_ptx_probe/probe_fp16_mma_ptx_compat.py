#!/usr/bin/env python3
"""
背景
- Ventus raw MMA ABI 已经在隔离实验里确认过：`a/b/c/d` 的 raw 寄存器窗口可以从 Ventus kernel
  直接导出，并与 helper 口径一致。
- 直接把 Ventus raw 窗口喂给 PTX `mma.sync` 的 no-`ldmatrix` 路径仍然没有跑通，但这不等于输入侧
  没有解法；PTX 侧的 fragment ABI 允许先做一个兼容性的 host transform。
- 这个 probe 专注于把“Ventus raw window -> PTX fragment window”的变换钉死，并验证 direct-lane
  `mma.sync` 的输出覆盖情况。

需求/作用
- 以 `m16n8k16 row.col f16->f16` 为目标。
- 先把 Ventus 逻辑 tile 重排成 PTX `mma.sync` 期望的 fragment 形态，再喂给 no-`ldmatrix` PTX kernel。
- 额外提供一个 fragment basis 校验：用唯一标记输入，确认 host transform 与 PTX fragment ABI 一致。
- 输出仍按 Ventus raw 口径做对照；如果 direct-lane 输出没有完整覆盖，会明确打印第一处 mismatch。

用法
- `python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_compat.py --seed 0x20260413`
- `python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_compat.py --validate-fragments`
- `python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_compat.py --keep-workdir`

实现原理/处理步骤
1. 复用 `tools/fp16_mma_spike_cpu_ref.py` 生成随机 seed，并重建 Ventus raw `a/b/c` logical tile。
2. 用显式的 host packing 规则把 logical tile 转换成 PTX fragment layout。
3. 可选运行一个 fragment dump PTX kernel，对照 basis 输入验证 packing 规则确实和 PTX `ldmatrix`
   得到的 fragment ABI 一致。
4. 运行当前 no-`ldmatrix` PTX `mma.sync` kernel，读回输出并与 CPU reference 做比较。
5. 对于尚未完全理解的 D 输出布局，保留第一处 mismatch、prefix 覆盖情况和未覆盖的尾段统计。
"""

from __future__ import annotations

import argparse
import importlib.util
import math
import subprocess
import sys
import tempfile
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
HELPER_PATH = REPO_ROOT / "tools" / "fp16_mma_spike_cpu_ref.py"
DIRECT_PTX_PATH = SCRIPT_DIR / "probe_fp16_mma_ptx.py"
DEFAULT_EXE = REPO_ROOT / "build" / "ventus_ocl_run"
DEFAULT_SRC = REPO_ROOT / "testcases" / "ocl_compare" / "custom_mma_kernels.cl"
DEFAULT_ENV_SH = (REPO_ROOT / ".." / "env.sh").resolve()
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
PTX = load_module(DIRECT_PTX_PATH, "probe_fp16_mma_ptx")


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Compatibility-transform PTX probe for fp16 MMA.")
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0x20260413)
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC)
    ap.add_argument("--env-sh", type=Path, default=DEFAULT_ENV_SH)
    ap.add_argument("--arch", default=DEFAULT_ARCH)
    ap.add_argument("--ptx-version", default=DEFAULT_PTX_VERSION)
    ap.add_argument("--ulp-tol", type=int, default=1)
    ap.add_argument("--validate-fragments", action="store_true", help="Run a fragment basis self-check first.")
    ap.add_argument("--keep-workdir", action="store_true")
    return ap.parse_args()


def require_path(path: Path) -> None:
    if not path.exists():
        raise SystemExit(f"missing required path: {path}")


def write_u32_words(path: Path, words: list[int]) -> None:
    path.write_bytes(b"".join(int(w & 0xFFFFFFFF).to_bytes(4, "little") for w in words))


def run_checked(cmd: str, cwd: Path) -> None:
    proc = subprocess.run(["bash", "-lc", cmd], cwd=str(cwd), text=True, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(
            f"command failed rc={proc.returncode}\ncmd: {cmd}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )


def pack_ptx_a(a_tile: list[list[int]]) -> list[int]:
    out: list[int] = []
    for row in range(8):
        for group in range(4):
            out.extend(
                [
                    a_tile[row][2 * group] | (a_tile[row][2 * group + 1] << 16),
                    a_tile[row + 8][2 * group] | (a_tile[row + 8][2 * group + 1] << 16),
                    a_tile[row][8 + 2 * group] | (a_tile[row][9 + 2 * group] << 16),
                    a_tile[row + 8][8 + 2 * group] | (a_tile[row + 8][9 + 2 * group] << 16),
                ]
            )
    return out


def pack_ptx_b(b_tile: list[list[int]]) -> list[int]:
    out: list[int] = []
    for row in range(8):
        for group in range(4):
            out.extend(
                [
                    b_tile[row][2 * group] | (b_tile[row][2 * group + 1] << 16),
                    b_tile[row][8 + 2 * group] | (b_tile[row][9 + 2 * group] << 16),
                ]
            )
    return out


def pack_ptx_c(c_tile: list[list[int]]) -> list[int]:
    out: list[int] = []
    for row in range(16):
        for group in range(2):
            out.extend(
                [
                    c_tile[row][4 * group] | (c_tile[row][4 * group + 1] << 16),
                    c_tile[row][4 * group + 2] | (c_tile[row][4 * group + 3] << 16),
                ]
            )
    return out


def transpose_tile(tile: list[list[int]]) -> list[list[int]]:
    rows = len(tile)
    cols = len(tile[0]) if rows else 0
    return [[tile[r][c] for r in range(rows)] for c in range(cols)]


def tile_to_bytes_u16(tile: list[list[int]]) -> bytes:
    return b"".join(int(value & 0xFFFF).to_bytes(2, "little") for row in tile for value in row)


def u16_to_f16(bits: int) -> float:
    import struct

    return struct.unpack("<e", struct.pack("<H", bits & 0xFFFF))[0]


def f16_to_u16(value: float) -> int:
    import struct

    if math.isnan(value):
        return 0x7E00
    if math.isinf(value):
        return 0xFC00 if value < 0 else 0x7C00
    try:
        return struct.unpack("<H", struct.pack("<e", float(value)))[0]
    except OverflowError:
        return 0xFC00 if value < 0 else 0x7C00


def compute_cpu_reference_tile(a_tile: list[list[int]], b_tile: list[list[int]], c_tile: list[list[int]]) -> list[list[int]]:
    d_tile = [[0] * N_DIM for _ in range(M_DIM)]
    for m in range(M_DIM):
        for n in range(N_DIM):
            acc = u16_to_f16(c_tile[m][n])
            for k in range(K_DIM):
                acc += u16_to_f16(a_tile[m][k]) * u16_to_f16(b_tile[n][k])
            d_tile[m][n] = f16_to_u16(acc)
    return d_tile


def make_unique_tile(rows: int, cols: int, start: int) -> list[list[int]]:
    tile = [[0] * cols for _ in range(rows)]
    value = start
    for r in range(rows):
        for c in range(cols):
            tile[r][c] = value
            value += 1
    return tile


def render_fragment_dump_ptx(arch: str, ptx_version: str) -> str:
    return "\n".join(
        [
            f".version {ptx_version}",
            f".target {arch}",
            ".address_size 64",
            "",
            ".visible .entry probe_dump(",
            "    .param .u64 a_ptr,",
            "    .param .u64 b_ptr,",
            "    .param .u64 c_ptr,",
            "    .param .u64 o_ptr",
            ")",
            "{",
            "    .shared .align 16 .b16 As[256];",
            "    .shared .align 16 .b16 Bs[128];",
            "    .reg .u32 %r<32>;",
            "    .reg .u64 %rd<32>;",
            "    .reg .b32 %a<4>;",
            "    .reg .b32 %b<2>;",
            "    .reg .b32 %c<2>;",
            "",
            "    ld.param.u64 %rd1, [a_ptr];",
            "    ld.param.u64 %rd2, [b_ptr];",
            "    ld.param.u64 %rd3, [c_ptr];",
            "    ld.param.u64 %rd4, [o_ptr];",
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
            "    mul.wide.u32 %rd15, %r0, 8;",
            "    add.u64 %rd16, %rd3, %rd15;",
            "    ld.global.b32 %c0, [%rd16+0];",
            "    ld.global.b32 %c1, [%rd16+4];",
            "",
            "    mul.wide.u32 %rd22, %r0, 32;",
            "    add.u64 %rd23, %rd4, %rd22;",
            "    st.global.b32 [%rd23+0], %a0;",
            "    st.global.b32 [%rd23+4], %a1;",
            "    st.global.b32 [%rd23+8], %a2;",
            "    st.global.b32 [%rd23+12], %a3;",
            "    st.global.b32 [%rd23+16], %b0;",
            "    st.global.b32 [%rd23+20], %b1;",
            "    st.global.b32 [%rd23+24], %c0;",
            "    st.global.b32 [%rd23+28], %c1;",
            "    ret;",
            "}",
            "",
        ]
    )


def run_fragment_dump(workdir: Path, arch: str, ptx_version: str) -> None:
    a_tile = make_unique_tile(M_DIM, K_DIM, 0x0401)
    b_tile = make_unique_tile(N_DIM, K_DIM, 0x0501)
    c_tile = make_unique_tile(M_DIM, N_DIM, 0x0601)
    a_bytes = tile_to_bytes_u16(transpose_tile(a_tile))
    b_bytes = tile_to_bytes_u16(transpose_tile(b_tile))
    c_bytes = tile_to_bytes_u16(c_tile)

    ptx_text = render_fragment_dump_ptx(arch, ptx_version)
    cuda = PTX.CudaDriver()
    module = cuda.load_module_from_ptx(ptx_text)
    fun = cuda.get_function(module, "probe_dump")
    a_dev = cuda.alloc(len(a_bytes))
    b_dev = cuda.alloc(len(b_bytes))
    c_dev = cuda.alloc(len(c_bytes))
    o_dev = cuda.alloc(32 * 8 * 4)
    try:
        cuda.memcpy_htod(a_dev, a_bytes)
        cuda.memcpy_htod(b_dev, b_bytes)
        cuda.memcpy_htod(c_dev, c_bytes)
        cuda.launch(fun, (1, 1, 1), (WARP_LANES, 1, 1), [a_dev, b_dev, c_dev, o_dev])
        out = bytearray(32 * 8 * 4)
        cuda.memcpy_dtoh(out, o_dev)
        words = [int.from_bytes(out[i:i + 4], "little") for i in range(0, len(out), 4)]
        a_dump: list[int] = []
        b_dump: list[int] = []
        c_dump: list[int] = []
        for lane in range(WARP_LANES):
            base = lane * 8
            a_dump.extend(words[base:base + 4])
            b_dump.extend(words[base + 4:base + 6])
            c_dump.extend(words[base + 6:base + 8])
        if a_dump != pack_ptx_a(a_tile):
            raise RuntimeError("A fragment basis check failed")
        if b_dump != pack_ptx_b(b_tile):
            raise RuntimeError("B fragment basis check failed")
        if c_dump != pack_ptx_c(c_tile):
            raise RuntimeError("C fragment basis check failed")
        print("[INFO] fragment basis check passed")
    finally:
        cuda.free(a_dev)
        cuda.free(b_dev)
        cuda.free(c_dev)
        cuda.free(o_dev)
        cuda.unload_module(module)
        cuda.close()


def main() -> int:
    args = parse_args()
    for path in (args.exe, args.src, args.env_sh, HELPER_PATH, DIRECT_PTX_PATH):
        require_path(path.resolve())

    tempdir = tempfile.TemporaryDirectory(prefix="fp16_mma_ptx_compat_")
    workdir = Path(tempdir.name)
    if args.keep_workdir:
        print(f"[INFO] workdir={workdir}")

    cuda: PTX.CudaDriver | None = None
    module = None
    try:
        if args.validate_fragments:
            run_fragment_dump(workdir, args.arch, args.ptx_version)

        seed_words = HELPER.build_seed_words(WARP_LANES, args.seed)
        a_regs, b_regs, c_regs = PTX.build_m16n8_inputs(seed_words)
        a_tile, b_tile, c_tile = PTX.load_logical_tiles(a_regs, b_regs, c_regs)
        a_words = pack_ptx_a(a_tile)
        b_words = pack_ptx_b(b_tile)
        c_words = pack_ptx_c(c_tile)
        expected_ptx_words = pack_ptx_c(compute_cpu_reference_tile(a_tile, b_tile, c_tile))

        variants = PTX.build_variants()
        target = [v for v in variants if v.a_order == (0, 1, 2, 3) and v.b_order == (0, 1) and v.c_order == (0, 1) and v.d_order == (0, 1)]
        if not target:
            raise RuntimeError("missing canonical PTX variant")
        ptx_text = PTX.render_ptx_module(args.arch, args.ptx_version, [target[0]])
        cuda = PTX.CudaDriver()
        module = cuda.load_module_from_ptx(ptx_text)
        fun = cuda.get_function(module, f"probe_{target[0].name}")

        a_dev = cuda.alloc(len(a_words) * 4)
        b_dev = cuda.alloc(len(b_words) * 4)
        c_dev = cuda.alloc(len(c_words) * 4)
        d_dev = cuda.alloc(64 * 4)
        try:
            cuda.memcpy_htod(a_dev, b"".join(int(w).to_bytes(4, "little") for w in a_words))
            cuda.memcpy_htod(b_dev, b"".join(int(w).to_bytes(4, "little") for w in b_words))
            cuda.memcpy_htod(c_dev, b"".join(int(w).to_bytes(4, "little") for w in c_words))
            cuda.launch(fun, (1, 1, 1), (WARP_LANES, 1, 1), [a_dev, b_dev, c_dev, d_dev])

            out = bytearray(64 * 4)
            cuda.memcpy_dtoh(out, d_dev)
            out_words = [int.from_bytes(out[i:i + 4], "little") for i in range(0, len(out), 4)]

            prefix_match = 0
            first_mismatch: str | None = None
            for idx, (actual, expected) in enumerate(zip(out_words, expected_ptx_words)):
                if actual != expected:
                    first_mismatch = f"word={idx} actual=0x{actual:08x} expected=0x{expected:08x}"
                    break
                prefix_match += 1

            tail_nonzero = sum(1 for word in out_words[33:] if word != 0)
            print(f"[INFO] seed=0x{args.seed:08x} prefix_match={prefix_match}/64 tail_nonzero={tail_nonzero}")
            if first_mismatch is not None:
                print(f"[INFO] first_mismatch={first_mismatch}")
            if prefix_match == len(expected_ptx_words) and tail_nonzero == 0:
                print("[FAIL] direct-lane output is still incomplete; no-ldmatrix path remains blocked")
                return 1
            print("[FAIL] direct-lane output does not yet match CPU ref")
            return 1
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
        if args.keep_workdir:
            print(f"[INFO] kept workdir={workdir}")
        else:
            tempdir.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
