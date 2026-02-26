#!/usr/bin/env python3
import argparse
import json
import os
import subprocess
import sys
import time
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Optional


@dataclass(frozen=True)
class TestCase:
    name: str
    path: Path
    cmd: list[str]
    timeout_s: int = 120
    need_make: bool = True


def _ventus_testcases(ventus_root: Path) -> list[TestCase]:
    pocl_dir = ventus_root / "pocl"
    rodinia_dir = ventus_root / "rodinia"
    ventus_testcase_dir = ventus_root / "testcases"
    return [
        TestCase(name="matadd", path=pocl_dir / "build/examples/matadd", cmd=["./matadd"], need_make=False),
        TestCase(name="vecadd_4096", path=pocl_dir / "build/examples/vecadd", cmd=["./vecadd", "4096", "128"], need_make=False),
        TestCase(
            name="gaussian_16",
            path=rodinia_dir / "opencl/gaussian",
            cmd=["./gaussian.out", "-p", "0", "-d", "0", "-f", "../../data/gaussian/matrix16.txt", "-v"],
        ),
        TestCase(
            name="b+tree_128",
            path=rodinia_dir / "opencl/b+tree",
            cmd=[
                "./b+tree.out",
                "file",
                "../../data/b+tree/mil.txt",
                "command",
                "../../data/b+tree/command_128.txt",
                "--ref",
                "output_128.nvidia.txt",
            ],
        ),
        TestCase(name="backprop_1024", path=rodinia_dir / "opencl/backprop", cmd=["./backprop.out", "-n", "1024", "--ref", "nvidia-result-n1024"]),
        TestCase(name="bfs_4096", path=rodinia_dir / "opencl/bfs", cmd=["./bfs.out", "../../data/bfs/graph4096.txt"]),
        TestCase(
            name="nn_1024",
            path=rodinia_dir / "opencl/nn",
            cmd=[
                "./nn.out",
                "../../data/nn/list1k.txt",
                "-r",
                "20",
                "-lat",
                "13",
                "-lng",
                "27",
                "-f",
                "../../data/nn",
                "-t",
                "-p",
                "0",
                "-d",
                "0",
                "--ref",
                "nvidia-result-1k-lat13-lng27",
            ],
        ),
        TestCase(
            name="nn_64k",
            path=rodinia_dir / "opencl/nn",
            cmd=[
                "./nn.out",
                "../../data/nn/list64k.txt",
                "-r",
                "20",
                "-lat",
                "30",
                "-lng",
                "90",
                "-f",
                "../../data/nn",
                "-t",
                "-p",
                "0",
                "-d",
                "0",
                "--ref",
                "nvidia-result-64k-lat30-lng90",
            ],
        ),
        TestCase(
            name="kmeans_512",
            path=rodinia_dir / "opencl/kmeans",
            cmd=["./kmeans.out", "-o", "-r", "-i", "../../data/kmeans/512_34f.txt", "-g", "nvidia_result_512_34f_k5", "-p", "0", "-d", "0"],
        ),
        TestCase(name="mnist_conv_small", path=ventus_testcase_dir / "_get_case/MNIST_conv_small", cmd=["./conv.out"]),
        TestCase(name="mnist", path=ventus_testcase_dir / "_get_case/MNIST", cmd=["./nn_forward.out"]),
        TestCase(name="alexnet", path=ventus_testcase_dir / "_get_case/AlexNet", cmd=["./AlexNet.out"], timeout_s=240),
        TestCase(name="lenet5", path=ventus_testcase_dir / "_get_case/LeNet5", cmd=["./LeNet5.out"], timeout_s=240),
        TestCase(name="lenet5_simple", path=ventus_testcase_dir / "_get_case/LeNet5-simple", cmd=["./LeNet5-simple.out"], timeout_s=240),
        # TestCase(name="resnet18", path=ventus_testcase_dir / "_get_case/ResNet", cmd=["./ResNet18.out"], timeout_s=600),
    ]


