#!/usr/bin/env python3
"""
背景
- custom non-MMA 指令现在已经有 Spike 软件栈支持，可以直接用 Spike backend 作为语义 oracle，而不是继续停留在仓库内 reference model。

需求/作用
- 用 `ventus_ocl_run` 执行 `testcases/ocl_compare/custom_non_mma_kernels.cl` 中的 custom 微测例。
- 对每个 kernel 同时执行 Spike-vs-PTX 输出对照，以及 `sbt_decode --require-known` 与 `sbt_ptx + ptxas` compile-first 检查。
- 对整数 / packed 算术使用精确比较，对 SFU 浮点结果按容差比较。

用法
- `python3 tools/custom_non_mma_oracle.py`
- `python3 tools/custom_non_mma_oracle.py --n 128 --sm sm_89`

实现原理/处理步骤
1) 通过 `source ../env.sh` 分别设置 `VENTUS_BACKEND=spike` 与 `VENTUS_BACKEND=ptx` 运行同一组 custom kernel。
2) 基于该次编译产物 `object0.riscv` 执行 `sbt_decode --require-known` 与 `sbt_ptx/ptxas` compile-first。
3) 读取 Spike / PTX 输出并逐项比较；失败时显式报错并返回非 0。
"""

from __future__ import annotations

import argparse
import shlex
import struct
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
DEFAULT_EXE = REPO_ROOT / "build/ventus_ocl_run"
DEFAULT_SBT_DECODE = REPO_ROOT / "build/sbt_decode"
DEFAULT_SBT_PTX = REPO_ROOT / "build/sbt_ptx"
DEFAULT_SRC = REPO_ROOT / "testcases/ocl_compare/custom_non_mma_kernels.cl"
DEFAULT_ENV_SH = (REPO_ROOT / ".." / "env.sh").resolve()


def u32_to_f32(x: int) -> float:
    return struct.unpack("<f", struct.pack("<I", x & 0xFFFFFFFF))[0]


def fp16_bits_to_f32(bits: int) -> float:
    return float(struct.unpack("<e", struct.pack("<H", bits & 0xFFFF))[0])


def bf16_bits_to_f32(bits: int) -> float:
    return u32_to_f32((bits & 0xFFFF) << 16)


def pack_u16x2(lo: int, hi: int) -> int:
    return ((hi & 0xFFFF) << 16) | (lo & 0xFFFF)


def unpack_u16x2(v: int) -> tuple[int, int]:
    return v & 0xFFFF, (v >> 16) & 0xFFFF


