#!/usr/bin/env python3
"""
背景
- 需要在 bring-up 阶段对比同一份 OpenCL kernel 在 Spike backend 与 PTX backend 下的行为是否一致，并统计指令覆盖率。

需求/作用
- 通过 ventus_ocl_run 分别在 VENTUS_BACKEND=spike 与 VENTUS_BACKEND=ptx 下运行 kernel，比较输出 buffer。
- 可选导出 sbt_decode decoded JSON，并调用 ventus_inst_coverage.py 统计 VentusInst_basic.txt 的 mnemonic 覆盖率。

用法
- tools/ventus_ocl_compare.py --help
- tools/ventus_ocl_compare.py --kernels mt_int_all --n 256
- tools/ventus_ocl_compare.py --coverage

实现原理/处理步骤
1) `source ../env.sh` 后设置 `VENTUS_BACKEND=<spike|ptx>`，运行 ventus_ocl_run 生成输出文件。
2) 读取输出并按整数/浮点规则做对比（浮点按 atol/rtol 容差）。
3) 覆盖率模式下，对目标 ELF/函数调用 sbt_decode 导出 JSON，再由 ventus_inst_coverage.py 聚合统计。
"""

import argparse
import glob
import math
import os
import shlex
import subprocess
import struct
import tempfile
from pathlib import Path
from typing import Optional

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
DEFAULT_EXE = REPO_ROOT / "build/ventus_ocl_run"
DEFAULT_SBT_DECODE = REPO_ROOT / "build/sbt_decode"
DEFAULT_SBT_PTX = REPO_ROOT / "build/sbt_ptx"
DEFAULT_SRC = REPO_ROOT / "testcases/ocl_compare/kernels.cl"
DEFAULT_EXCEPTIONS = REPO_ROOT / "data/inst_exceptions.txt"
DEFAULT_COVERAGE_TARGET = REPO_ROOT / "VentusInst_basic.txt"
DEFAULT_ENV_SH = (REPO_ROOT / ".." / "env.sh").resolve()
DEFAULT_COVERAGE_TOOL = SCRIPT_DIR / "ventus_inst_coverage.py"
DEFAULT_COVERAGE_EXTRA_ELF_GLOBS = [
    str((REPO_ROOT / ".." / "rodinia" / "opencl" / "*" / "object0.riscv").resolve()),
    str((REPO_ROOT / ".." / "rodinia" / "opencl" / "*" / "object1.riscv").resolve()),
]


def default_gpu_sbt_ptx() -> Path | None:
    if "GPU_SBT_PTX" in os.environ:
        return None
    if DEFAULT_SBT_PTX.is_file():
        return DEFAULT_SBT_PTX.resolve()
    return None


def run_one(
    backend: str, exe: Path, src: Path, kernel: str, n: int, out: Path, env_sh: Path, gpu_sbt_ptx: Path | None
) -> str:
    env_parts = [f"VENTUS_BACKEND={shlex.quote(backend)}"]
    if backend == "ptx" and gpu_sbt_ptx is not None:
        env_parts.append(f"GPU_SBT_PTX={shlex.quote(str(gpu_sbt_ptx))}")
    cmd = (
        f"source {shlex.quote(str(env_sh))} >/dev/null 2>&1 && "
        + " ".join(env_parts)
        + " "
        f"{shlex.quote(str(exe))} --src {shlex.quote(str(src))} "
        f"--kernel {shlex.quote(kernel)} --n {n} --out {shlex.quote(str(out))}"
    )
    p = subprocess.run(["bash", "-lc", cmd], text=True, capture_output=True)
    if p.returncode != 0:
        raise RuntimeError(
            f"run failed backend={backend} kernel={kernel} rc={p.returncode}\n"
            f"stdout:\n{p.stdout}\n"
            f"stderr:\n{p.stderr}\n"
        )
    last = ""
    for line in p.stdout.splitlines():
        if line.startswith("bytes="):
            last = line.strip()
    return last or p.stdout.strip()


def _compare_f32_bytes(a: bytes, b: bytes, *, atol: float, rtol: float) -> tuple[bool, str]:
    if len(a) != len(b):
        return False, f"size_mismatch bytes_a={len(a)} bytes_b={len(b)}"
    if len(a) % 4 != 0:
        return False, f"invalid_f32_size bytes={len(a)}"
    n = len(a) // 4
    for i in range(n):
        xa = struct.unpack_from("<f", a, i * 4)[0]
        xb = struct.unpack_from("<f", b, i * 4)[0]
        if math.isnan(xa) and math.isnan(xb):
            continue
        if math.isinf(xa) or math.isinf(xb):
            if xa == xb:
                continue
            return False, f"f32_diff i={i} a={xa} b={xb} (inf)"
        if abs(xa - xb) <= (atol + rtol * abs(xa)):
            continue
        return False, f"f32_diff i={i} a={xa} b={xb} atol={atol} rtol={rtol}"
    return True, f"f32_ok n={n} atol={atol} rtol={rtol}"