def _fmt_ms(ms: float) -> str:
    if ms < 1000:
        return f"{ms:.1f}ms"
    return f"{ms/1000.0:.3f}s"


def _ensure_dir(p: Path) -> None:
    p.mkdir(parents=True, exist_ok=True)


def _ensure_ventus_env(env: dict[str, str], ventus_root: Path) -> dict[str, str]:
    """
    Make this script runnable without manually `source ventus-env/env.sh`.

    The Rodinia Makefiles and the runtime OpenCL platform discovery depend on:
      - VENTUS_INSTALL_PREFIX (for headers/libs in build)
      - PATH / LD_LIBRARY_PATH (for toolchain + runtime libs)
      - POCL_DEVICES / OCL_ICD_VENDORS / POCL_ENABLE_UNINIT (for PoCL Ventus device)
    """
    env = dict(env)
    install_prefix = env.get("VENTUS_INSTALL_PREFIX", "").strip()
    if not install_prefix:
        install_prefix = str((ventus_root / "install").resolve())
        env["VENTUS_INSTALL_PREFIX"] = install_prefix

    bin_dir = str(Path(install_prefix) / "bin")
    lib_dir = str(Path(install_prefix) / "lib")
    env["PATH"] = bin_dir + os.pathsep + env.get("PATH", "")
    env["LD_LIBRARY_PATH"] = lib_dir + os.pathsep + env.get("LD_LIBRARY_PATH", "")

    env.setdefault("POCL_DEVICES", "ventus")
    env.setdefault("OCL_ICD_VENDORS", str(Path(install_prefix) / "lib" / "libpocl.so"))
    env.setdefault("POCL_ENABLE_UNINIT", "1")

    # Select the PTX backend unless the user explicitly overrides.
    env.setdefault("VENTUS_BACKEND", "ptx")
    return env


