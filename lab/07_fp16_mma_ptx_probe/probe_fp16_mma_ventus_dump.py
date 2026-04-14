#!/usr/bin/env python3
"""
背景
- 之前的 PTX probe 已经证明了 `ldmatrix` 路径可以跑通，但用户当前要看的不是 shared/fragments
  实现细节，而是 Ventus MMA 指令本身的寄存器 ABI。
- 为了把 ABI 钉死，这个 probe 先在 Ventus kernel 侧原样导出 `a/b/c/d` 的 raw 寄存器窗口，
  再把这份 Ventus dump 直接喂给一个不使用 `ldmatrix` 的 PTX `mma.sync` 变体。

需求/作用
- 生成一个最小的 Ventus OpenCL dump kernel：把 `uint4 a`、`uint2 b`、`uint2 c`、`uint2 d`
  原样写回 global memory。
- 先确认当前 helper 生成的输入窗口与 Ventus dump 是否一致。
- 再把 Ventus dump 出来的原始 `a/b/c` 直接喂给无 `ldmatrix` 的 PTX `mma.sync` 变体，
  以 Ventus `d` 的 raw 寄存器窗口为比较基准。

用法
- `python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ventus_dump.py --seed 0x20260413`
- `python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ventus_dump.py --keep-workdir`

实现原理/处理步骤
1. 复用 `tools/fp16_mma_spike_cpu_ref.py` 的种子与 Ventus 风格输入窗口生成口径。
2. 通过 `VENTUS_BACKEND=spike` 运行一个本地材料化的 dump kernel，把 `a/b/c/d` 直接导出到输出缓冲区。
3. 用导出的 Ventus raw `a/b/c` 作为当前已跑通的 no-`ldmatrix` direct-raw PTX probe 输入。
4. 把 PTX 输出和 Ventus dump 的 `d` raw 窗口直接对齐，避免再引入 logical tile 解释层。
"""

from __future__ import annotations

import argparse
import importlib.util
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
HELPER_PATH = REPO_ROOT / "tools" / "fp16_mma_spike_cpu_ref.py"
DIRECT_PTX_PATH = SCRIPT_DIR / "probe_fp16_mma_ptx.py"
DIRECT_RAW_PATH = SCRIPT_DIR / "probe_fp16_mma_ptx_direct_raw.py"
DEFAULT_EXE = REPO_ROOT / "build" / "ventus_ocl_run"
DEFAULT_SRC = REPO_ROOT / "testcases" / "ocl_compare" / "custom_mma_kernels.cl"
DEFAULT_ENV_SH = (REPO_ROOT / ".." / "env.sh").resolve()
WARP_LANES = 32
RAW_WORDS_PER_LANE = 10
SHAPE = "m16n8k16"
DUMP_KERNEL = "mt_custom_mma_m16n8k16_row_col_f16_f16_f16_f16_dump"
DUMP_SLOTS = 10


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


HELPER = load_module(HELPER_PATH, "fp16_mma_spike_cpu_ref")
PTX_PROBE = load_module(DIRECT_PTX_PATH, "probe_fp16_mma_ptx")
DIRECT_RAW_PROBE = load_module(DIRECT_RAW_PATH, "probe_fp16_mma_ptx_direct_raw")


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Ventus dump-ABI probe for fp16 MMA.")
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0x20260413)
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC)
    ap.add_argument("--env-sh", type=Path, default=DEFAULT_ENV_SH)
    ap.add_argument("--arch", default="sm_89")
    ap.add_argument("--ptx-version", default="8.0")
    ap.add_argument("--ulp-tol", type=int, default=1)
    ap.add_argument("--keep-workdir", action="store_true")
    return ap.parse_args()


def require_path(path: Path) -> None:
    if not path.exists():
        raise SystemExit(f"missing required path: {path}")


def run_checked(cmd: str, cwd: Path) -> None:
    proc = subprocess.run(["bash", "-lc", cmd], cwd=str(cwd), text=True, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"command failed rc={proc.returncode}\ncmd: {cmd}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")