def dump_decoded_json(
    sbt_decode: Path, elf: Path, out_json: Path, *, func: Optional[str] = None, require_known: bool = True
) -> None:
    cmd_parts = [f"{shlex.quote(str(sbt_decode))} decode {shlex.quote(str(elf))}"]
    if func:
        cmd_parts.append(f"--func {shlex.quote(func)}")
    if require_known:
        cmd_parts.append("--require-known")
    cmd_parts.append(f"--json {shlex.quote(str(out_json))} >/dev/null")
    cmd = " ".join(cmd_parts)
    p = subprocess.run(["bash", "-lc", cmd], text=True, capture_output=True)
    if p.returncode != 0:
        raise RuntimeError(f"sbt_decode failed rc={p.returncode}\nstdout:\n{p.stdout}\nstderr:\n{p.stderr}\n")


def resolve_extra_coverage_elfs(glob_patterns: list[str]) -> list[Path]:
    out: list[Path] = []
    seen: set[Path] = set()
    for pattern in glob_patterns:
        for m in sorted(glob.glob(pattern)):
            p = Path(m).resolve()
            if not p.is_file() or p in seen:
                continue
            seen.add(p)
            out.append(p)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE, help="Path to ventus_ocl_run")
    ap.add_argument("--sbt-decode", type=Path, default=DEFAULT_SBT_DECODE, help="Path to sbt_decode")
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC, help="Kernel source file")
    ap.add_argument("--n", type=int, default=256)
    ap.add_argument(
        "--kernels",
        nargs="*",
        default=[
            "mt_int_all",
            "mt_int_cov",
            "mt_csr_cov",
            "mt_mext_cov",
            "mt_rv32i_alu_cov",
            "mt_rv32i_mem_cov",
            "mt_rv32i_branch_cov",
            "mt_vbranch_lt",
            "mt_float_all",
            "mt_float_basic_cov",
            "mt_float_fma_cov",
            "mt_float_conv_cov",
            "mt_float_cmp_misc_cov",
            "mt_scalar_f_arith_fma_cov",
            "mt_scalar_f_cvt_float_cov",
            "mt_scalar_f_cvt_int_cov",
            "mt_scalar_f_cmp_class_cov",
            "mt_scalar_f_mem_move_cov",
        ],
    )
    ap.add_argument("--backend-a", default="spike")
    ap.add_argument("--backend-b", default="ptx")
    ap.add_argument("--coverage", action="store_true", help="Collect per-kernel decoded JSON and print coverage stats")
    ap.add_argument("--exceptions", type=Path, default=DEFAULT_EXCEPTIONS, help="Mnemonic-level exception list for coverage gate")
    ap.add_argument("--min-covered", type=int, default=0, help="Coverage gate: minimum covered mnemonics in VentusInst_basic")
    ap.add_argument("--min-ratio", type=float, default=0.0, help="Coverage gate: minimum covered/total ratio")
    ap.add_argument("--require-full", action="store_true", help="Coverage gate: require full coverage except exceptions")
    ap.add_argument(
        "--coverage-extra-elf-glob",
        nargs="*",
        default=DEFAULT_COVERAGE_EXTRA_ELF_GLOBS,
        help="Extra ELF glob(s) decoded for coverage only (decoded without --require-known)",
    )
    ap.add_argument(
        "--float-kernels",
        nargs="*",
        default=[
            "mt_float_all",
            "mt_float_basic_cov",
            "mt_float_fma_cov",
            "mt_float_conv_cov",
            "mt_scalar_f_arith_fma_cov",
        ],
        help="Kernels whose outputs are compared as float32 arrays (tolerance-based)",
    )
    ap.add_argument("--atol", type=float, default=1e-5, help="Float compare abs tolerance (default: 1e-5)")
    ap.add_argument("--rtol", type=float, default=1e-5, help="Float compare rel tolerance (default: 1e-5)")
    args = ap.parse_args()

    exe = args.exe.resolve()
    sbt_decode = args.sbt_decode.resolve()
    src = args.src.resolve()
    exceptions = args.exceptions.resolve()
    coverage_target = DEFAULT_COVERAGE_TARGET.resolve()
    env_sh = DEFAULT_ENV_SH
    coverage_tool = DEFAULT_COVERAGE_TOOL.resolve()
    if not exe.exists():
        raise SystemExit(f"missing exe: {exe}")
    if not sbt_decode.exists():
        raise SystemExit(f"missing sbt_decode: {sbt_decode}")
    if not src.exists():
        raise SystemExit(f"missing src: {src}")
    if not env_sh.exists():
        raise SystemExit(f"missing env.sh: {env_sh}")
    if args.coverage and not coverage_target.exists():
        raise SystemExit(f"missing coverage target: {coverage_target}")
    if args.coverage and not coverage_tool.exists():
        raise SystemExit(f"missing coverage tool: {coverage_tool}")

    gpu_sbt_ptx = default_gpu_sbt_ptx()
    if gpu_sbt_ptx is not None:
        print(f"[INFO] GPU_SBT_PTX={gpu_sbt_ptx}")

    # The Ventus PoCL device may choose to emit a side-effect ELF (object0.riscv) into the CWD.
    # If it already exists, it can become stale and break coverage accounting across edits.
    if args.coverage:
        for p in (Path("object0.riscv"), Path("object0.cl")):
            try:
                p.unlink()
            except FileNotFoundError:
                pass

    ok = True
    with tempfile.TemporaryDirectory(prefix="ventus_ocl_compare_") as td:
        tdp = Path(td)
        decoded_jsons: list[Path] = []
        decoded_start = False

        for k in args.kernels:
            out_a = tdp / f"{k}.{args.backend_a}.bin"
            out_b = tdp / f"{k}.{args.backend_b}.bin"
            log_a = run_one(args.backend_a, exe, src, k, args.n, out_a, env_sh, gpu_sbt_ptx)
            log_b = run_one(args.backend_b, exe, src, k, args.n, out_b, env_sh, gpu_sbt_ptx)

            ba = out_a.read_bytes()
            bb = out_b.read_bytes()
            if k in set(args.float_kernels):
                same, detail = _compare_f32_bytes(ba, bb, atol=args.atol, rtol=args.rtol)
                if not same:
                    ok = False
                    print(f"FAIL kernel={k} {detail}")
                else:
                    print(f"PASS kernel={k} {detail} {log_a} {log_b}")
            else:
                if ba != bb:
                    ok = False
                    off = next((i for i, (x, y) in enumerate(zip(ba, bb)) if x != y), None)
                    if off is None and len(ba) != len(bb):
                        off = min(len(ba), len(bb))
                    print(f"FAIL kernel={k} bytes_a={len(ba)} bytes_b={len(bb)}")
                    if off is not None:
                        xa = ba[off] if off < len(ba) else None
                        xb = bb[off] if off < len(bb) else None
                        print(f"  first_diff_off={off} a={xa} b={xb}")
                else:
                    print(f"PASS kernel={k} {log_a} {log_b}")

            if args.coverage:
                elf = Path("object0.riscv")
                if elf.exists():
                    if not decoded_start:
                        out_start = tdp / "_start.decoded.json"
                        dump_decoded_json(sbt_decode, elf, out_start, func="_start", require_known=True)
                        decoded_jsons.append(out_start)
                        decoded_start = True
                    out_json = tdp / f"{k}.decoded.json"
                    dump_decoded_json(sbt_decode, elf, out_json, func=k, require_known=True)
                    decoded_jsons.append(out_json)

        if args.coverage:
            extra_elfs = resolve_extra_coverage_elfs(args.coverage_extra_elf_glob)
            extra_decoded = 0
            for i, extra_elf in enumerate(extra_elfs):
                out_json = tdp / f"_extra_{i}.decoded.json"
                try:
                    dump_decoded_json(sbt_decode, extra_elf, out_json, func=None, require_known=False)
                except RuntimeError as err:
                    first = str(err).splitlines()[0] if str(err) else "decode failed"
                    print(f"WARN coverage extra decode skipped elf={extra_elf}: {first}")
                    continue
                decoded_jsons.append(out_json)
                extra_decoded += 1
            print(f"INFO coverage extras: globs={len(args.coverage_extra_elf_glob)} found={len(extra_elfs)} decoded={extra_decoded}")

        if args.coverage and decoded_jsons:
            cov_cmd = [
                "python3",
                str(coverage_tool),
                "--target",
                str(coverage_target),
                "--exceptions",
                str(exceptions),
                "--decoded-json",
                *[str(p) for p in decoded_jsons],
                "--min-covered",
                str(args.min_covered),
                "--min-ratio",
                str(args.min_ratio),
            ]
            if args.require_full:
                cov_cmd.append("--require-full")
            p = subprocess.run(cov_cmd, text=True, capture_output=True)
            if p.stdout.strip():
                print(p.stdout.strip())
            if p.returncode != 0:
                if p.stderr.strip():
                    print(p.stderr.strip())
                ok = False

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
