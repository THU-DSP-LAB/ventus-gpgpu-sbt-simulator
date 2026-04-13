#!/usr/bin/env python3
"""
背景
- `probe_fp16_mma_ptx_direct_logical.py` 已经证明：no-`ldmatrix` direct-lane `mma.sync` 可以跑通，
  但那条路径会先把 Ventus raw window 解释成 logical tile，再按 logical tile 内存取数。
- 这里进一步探索更直接的兼容变换：尽量不经过中间 logical tile 内存，而是直接从 Ventus raw
  `a/b/c` window 组装 PTX direct-lane `a0..a3 / b0..b1 / c0..c1`。
- 当前已知难点只在 `B`：
  - Ventus raw `A/C` 的 word 顺序可以视为 logical row-major word space
  - Ventus raw `B` 的每个 word 存的是“同一列上的相邻两行”，而 PTX `b0/b1` 需要“同一行上的相邻两列”

需求/作用
- 为 `m16n8k16 row.col f16->f16` 提供一个“Ventus raw reg window -> PTX direct-lane寄存器装填”的
  最小端到端 probe。
- 保留 raw-window 输入形态：
  - `A`: 4 x packed-u32 / lane
  - `B`: 2 x packed-u32 / lane
  - `C`: 2 x packed-u32 / lane
- 在 PTX kernel 中直接完成 raw-to-PTX-reg 映射，再执行
  `mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16`。
- 输出按 Ventus raw `D` window 口径回写，与 Spike / CPU ref 对比。

用法
- `python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_direct_raw.py --seed 0x20260413`
- `python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_direct_raw.py --load-mode cubin`
- `python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_direct_raw.py --keep-workdir`

实现原理/处理步骤
1. 复用现有 helper 生成 32-lane seed，并构造 Ventus raw `a/b/c` window。
2. PTX kernel 直接从 raw AoS window 取数：
   - `A`: 以 raw word index `w = group*8 + thread + {0,64,4,68}` 做 gather
   - `C`: 直接取当前 lane 的 `c.x/c.y`
   - `B`: 以 `src_lane0 = thread*8 + (group>>1)`、`src_lane1 = src_lane0 + 4` 取两个 raw word，
     再按 `group&1` 选择 low/high half，重打包成 PTX `b0/b1`
3. 执行 no-`ldmatrix` direct-lane `mma.sync`。
4. 按 Ventus raw `D` ABI 回写：偶数 lane 取 `d0`，奇数 lane 取 `d1`。
5. 与 CPU ref / Spike 做相同 ULP 口径比较。
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
DEFAULT_ARCH = "sm_89"
DEFAULT_PTX_VERSION = "8.0"
WARP_LANES = 32


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


HELPER = load_module(HELPER_PATH, "fp16_mma_spike_cpu_ref")
PTX = load_module(DIRECT_PTX_PATH, "probe_fp16_mma_ptx_direct_raw_base")


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="No-ldmatrix direct-lane PTX probe over Ventus raw A/B/C windows.")
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


def flatten_lane_regs(regs: list[list[int]]) -> list[int]:
    return [word for lane in regs for word in lane]


def render_ptx_module(arch: str, ptx_version: str) -> str:
    return "\n".join(
        [
            f".version {ptx_version}",
            f".target {arch}",
            ".address_size 64",
            "",
            ".visible .entry probe_direct_raw(",
            "    .param .u64 a_ptr,",
            "    .param .u64 b_ptr,",
            "    .param .u64 c_ptr,",
            "    .param .u64 d_ptr",
            ")",
            "{",
            "    .reg .pred %p<2>;",
            "    .reg .u32 %r<32>;",
            "    .reg .u64 %rd<32>;",
            "    .reg .b32 %a<4>;",
            "    .reg .b32 %b<2>;",
            "    .reg .b32 %c<2>;",
            "    .reg .b32 %d<2>;",
            "    .reg .u32 %tmp<16>;",
            "",
            "    ld.param.u64 %rd1, [a_ptr];",
            "    ld.param.u64 %rd2, [b_ptr];",
            "    ld.param.u64 %rd3, [c_ptr];",
            "    ld.param.u64 %rd4, [d_ptr];",
            "    mov.u32 %r0, %tid.x;",
            "    shr.u32 %r1, %r0, 2;",
            "    and.b32 %r2, %r0, 3;",
            "    shr.u32 %r3, %r1, 1;",
            "    and.b32 %r4, %r1, 1;",
            "",
            "    // A direct gather: Ventus raw A already follows logical row-major word order.",
            "    mul.lo.u32 %r5, %r1, 8;",
            "    add.u32 %r5, %r5, %r2;",
            "",
            "    add.u32 %r6, %r5, 0;",
            "    and.b32 %r7, %r6, 31;",
            "    shr.u32 %r8, %r6, 5;",
            "    mul.wide.u32 %rd5, %r7, 16;",
            "    mul.wide.u32 %rd6, %r8, 4;",
            "    add.u64 %rd7, %rd1, %rd5;",
            "    add.u64 %rd7, %rd7, %rd6;",
            "    ld.global.b32 %a0, [%rd7];",
            "",
            "    add.u32 %r6, %r5, 64;",
            "    and.b32 %r7, %r6, 31;",
            "    shr.u32 %r8, %r6, 5;",
            "    mul.wide.u32 %rd5, %r7, 16;",
            "    mul.wide.u32 %rd6, %r8, 4;",
            "    add.u64 %rd7, %rd1, %rd5;",
            "    add.u64 %rd7, %rd7, %rd6;",
            "    ld.global.b32 %a1, [%rd7];",
            "",
            "    add.u32 %r6, %r5, 4;",
            "    and.b32 %r7, %r6, 31;",
            "    shr.u32 %r8, %r6, 5;",
            "    mul.wide.u32 %rd5, %r7, 16;",
            "    mul.wide.u32 %rd6, %r8, 4;",
            "    add.u64 %rd7, %rd1, %rd5;",
            "    add.u64 %rd7, %rd7, %rd6;",
            "    ld.global.b32 %a2, [%rd7];",
            "",
            "    add.u32 %r6, %r5, 68;",
            "    and.b32 %r7, %r6, 31;",
            "    shr.u32 %r8, %r6, 5;",
            "    mul.wide.u32 %rd5, %r7, 16;",
            "    mul.wide.u32 %rd6, %r8, 4;",
            "    add.u64 %rd7, %rd1, %rd5;",
            "    add.u64 %rd7, %rd7, %rd6;",
            "    ld.global.b32 %a3, [%rd7];",
            "",
            "    // B repack: Ventus raw word packs vertical pairs, PTX b0/b1 need horizontal pairs.",
            "    mul.lo.u32 %r10, %r2, 8;",
            "    add.u32 %r10, %r10, %r3;",
            "    add.u32 %r11, %r10, 4;",
            "",
            "    mul.wide.u32 %rd8, %r10, 8;",
            "    add.u64 %rd9, %rd2, %rd8;",
            "    ld.global.b32 %tmp0, [%rd9+0];",
            "    ld.global.b32 %tmp1, [%rd9+4];",
            "",
            "    mul.wide.u32 %rd10, %r11, 8;",
            "    add.u64 %rd11, %rd2, %rd10;",
            "    ld.global.b32 %tmp2, [%rd11+0];",
            "    ld.global.b32 %tmp3, [%rd11+4];",
            "",
            "    and.b32 %tmp4, %tmp0, 0xffff;",
            "    and.b32 %tmp5, %tmp2, 0xffff;",
            "    shr.u32 %tmp6, %tmp0, 16;",
            "    shr.u32 %tmp7, %tmp2, 16;",
            "    setp.ne.u32 %p0, %r4, 0;",
            "    selp.b32 %tmp8, %tmp6, %tmp4, %p0;",
            "    selp.b32 %tmp9, %tmp7, %tmp5, %p0;",
            "    shl.b32 %tmp9, %tmp9, 16;",
            "    or.b32 %b0, %tmp8, %tmp9;",
            "",
            "    and.b32 %tmp4, %tmp1, 0xffff;",
            "    and.b32 %tmp5, %tmp3, 0xffff;",
            "    shr.u32 %tmp6, %tmp1, 16;",
            "    shr.u32 %tmp7, %tmp3, 16;",
            "    selp.b32 %tmp8, %tmp6, %tmp4, %p0;",
            "    selp.b32 %tmp9, %tmp7, %tmp5, %p0;",
            "    shl.b32 %tmp9, %tmp9, 16;",
            "    or.b32 %b1, %tmp8, %tmp9;",
            "",
            "    // C direct gather: Ventus raw C lane layout already matches current lane's c0/c1.",
            "    mul.wide.u32 %rd12, %r0, 8;",
            "    add.u64 %rd13, %rd3, %rd12;",
            "    ld.global.b32 %c0, [%rd13+0];",
            "    ld.global.b32 %c1, [%rd13+4];",
            "",
            "    mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16",
            "        {%d0,%d1}, {%a0,%a1,%a2,%a3}, {%b0,%b1}, {%c0,%c1};",
            "",
            "    // Ventus raw D ABI keeps d0 on even lanes and d1 on odd lanes.",
            "    and.b32 %r12, %r0, 1;",
            "    setp.eq.u32 %p1, %r12, 0;",
            "    selp.b32 %tmp10, %d0, %d1, %p1;",
            "    mul.wide.u32 %rd14, %r0, 4;",
            "    add.u64 %rd15, %rd4, %rd14;",
            "    st.global.b32 [%rd15], %tmp10;",
            "    ret;",
            "}",
            "",
        ]
    )


def run_probe(
    a_words: list[int],
    b_words: list[int],
    c_words: list[int],
    arch: str,
    ptx_version: str,
    load_mode: str,
) -> list[int]:
    ptx_text = render_ptx_module(arch, ptx_version)
    cuda = PTX.CudaDriver()
    module = None
    try:
        if load_mode == "cubin":
            tempdir = tempfile.TemporaryDirectory(prefix="fp16_mma_ptx_direct_raw_cubin_")
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

        fun = cuda.get_function(module, "probe_direct_raw")
        a_bytes = b"".join(int(word).to_bytes(4, "little") for word in a_words)
        b_bytes = b"".join(int(word).to_bytes(4, "little") for word in b_words)
        c_bytes = b"".join(int(word).to_bytes(4, "little") for word in c_words)
        d_bytes = bytearray(WARP_LANES * 4)

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
            return [int.from_bytes(d_bytes[i:i + 4], "little") for i in range(0, len(d_bytes), 4)]
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
    for path in (args.exe, args.src, args.env_sh, HELPER_PATH, DIRECT_PTX_PATH):
        require_path(path.resolve())

    tempdir = tempfile.TemporaryDirectory(prefix="fp16_mma_ptx_direct_raw_")
    workdir = Path(tempdir.name)
    if args.keep_workdir:
        print(f"[INFO] workdir={workdir}")

    try:
        seed_words = HELPER.build_seed_words(WARP_LANES, args.seed)
        a_regs, b_regs, c_regs = PTX.build_m16n8_inputs(seed_words)
        a_words = flatten_lane_regs(a_regs)
        b_words = flatten_lane_regs(b_regs)
        c_words = flatten_lane_regs(c_regs)
        expected_words = HELPER.compute_cpu_reference(seed_words)

        actual_words = run_probe(a_words, b_words, c_words, args.arch, args.ptx_version, args.load_mode)
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
        print("[INFO] direct raw mapping:")
        print("  A gather: w = group*8 + thread + {0,64,4,68}")
        print("  B repack: src_lane0 = thread*8 + (group>>1), src_lane1 = src_lane0 + 4, row parity = group&1")
        print("  C gather: current lane c.x/c.y")
        print("  D writeback: even lane -> d0, odd lane -> d1")
        print("PASS fp16 mma PTX direct-raw vs Spike vs CPU ref")
        return 0
    finally:
        if args.keep_workdir:
            print(f"[INFO] kept workdir={workdir}")
        else:
            tempdir.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