def parse_u32_array(blob: bytes) -> list[int]:
    if len(blob) % 4 != 0:
        raise RuntimeError(f"invalid output byte size: {len(blob)}")
    return list(struct.unpack("<" + ("I" * (len(blob) // 4)), blob))


def f32_to_u32(x: float) -> int:
    return struct.unpack("<I", struct.pack("<f", x))[0]


def f32_to_fp16_bits(x: float) -> int:
    return struct.unpack("<H", struct.pack("<e", x))[0]


def f32_to_bf16_bits(x: float) -> int:
    return (f32_to_u32(x) >> 16) & 0xFFFF


def approx_equal(a: float, b: float, *, atol: float, rtol: float) -> bool:
    if a == b:
        return True
    if a != a and b != b:
        return True
    if a != a or b != b:
        return False
    diff = abs(a - b)
    limit = max(atol, rtol * max(abs(a), abs(b)))
    return diff <= limit


def run_checked(cmd: str, cwd: Path) -> subprocess.CompletedProcess[str]:
    p = subprocess.run(["bash", "-lc", cmd], cwd=str(cwd), text=True, capture_output=True)
    if p.returncode != 0:
        raise RuntimeError(f"command failed rc={p.returncode}\ncmd: {cmd}\nstdout:\n{p.stdout}\nstderr:\n{p.stderr}")
    return p


def normalize_sm(sm: str) -> int:
    s = sm.strip()
    if s.startswith("sm_"):
        s = s[3:]
    v = int(s)
    if v <= 0:
        raise ValueError(f"invalid sm: {sm}")
    return v


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


def compare_exact_u32(name: str, got: list[int], exp: list[int]) -> None:
    if len(got) != len(exp):
        raise RuntimeError(f"{name}: size mismatch got={len(got)} expected={len(exp)}")
    for i, (g, e) in enumerate(zip(got, exp)):
        if g != e:
            raise RuntimeError(f"{name}: mismatch i={i} got=0x{g:08x} expected=0x{e:08x}")


def compare_f32_bits_tol(name: str, got: list[int], exp: list[int], atol: float, rtol: float) -> None:
    if len(got) != len(exp):
        raise RuntimeError(f"{name}: size mismatch got={len(got)} expected={len(exp)}")
    for i, (g, e) in enumerate(zip(got, exp)):
        gf = u32_to_f32(g)
        ef = u32_to_f32(e)
        if not approx_equal(gf, ef, atol=atol, rtol=rtol):
            raise RuntimeError(f"{name}: mismatch i={i} got={gf} expected={ef} atol={atol} rtol={rtol}")


def compare_packed_tol(name: str, got: list[int], exp: list[int], *, kind: str, atol: float, rtol: float) -> None:
    if len(got) != len(exp):
        raise RuntimeError(f"{name}: size mismatch got={len(got)} expected={len(exp)}")
    for i, (g, e) in enumerate(zip(got, exp)):
        g0, g1 = unpack_u16x2(g)
        e0, e1 = unpack_u16x2(e)
        if kind == "f16":
            gf0, gf1 = fp16_bits_to_f32(g0), fp16_bits_to_f32(g1)
            ef0, ef1 = fp16_bits_to_f32(e0), fp16_bits_to_f32(e1)
        else:
            gf0, gf1 = bf16_bits_to_f32(g0), bf16_bits_to_f32(g1)
            ef0, ef1 = bf16_bits_to_f32(e0), bf16_bits_to_f32(e1)
        if not approx_equal(gf0, ef0, atol=atol, rtol=rtol):
            raise RuntimeError(f"{name}: lane0 mismatch i={i} got={gf0} expected={ef0} atol={atol} rtol={rtol}")
        if not approx_equal(gf1, ef1, atol=atol, rtol=rtol):
            raise RuntimeError(f"{name}: lane1 mismatch i={i} got={gf1} expected={ef1} atol={atol} rtol={rtol}")


def build_vrsqrt_input_words(kind: str, n: int) -> list[int]:
    if kind == "f16":
        samples = [
            pack_u16x2(0x0000, f32_to_fp16_bits(0.25)),
            pack_u16x2(f32_to_fp16_bits(0.75), 0x0000),
            pack_u16x2(f32_to_fp16_bits(-1.0), f32_to_fp16_bits(0.25)),
            pack_u16x2(f32_to_fp16_bits(1.0), f32_to_fp16_bits(-1.0)),
            pack_u16x2(0x7C00, f32_to_fp16_bits(0.25)),
            pack_u16x2(0xFC00, f32_to_fp16_bits(0.25)),
            pack_u16x2(0x7FFF, f32_to_fp16_bits(0.25)),
            pack_u16x2(f32_to_fp16_bits(1.0), f32_to_fp16_bits(4.0)),
        ]
    else:
        samples = [
            pack_u16x2(0x0000, f32_to_bf16_bits(0.25)),
            pack_u16x2(f32_to_bf16_bits(0.75), 0x0000),
            pack_u16x2(f32_to_bf16_bits(-1.0), f32_to_bf16_bits(0.25)),
            pack_u16x2(f32_to_bf16_bits(1.0), f32_to_bf16_bits(-1.0)),
            pack_u16x2(0x7F80, f32_to_bf16_bits(0.25)),
            pack_u16x2(0xFF80, f32_to_bf16_bits(0.25)),
            pack_u16x2(0x7FFF, f32_to_bf16_bits(0.25)),
            pack_u16x2(f32_to_bf16_bits(1.0), f32_to_bf16_bits(4.0)),
        ]
    out: list[int] = []
    for i in range(n):
        out.append(samples[i % len(samples)])
    return out


def write_u32_words(path: Path, words: list[int]) -> None:
    path.write_bytes(struct.pack("<" + ("I" * len(words)), *words))


def run_backend(
    *,
    backend: str,
    exe: Path,
    sbt_ptx: Path,
    src: Path,
    env_sh: Path,
    workdir: Path,
    kernel: str,
    n: int,
    out_path: Path,
    sm_num: int,
    in_path: Path | None,
) -> None:
    env_parts = [f"VENTUS_BACKEND={backend}"]
    if backend == "ptx":
        env_parts.append(f"GPU_SBT_PTX={shlex.quote(str(sbt_ptx))}")
        env_parts.append(f"VENTUS_PTX_SM={sm_num}")
    cmd = (
        f"source {shlex.quote(str(env_sh))} >/dev/null 2>&1 && "
        + " ".join(env_parts)
        + " "
        + f"{shlex.quote(str(exe))} --src {shlex.quote(str(src))} "
        + f"--kernel {shlex.quote(kernel)} --n {n} "
        + (f"--in {shlex.quote(str(in_path))} " if in_path is not None else "")
        + f"--out {shlex.quote(str(out_path))}"
    )
    run_checked(cmd, workdir)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    ap.add_argument("--sbt-decode", type=Path, default=DEFAULT_SBT_DECODE)
    ap.add_argument("--sbt-ptx", type=Path, default=DEFAULT_SBT_PTX)
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC)
    ap.add_argument("--env-sh", type=Path, default=DEFAULT_ENV_SH)
    ap.add_argument("--n", type=int, default=64)
    ap.add_argument("--sm", type=str, default="89")
    ap.add_argument("--ptxas", type=str, default="ptxas")
    ap.add_argument("--sfu-atol", type=float, default=3e-2)
    ap.add_argument("--sfu-rtol", type=float, default=5e-2)
    args = ap.parse_args()

    exe = args.exe.resolve()
    sbt_decode = args.sbt_decode.resolve()
    sbt_ptx = args.sbt_ptx.resolve()
    src = args.src.resolve()
    env_sh = args.env_sh.resolve()
    sm_num = normalize_sm(args.sm)

    for p in (exe, sbt_decode, sbt_ptx, src, env_sh):
        if not p.exists():
            raise SystemExit(f"missing required path: {p}")

    with tempfile.TemporaryDirectory(prefix="custom_non_mma_oracle_") as td:
        workdir = Path(td)
        for spec in KERNELS:
            out_spike = workdir / f"{spec.name}.spike.bin"
            out_ptx = workdir / f"{spec.name}.ptx.bin"
            ptx_path = workdir / f"{spec.name}.ptx"
            cubin_path = workdir / f"{spec.name}.cubin"
            input_path: Path | None = None

            if spec.name == "mt_custom_vrsqrt_f16x2":
                input_path = workdir / f"{spec.name}.in.bin"
                write_u32_words(input_path, build_vrsqrt_input_words("f16", args.n))
            elif spec.name == "mt_custom_vrsqrt_bf16x2":
                input_path = workdir / f"{spec.name}.in.bin"
                write_u32_words(input_path, build_vrsqrt_input_words("bf16", args.n))

            effective_n = max(args.n, WARP_LANES) if spec.name in SHUFFLE_KERNELS else args.n

            run_backend(
                backend="spike",
                exe=exe,
                sbt_ptx=sbt_ptx,
                src=src,
                env_sh=env_sh,
                workdir=workdir,
                kernel=spec.name,
                n=effective_n,
                out_path=out_spike,
                sm_num=sm_num,
                in_path=input_path,
            )

            elf = workdir / "object0.riscv"
            if not elf.exists():
                raise RuntimeError(f"{spec.name}: missing generated ELF at {elf}")

            decode_cmd = (
                f"{shlex.quote(str(sbt_decode))} decode {shlex.quote(str(elf))} "
                f"--func {shlex.quote(spec.name)} --require-known >/dev/null"
            )
            run_checked(decode_cmd, workdir)

            emit_cmd = (
                f"{shlex.quote(str(sbt_ptx))} {shlex.quote(str(elf))} "
                f"--func {shlex.quote(spec.name)} --require-known --sm {sm_num} --out {shlex.quote(str(ptx_path))}"
            )
            run_checked(emit_cmd, workdir)

            ptxas_cmd = (
                f"{shlex.quote(args.ptxas)} -arch=sm_{sm_num} "
                f"{shlex.quote(str(ptx_path))} -o {shlex.quote(str(cubin_path))}"
            )
            run_checked(ptxas_cmd, workdir)

            run_backend(
                backend="ptx",
                exe=exe,
                sbt_ptx=sbt_ptx,
                src=src,
                env_sh=env_sh,
                workdir=workdir,
                kernel=spec.name,
                n=effective_n,
                out_path=out_ptx,
                sm_num=sm_num,
                in_path=input_path,
            )

            exp = parse_u32_array(out_spike.read_bytes())
            got = parse_u32_array(out_ptx.read_bytes())
            if spec.mode == "exact_u32":
                compare_exact_u32(spec.name, got, exp)
            elif spec.mode == "f32_tol":
                compare_f32_bits_tol(spec.name, got, exp, atol=args.sfu_atol, rtol=args.sfu_rtol)
            elif spec.mode == "packed_f16_tol":
                compare_packed_tol(spec.name, got, exp, kind="f16", atol=args.sfu_atol, rtol=args.sfu_rtol)
            elif spec.mode == "packed_bf16_tol":
                compare_packed_tol(spec.name, got, exp, kind="bf16", atol=args.sfu_atol, rtol=args.sfu_rtol)
            else:
                raise RuntimeError(f"unknown compare mode: {spec.mode}")

            print(f"PASS kernel={spec.name} mode={spec.mode} sm=sm_{sm_num}")

    print("PASS custom non-MMA oracle gate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