def write_u32_words(path: Path, words: list[int]) -> None:
    path.write_bytes(struct.pack("<" + ("I" * len(words)), *words))


def parse_u32_words(path: Path) -> list[int]:
    blob = path.read_bytes()
    if len(blob) % 4 != 0:
        raise RuntimeError(f"invalid output byte size: {len(blob)}")
    return list(struct.unpack("<" + ("I" * (len(blob) // 4)), blob))


def materialize_dump_source(base_src: Path, dst_src: Path, slot: int) -> Path:
    text = base_src.read_text(encoding="utf-8")
    dump_kernel = """
#if defined(SBT_MMA_ENABLE_FP16_FP16_DUMP)
__kernel void mt_custom_mma_m16n8k16_row_col_f16_f16_f16_f16_dump(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint seed = A[gid] ^ (gid * 0x9e3779b9u) ^ 0x5bd1e995u;

  const uint4 a = (uint4)(sample_packed_f16(seed, 0u, 1u), sample_packed_f16(seed, 2u, 3u),
                          sample_packed_f16(seed, 4u, 5u), sample_packed_f16(seed, 6u, 7u));
  const uint2 b = (uint2)(sample_packed_f16(seed ^ 0x13579bdfu, 1u, 3u), sample_packed_f16(seed ^ 0x2468ace0u, 5u, 7u));
  const uint2 c = (uint2)(sample_packed_f16(seed ^ 0xa5a5a5a5u, 0u, 2u), sample_packed_f16(seed ^ 0x5a5a5a5au, 4u, 6u));
  const uint2 d = mma_m16n8k16_row_col_f16_f16_f16_f16(a, b, c);

  const uint slot = DUMP_SLOT;
  uint out = 0u;
  if (slot == 0u) out = a.x;
  else if (slot == 1u) out = a.y;
  else if (slot == 2u) out = a.z;
  else if (slot == 3u) out = a.w;
  else if (slot == 4u) out = b.x;
  else if (slot == 5u) out = b.y;
  else if (slot == 6u) out = c.x;
  else if (slot == 7u) out = c.y;
  else if (slot == 8u) out = d.x;
  else if (slot == 9u) out = d.y;
  B[gid] = out;
}
#endif
"""
    dst_src.write_text(
        f"#define {HELPER.feature_define_for_shape(SHAPE)} 1\n"
        "#define SBT_MMA_ENABLE_FP16_FP16_DUMP 1\n"
        f"#define DUMP_SLOT {slot}\n"
        f"{text}\n{dump_kernel}\n",
        encoding="utf-8",
    )
    return dst_src


def run_dump_kernel(exe: Path, env_sh: Path, src: Path, workdir: Path, seed_words: list[int], slot: int) -> list[int]:
    input_path = workdir / f"dump_in_slot{slot}.bin"
    output_path = workdir / f"dump_out_slot{slot}.bin"
    kernel_src = materialize_dump_source(src, workdir / f"dump_kernel_slot{slot}.cl", slot)
    write_u32_words(input_path, seed_words)
    cmd = (
        f"source {env_sh!s} >/dev/null 2>&1 && "
        f"VENTUS_BACKEND=spike {exe!s} --src {kernel_src!s} --kernel {DUMP_KERNEL!s} "
        f"--n {WARP_LANES} --in {input_path!s} --out {output_path!s}"
    )
    run_checked(cmd, workdir)
    return parse_u32_words(output_path)


def split_dump_words(words: list[int]) -> tuple[list[int], list[int], list[int], list[int]]:
    if len(words) != WARP_LANES:
        raise RuntimeError(f"unexpected dump size: {len(words)} expected={WARP_LANES}")
    return words[:], words[:], words[:], words[:]


def compare_raw_words(name: str, got: list[int], exp: list[int]) -> None:
    if len(got) != len(exp):
        raise RuntimeError(f"{name}: size mismatch got={len(got)} expected={len(exp)}")
    for idx, (g, e) in enumerate(zip(got, exp)):
        if g != e:
            raise RuntimeError(f"{name}: mismatch idx={idx} got=0x{g:08x} expected=0x{e:08x}")


def reconstruct_raw_windows(slot_outputs: list[list[int]]) -> tuple[list[int], list[int], list[int], list[int]]:
    if len(slot_outputs) != DUMP_SLOTS:
        raise RuntimeError(f"unexpected slot count: {len(slot_outputs)} expected={DUMP_SLOTS}")
    for slot_idx, words in enumerate(slot_outputs):
        if len(words) != WARP_LANES:
            raise RuntimeError(f"slot {slot_idx}: unexpected lane count {len(words)} expected={WARP_LANES}")

    a_words: list[int] = []
    for lane in range(WARP_LANES):
        for slot in range(4):
            a_words.append(slot_outputs[slot][lane])

    b_words: list[int] = []
    for lane in range(WARP_LANES):
        for slot in range(4, 6):
            b_words.append(slot_outputs[slot][lane])

    c_words: list[int] = []
    for lane in range(WARP_LANES):
        for slot in range(6, 8):
            c_words.append(slot_outputs[slot][lane])

    d_words: list[int] = []
    for lane in range(WARP_LANES):
        d_words.append(slot_outputs[8][lane] if (lane & 1) == 0 else slot_outputs[9][lane])

    return a_words, b_words, c_words, d_words


def main() -> int:
    args = parse_args()
    for path in (args.exe, args.src, args.env_sh, HELPER_PATH, DIRECT_PTX_PATH):
        require_path(path.resolve())

    tempdir = tempfile.TemporaryDirectory(prefix="fp16_mma_ventus_dump_")
    workdir = Path(tempdir.name)
    if args.keep_workdir:
        print(f"[INFO] workdir={workdir}")

    try:
        seed_words = HELPER.build_seed_words(WARP_LANES, args.seed)
        dump_slots: list[list[int]] = []
        for slot in range(DUMP_SLOTS):
            dump_slots.append(run_dump_kernel(args.exe.resolve(), args.env_sh.resolve(), args.src.resolve(), workdir, seed_words, slot))
        a_dump, b_dump, c_dump, d_dump = reconstruct_raw_windows(dump_slots)

        a_regs, b_regs, c_regs = HELPER.build_register_windows(seed_words, shape=SHAPE)
        helper_a = PTX_PROBE.flatten_lane_regs(a_regs)
        helper_b = PTX_PROBE.flatten_lane_regs(b_regs)
        helper_c = PTX_PROBE.flatten_lane_regs(c_regs)
        helper_d = HELPER.compute_cpu_reference(seed_words, shape=SHAPE)

        print(f"[INFO] seed=0x{args.seed:08x} dump_slots={len(dump_slots)}")
        compare_raw_words("ventus_a_vs_helper", a_dump, helper_a)
        compare_raw_words("ventus_b_vs_helper", b_dump, helper_b)
        compare_raw_words("ventus_c_vs_helper", c_dump, helper_c)
        compare_raw_words("ventus_d_vs_cpu_ref", d_dump, helper_d)
        print("[INFO] Ventus dump matches helper-generated ABI and CPU ref")

        actual_words = DIRECT_RAW_PROBE.run_probe(a_dump, b_dump, c_dump, args.arch, args.ptx_version, "ptx")
        nan_pairs, finite_pairs, finite_pass, max_abs_err, max_ulp_err, failures = PTX_PROBE.compare_words(
            actual_words, d_dump, args.ulp_tol
        )
        if failures:
            raise RuntimeError(
                f"direct-raw PTX vs Ventus dump mismatch_count={len(failures)} "
                f"first={failures[0]} max_abs={max_abs_err} max_ulp={max_ulp_err}"
            )
        print(
            f"PASS no-ldmatrix PTX matched Ventus dump "
            f"finite_pass={finite_pass}/{finite_pairs} max_abs={max_abs_err} max_ulp={max_ulp_err}"
        )
        return 0
    finally:
        if args.keep_workdir:
            print(f"[INFO] kept workdir={workdir}")
        else:
            tempdir.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