def _run_logged(cmd: list[str], *, cwd: Path, timeout_s: float, log_path: Path, env: dict[str, str]) -> tuple[int, float]:
    started = time.perf_counter()
    with log_path.open("w", encoding="utf-8", errors="replace") as f:
        f.write(f"cwd: {cwd}\n")
        f.write("cmd: " + " ".join(cmd) + "\n")
        f.write(f"timeout_s: {timeout_s}\n")
        f.write("=== begin ===\n")
        f.flush()
        try:
            rc = subprocess.run(cmd, cwd=str(cwd), env=env, stdout=f, stderr=f, timeout=timeout_s).returncode
        except subprocess.TimeoutExpired:
            f.write("\n=== timeout ===\n")
            rc = 124
        except Exception as e:
            f.write(f"\n=== exception: {e} ===\n")
            rc = 125
    elapsed_s = time.perf_counter() - started
    return rc, elapsed_s


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Profile Ventus regression testcases with per-case wall-time breakdown.")
    ap.add_argument("--ventus-root", type=Path, default=Path("ventus-env"), help="Path to ventus-env root (default: ./ventus-env)")
    ap.add_argument("--timeout-scale", type=float, default=1.0, help="Scale each testcase timeout (default: 1.0)")
    ap.add_argument("--log-dir", type=Path, default=Path("build/ventus-regression-profile/logs"), help="Directory to store per-case logs")
    ap.add_argument("--json-out", type=Path, default=Path("build/ventus-regression-profile/summary.json"), help="Where to write JSON summary")
    ap.add_argument("--only", type=str, default="", help="Comma-separated testcase names to run (default: all)")
    ap.add_argument(
        "--sbt-ptx-profile-log",
        type=Path,
        default=Path("build/ventus-regression-profile/sbt_ptx_profile.jsonl"),
        help="If set, export GPU_SBT_PTX_PROFILE_LOG to collect sbt_ptx timings",
    )
    ap.add_argument("--clean", action="store_true", help="Delete existing logs/summary before run")
    args = ap.parse_args(argv)

    ventus_root = args.ventus_root.resolve()
    if not ventus_root.exists():
        print(f"error: ventus root not found: {ventus_root}", file=sys.stderr)
        return 2

    testcases = _ventus_testcases(ventus_root)
    only = [x.strip() for x in (args.only or "").split(",") if x.strip()]
    if only:
        wanted = set(only)
        testcases = [t for t in testcases if t.name in wanted]
        missing = sorted(wanted - {t.name for t in testcases})
        if missing:
            print(f"error: unknown testcase(s): {missing}", file=sys.stderr)
            return 2

    log_dir = args.log_dir
    if args.clean and log_dir.exists():
        for p in log_dir.glob("*.log"):
            p.unlink(missing_ok=True)
    _ensure_dir(log_dir)
    _ensure_dir(args.json_out.parent)

    sbt_profile_log = args.sbt_ptx_profile_log
    if args.clean:
        sbt_profile_log.unlink(missing_ok=True)

    env = dict(os.environ)
    if sbt_profile_log:
        env["GPU_SBT_PTX_PROFILE_LOG"] = str(sbt_profile_log.resolve())
    env = _ensure_ventus_env(env, ventus_root)

    compiled: set[Path] = set()
    rows: list[dict] = []

    for idx, tc in enumerate(testcases):
        if not tc.path.exists():
            print(f"[{idx+1}/{len(testcases)}] {tc.name}: skip (missing path: {tc.path})", file=sys.stderr)
            continue

        print(f"[{idx+1}/{len(testcases)}] {tc.name} ...", flush=True)
        compile_rc: Optional[int] = None
        compile_s = 0.0
        if tc.need_make and tc.path not in compiled:
            compile_log = log_dir / f"{tc.name}.compile.log"
            compile_rc, compile_s = _run_logged(
                ["make"],
                cwd=tc.path,
                timeout_s=60.0 * args.timeout_scale,
                log_path=compile_log,
                env=env,
            )
            if compile_rc == 0:
                compiled.add(tc.path)

        run_log = log_dir / f"{tc.name}.run.log"
        run_rc, run_s = _run_logged(
            tc.cmd,
            cwd=tc.path,
            timeout_s=float(tc.timeout_s) * args.timeout_scale,
            log_path=run_log,
            env=env,
        )

        row = {
            "name": tc.name,
            "path": str(tc.path),
            "cmd": tc.cmd,
            "need_make": tc.need_make,
            "compile_rc": compile_rc,
            "compile_s": compile_s,
            "run_rc": run_rc,
            "run_s": run_s,
            "total_s": compile_s + run_s,
            "compile_log": str((log_dir / f"{tc.name}.compile.log").resolve()),
            "run_log": str(run_log.resolve()),
        }
        rows.append(row)

    rows_sorted = sorted(rows, key=lambda r: r["total_s"], reverse=True)
    print("\nPer-test wall time (sorted):")
    for r in rows_sorted:
        comp = _fmt_ms(r["compile_s"] * 1000.0) if r["compile_rc"] is not None else "-"
        run = _fmt_ms(r["run_s"] * 1000.0)
        tot = _fmt_ms(r["total_s"] * 1000.0)
        ok = "OK" if r["run_rc"] == 0 and (r["compile_rc"] in (None, 0)) else f"rc={r['run_rc']}"
        print(f"  {r['name']:<16} total={tot:>9} run={run:>9} compile={comp:>9} {ok}")

    summary = {
        "ventus_root": str(ventus_root),
        "timeout_scale": args.timeout_scale,
        "sbt_ptx_profile_log": str(sbt_profile_log.resolve()) if sbt_profile_log else None,
        "testcases": rows,
    }
    args.json_out.write_text(json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")
    print(f"\nWrote JSON summary: {args.json_out}")
    if sbt_profile_log:
        print(f"sbt_ptx profile log (if enabled by sbt_ptx): {sbt_profile_log}")

    all_ok = all(r["run_rc"] == 0 and (r["compile_rc"] in (None, 0)) for r in rows)
    if not all_ok:
        print("\nerror: one or more testcases failed (see logs).", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
