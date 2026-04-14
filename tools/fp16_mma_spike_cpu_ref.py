#!/usr/bin/env python3
"""
背景
- `fp16 -> fp16` MMA 需要一个独立的 Spike-vs-CPU-reference 入口，用来交叉验证共享 MMA CPU reference。
- 当前仓库已经把 CPU reference 扩展成所有已支持 MMA family 共用的 helper；本脚本保留为 `fp16 -> fp16`
  的独立 wrapper，便于单独排查这两条 family。

需求/作用
- 运行 `m16n8k16 row.col f16->f16` 或 `m16n16k16 row.col f16->f16` 的 Spike-vs-CPU-reference 检查。
- 使用可复现随机 seed 生成有限值输入。
- 按 `NaN` 分类相等、有限值 `<= ULP` 的规则输出统计。

用法
- `python3 tools/fp16_mma_spike_cpu_ref.py`
- `python3 tools/fp16_mma_spike_cpu_ref.py --shape m16n16k16`
- `python3 tools/fp16_mma_spike_cpu_ref.py --seed 0x20260413 --ulp-tol 1 --keep-workdir`

实现原理/处理步骤
1) 生成 32 个随机 seed，并 materialize 只暴露目标 kernel 的单-kernel OpenCL 源文件。
2) 用 `VENTUS_BACKEND=spike` + `build/ventus_ocl_run` 执行目标 `fp16 -> fp16` kernel。
3) 调用共享 `mma_cpu_ref` helper 计算 CPU 参考输出。
4) 按 `fp16` ULP 规则比较 Spike 与 CPU reference，并打印统计。
"""

from __future__ import annotations

import argparse
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile

import mma_cpu_ref as mma_ref


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
DEFAULT_EXE = REPO_ROOT / "build/ventus_ocl_run"
DEFAULT_SRC = REPO_ROOT / "testcases/ocl_compare/custom_mma_kernels.cl"
DEFAULT_ENV_SH = (REPO_ROOT / ".." / "env.sh").resolve()
ALL_SHAPES = (mma_ref.SHAPE_M16N8K16, mma_ref.SHAPE_M16N16K16)


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC)
    ap.add_argument("--env-sh", type=Path, default=DEFAULT_ENV_SH)
    ap.add_argument("--shape", choices=ALL_SHAPES, default=mma_ref.SHAPE_M16N8K16)
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0x20260413)
    ap.add_argument("--n", type=int, default=mma_ref.WARP_LANES)
    ap.add_argument("--ulp-tol", type=int, default=1)
    ap.add_argument("--keep-workdir", action="store_true")
    return ap.parse_args()


def run_checked(cmd: str, cwd: Path) -> None:
    proc = subprocess.run(["bash", "-lc", cmd], cwd=str(cwd), text=True, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"command failed rc={proc.returncode}\ncmd: {cmd}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")


def materialize_kernel_source(base_src: Path, dst_src: Path, shape: str) -> Path:
    feature_define = mma_ref.feature_define_for_shape(shape)
    text = base_src.read_text(encoding="utf-8")
    dst_src.write_text(f"#define {feature_define} 1\n{text}", encoding="utf-8")
    return dst_src


def write_u32_words(path: Path, words: list[int]) -> None:
    path.write_bytes(struct.pack("<" + ("I" * len(words)), *words))


def parse_u32_words(path: Path) -> list[int]:
    blob = path.read_bytes()
    if len(blob) % 4 != 0:
        raise RuntimeError(f"invalid output size: {len(blob)}")
    return list(struct.unpack("<" + ("I" * (len(blob) // 4)), blob))


def run_spike(exe: Path, env_sh: Path, src: Path, workdir: Path, input_path: Path, output_path: Path, kernel_name: str) -> None:
    cmd = (
        f"source {shlex.quote(str(env_sh))} >/dev/null 2>&1 && "
        f"VENTUS_BACKEND=spike {shlex.quote(str(exe))} "
        f"--src {shlex.quote(str(src))} --kernel {shlex.quote(kernel_name)} "
        f"--n {mma_ref.WARP_LANES} --in {shlex.quote(str(input_path))} --out {shlex.quote(str(output_path))}"
    )
    run_checked(cmd, workdir)


def main() -> int:
    args = parse_args()
    if args.n != mma_ref.WARP_LANES:
        raise SystemExit(f"--n must be exactly {mma_ref.WARP_LANES} for this warp-scoped fp16 MMA test")

    exe = args.exe.resolve()
    src = args.src.resolve()
    env_sh = args.env_sh.resolve()
    for path in (exe, src, env_sh):
        if not path.exists():
            raise SystemExit(f"missing required path: {path}")

    kernel_name = mma_ref.kernel_name_for_shape(args.shape)
    seed_words = mma_ref.build_seed_words(args.n, args.seed)

    if args.keep_workdir:
        workdir = Path(tempfile.mkdtemp(prefix="fp16_mma_spike_cpu_ref_"))
        tempdir = None
    else:
        tempdir = tempfile.TemporaryDirectory(prefix="fp16_mma_spike_cpu_ref_")
        workdir = Path(tempdir.name)

    try:
        input_path = workdir / "in.bin"
        output_path = workdir / "out.bin"
        src_path = materialize_kernel_source(src, workdir / "kernel.cl", args.shape)
        write_u32_words(input_path, seed_words)
        run_spike(exe, env_sh, src_path, workdir, input_path, output_path, kernel_name)

        actual_words = parse_u32_words(output_path)
        expected_words = mma_ref.compute_cpu_reference(seed_words, kernel_name)
        stats = mma_ref.compare_outputs(
            actual_words,
            expected_words,
            kernel_name,
            fp16_ulp_tol=args.ulp_tol,
            f32_atol=1e-3,
            f32_rtol=1e-3,
        )

        print(f"[INFO] kernel={kernel_name} shape={args.shape} seed=0x{args.seed:08x} n={args.n}")
        print(f"[INFO] policy=NaN classify-equal; non-NaN <= {args.ulp_tol} fp16 ULP")
        print(f"[INFO] {mma_ref.format_compare_stats(stats)}")
        if stats.failures:
            print(f"[FAIL] mismatch_count={len(stats.failures)}")
            for item in stats.failures[:12]:
                print(f"  {item}")
            return 1
        print("PASS fp16 mma spike vs cpu ref")
        return 0
    finally:
        if tempdir is None:
            print(f"[INFO] kept workdir={workdir}")
        else:
            tempdir.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
