#!/usr/bin/env python3
import argparse
import math
import subprocess
import struct
import tempfile
from pathlib import Path


def run_one(backend: str, exe: Path, src: Path, kernel: str, n: int, out: Path) -> str:
    cmd = (
        f"source ../env.sh >/dev/null 2>&1 && "
        f"VENTUS_BACKEND={backend} "
        f"{exe} --src {src} --kernel {kernel} --n {n} --out {out}"
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


def dump_decoded_json(sbt_decode: Path, elf: Path, func: str, out_json: Path) -> None:
    cmd = f"{sbt_decode} decode {elf} --func {func} --require-known --json {out_json} >/dev/null"
    p = subprocess.run(["bash", "-lc", cmd], text=True, capture_output=True)
    if p.returncode != 0:
        raise RuntimeError(f"sbt_decode failed rc={p.returncode}\nstdout:\n{p.stdout}\nstderr:\n{p.stderr}\n")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default="build/ventus_ocl_run", help="Path to ventus_ocl_run")
    ap.add_argument("--sbt-decode", default="build/sbt_decode", help="Path to sbt_decode")
    ap.add_argument("--src", default="testcases/ocl_compare/kernels.cl", help="Kernel source file")
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
        ],
    )
    ap.add_argument("--backend-a", default="spike")
    ap.add_argument("--backend-b", default="ptx")
    ap.add_argument("--coverage", action="store_true", help="Collect per-kernel decoded JSON and print coverage stats")
    ap.add_argument("--exceptions", default="data/inst_exceptions.txt", help="Mnemonic-level exception list for coverage gate")
    ap.add_argument("--min-covered", type=int, default=0, help="Coverage gate: minimum covered mnemonics in VentusInst_basic")
    ap.add_argument("--min-ratio", type=float, default=0.0, help="Coverage gate: minimum covered/total ratio")
    ap.add_argument("--require-full", action="store_true", help="Coverage gate: require full coverage except exceptions")
    ap.add_argument(
        "--float-kernels",
        nargs="*",
        default=[
            "mt_float_all",
            "mt_float_basic_cov",
            "mt_float_fma_cov",
            "mt_float_conv_cov",
        ],
        help="Kernels whose outputs are compared as float32 arrays (tolerance-based)",
    )
    ap.add_argument("--atol", type=float, default=1e-5, help="Float compare abs tolerance (default: 1e-5)")
    ap.add_argument("--rtol", type=float, default=1e-5, help="Float compare rel tolerance (default: 1e-5)")
    args = ap.parse_args()

    exe = Path(args.exe)
    sbt_decode = Path(args.sbt_decode)
    src = Path(args.src)
    if not exe.exists():
        raise SystemExit(f"missing exe: {exe}")
    if not sbt_decode.exists():
        raise SystemExit(f"missing sbt_decode: {sbt_decode}")
    if not src.exists():
        raise SystemExit(f"missing src: {src}")

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
            log_a = run_one(args.backend_a, exe, src, k, args.n, out_a)
            log_b = run_one(args.backend_b, exe, src, k, args.n, out_b)

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
                        dump_decoded_json(sbt_decode, elf, "_start", out_start)
                        decoded_jsons.append(out_start)
                        decoded_start = True
                    out_json = tdp / f"{k}.decoded.json"
                    dump_decoded_json(sbt_decode, elf, k, out_json)
                    decoded_jsons.append(out_json)

        if args.coverage and decoded_jsons:
            cov_cmd = [
                "python3",
                "tools/ventus_inst_coverage.py",
                "--exceptions",
                str(Path(args.exceptions)),
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
