#!/usr/bin/env python3
"""
背景
- 本仓库的 custom MMA / SFU / Shuffle 等回归依赖 ventus-env 中的设备端编译器和 Spike 运行语义。
- 这些能力在部分内部 ventus-env 版本中存在，但开源版 `../llvm` / `../spike` 可能尚未提供完整支持。

需求/作用
- 对回归所需的 Ventus custom feature 做显式能力检测。
- 同时检查设备端 clang 是否识别对应 builtin、Spike `encoding.h`/insn 文件是否声明执行入口，以及 MMA/SFU 所需依赖目录是否存在。
- 输出可读报告或 shell 变量，供回归脚本按 feature 显式 skip。

用法
- `python3 tools/ventus_feature_probe.py --summary`
- `python3 tools/ventus_feature_probe.py --feature mma --format text`
- `python3 tools/ventus_feature_probe.py --feature shuffle --format shell`

实现原理/处理步骤
1) 默认以本仓库上级目录作为 ventus-env root；oracle 会按传入的 `env.sh` 推导 root，并读取其导出的 `VENTUS_INSTALL_PREFIX`。
2) 对每个 feature 用最小 OpenCL 片段执行 `clang -fsyntax-only`，验证设备端编译器是否真实识别 builtin。
3) 解析 Spike 文本入口，验证 `DECLARE_INSN(...)`、`riscv/insns/*.h` 与必要 dependency 是否存在。
4) 任一必要证据缺失时返回 unavailable，并列出缺失原因；不做 mock 或静默降级。
"""

from __future__ import annotations

import argparse
import re
import shlex
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
DEFAULT_VENTUS_ROOT = REPO_ROOT.parent
DEFAULT_SPIKE_ROOT = DEFAULT_VENTUS_ROOT / "spike"
DEFAULT_CLANG = DEFAULT_VENTUS_ROOT / "install" / "bin" / "clang"

FEATURES = ("mma", "shuffle", "sfu", "vcvt", "packed")
NON_MMA_FEATURES = ("shuffle", "vcvt", "packed", "sfu")


@dataclass(frozen=True)
class FeatureSpec:
    name: str
    clang_probe: str
    spike_decls: tuple[str, ...]
    spike_insns: tuple[str, ...]
    spike_deps: tuple[str, ...] = ()


@dataclass(frozen=True)
class FeatureStatus:
    name: str
    available: bool
    reasons: tuple[str, ...]


FEATURE_SPECS = {
    "mma": FeatureSpec(
        name="mma",
        clang_probe="""
typedef unsigned int uint;
typedef uint uint2 __attribute__((ext_vector_type(2)));
typedef uint uint4 __attribute__((ext_vector_type(4)));
typedef float float4 __attribute__((ext_vector_type(4)));
__kernel void k(__global uint *out) {
  uint4 a = (uint4)(0, 0, 0, 0);
  uint2 b = (uint2)(0, 0);
  float4 c = (float4)(0.0f, 0.0f, 0.0f, 0.0f);
  float4 d = __builtin_riscv_ventus_mma_m16n8k16_row_col_f32_f16_f16_f32(a, b, c);
  out[0] = as_uint(d.x);
}
""",
        spike_decls=("mma_m16n8k16",),
        spike_insns=("mma_m16n8k16.h",),
        spike_deps=("dependencies/mma-sim",),
    ),
    "shuffle": FeatureSpec(
        name="shuffle",
        clang_probe="""
__kernel void k(__global unsigned int *out) {
  out[0] = __builtin_riscv_ventus_shuffle_idx_i32(out[0], 0);
}
""",
        spike_decls=("shuffle_idx",),
        spike_insns=("shuffle_idx.h",),
    ),
    "sfu": FeatureSpec(
        name="sfu",
        clang_probe="""
__kernel void k(__global unsigned int *out) {
  float y = __builtin_riscv_ventus_vex2_approx_f32(1.0f);
  out[0] = as_uint(y);
}
""",
        spike_decls=("vex2_approx_f32",),
        spike_insns=("vex2_approx_f32.h",),
        spike_deps=("dependencies/unfu",),
    ),
    "vcvt": FeatureSpec(
        name="vcvt",
        clang_probe="""
__kernel void k(__global unsigned int *out) {
  out[0] = __builtin_riscv_ventus_vcvt_fp32_fp16(out[0]);
}
""",
        spike_decls=("vcvt_fp32_fp16",),
        spike_insns=("vcvt_fp32_fp16.h",),
    ),
    "packed": FeatureSpec(
        name="packed",
        clang_probe="""
__kernel void k(__global unsigned int *out) {
  out[0] = __builtin_riscv_ventus_vadd_f16x2(out[0], out[0]);
}
""",
        spike_decls=("vadd_f16x2",),
        spike_insns=("vadd_f16x2.h",),
    ),
}


SFU_KERNEL_PREFIXES = (
    "mt_custom_vex2",
    "mt_custom_vlg2",
    "mt_custom_vrcp",
    "mt_custom_vsqrt",
    "mt_custom_vrsqrt",
    "mt_custom_vsin",
    "mt_custom_vcos",
    "mt_custom_vtanh",
    "mt_custom_vgelu",
    "mt_custom_vsilu",
)
PACKED_KERNEL_PREFIXES = (
    "mt_custom_vadd_f16x2",
    "mt_custom_vmul_f16x2",
    "mt_custom_vfma_f16x2",
    "mt_custom_vadd_bf16x2",
    "mt_custom_vmul_bf16x2",
    "mt_custom_vfma_bf16x2",
)
SHUFFLE_KERNELS = {
    "mt_custom_shuffle_idx",
    "mt_custom_shuffle_up",
    "mt_custom_shuffle_down",
    "mt_custom_shuffle_bfly",
}


def custom_non_mma_kernel_feature(name: str) -> str:
    if name in SHUFFLE_KERNELS:
        return "shuffle"
    if name.startswith("mt_custom_vcvt"):
        return "vcvt"
    if name.startswith(SFU_KERNEL_PREFIXES):
        return "sfu"
    if name.startswith(PACKED_KERNEL_PREFIXES):
        return "packed"
    raise RuntimeError(f"unknown custom non-MMA feature group for kernel: {name}")


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def _probe_clang(clang: Path, source: str) -> tuple[bool, str]:
    if not clang.exists():
        return False, f"missing device clang: {clang}"
    with tempfile.TemporaryDirectory(prefix="ventus_feature_probe_") as td:
        src = Path(td) / "probe.cl"
        src.write_text(source, encoding="utf-8")
        cmd = [
            str(clang),
            "-target",
            "riscv32",
            "-mcpu=ventus-gpgpu",
            "-x",
            "cl",
            "-cl-std=CL2.0",
            "-fsyntax-only",
            str(src),
        ]
        proc = subprocess.run(cmd, text=True, capture_output=True, check=False)
    if proc.returncode == 0:
        return True, ""
    detail = (proc.stderr or proc.stdout).strip().splitlines()
    first = detail[0] if detail else f"clang probe failed rc={proc.returncode}"
    return False, f"device clang builtin probe failed: {first}"


def _has_spike_decl(encoding_text: str, name: str) -> bool:
    pattern = rf"^\s*DECLARE_INSN\({re.escape(name)}\s*,"
    return re.search(pattern, encoding_text, flags=re.MULTILINE) is not None


def probe_feature(
    feature: str,
    *,
    spike_root: Path = DEFAULT_SPIKE_ROOT,
    clang: Path = DEFAULT_CLANG,
) -> FeatureStatus:
    spec = FEATURE_SPECS[feature]
    reasons: list[str] = []
    encoding_h = spike_root / "riscv" / "encoding.h"
    encoding_text = _read_text(encoding_h)
    if not encoding_text:
        reasons.append(f"missing Spike encoding.h: {encoding_h}")

    for decl in spec.spike_decls:
        if encoding_text and not _has_spike_decl(encoding_text, decl):
            reasons.append(f"Spike encoding.h lacks DECLARE_INSN({decl}, ...)")

    for insn in spec.spike_insns:
        path = spike_root / "riscv" / "insns" / insn
        if not path.exists():
            reasons.append(f"missing Spike insn file: {path}")

    for dep in spec.spike_deps:
        path = spike_root / dep
        if not path.exists():
            reasons.append(f"missing Spike dependency: {path}")

    clang_ok, clang_reason = _probe_clang(clang, spec.clang_probe)
    if not clang_ok:
        reasons.append(clang_reason)

    return FeatureStatus(name=feature, available=not reasons, reasons=tuple(reasons))


def probe_features(
    features: tuple[str, ...] | list[str],
    *,
    spike_root: Path = DEFAULT_SPIKE_ROOT,
    clang: Path = DEFAULT_CLANG,
) -> dict[str, FeatureStatus]:
    return {feature: probe_feature(feature, spike_root=spike_root, clang=clang) for feature in features}


def clang_for_env_sh(env_sh: Path) -> Path:
    cmd = f"source {shlex.quote(str(env_sh))} >/dev/null 2>&1 && printf '%s\\n' \"$VENTUS_INSTALL_PREFIX\""
    proc = subprocess.run(["bash", "-lc", cmd], text=True, capture_output=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"failed to source env.sh for feature probe: {env_sh}\nstderr:\n{proc.stderr}")
    install_prefix = proc.stdout.strip()
    if not install_prefix:
        raise RuntimeError(f"env.sh did not set VENTUS_INSTALL_PREFIX for feature probe: {env_sh}")
    return Path(install_prefix) / "bin" / "clang"


def probe_features_for_env_sh(features: tuple[str, ...] | list[str], env_sh: Path) -> dict[str, FeatureStatus]:
    ventus_root = env_sh.resolve().parent
    return probe_features(
        features,
        spike_root=ventus_root / "spike",
        clang=clang_for_env_sh(env_sh),
    )


def format_text(status: FeatureStatus) -> str:
    state = "available" if status.available else "unavailable"
    if status.available:
        return f"{status.name}: {state}"
    return f"{status.name}: {state}; " + "; ".join(status.reasons)


def format_shell(status: FeatureStatus) -> str:
    key = status.name.upper()
    value = "1" if status.available else "0"
    reason = " | ".join(status.reasons)
    return f"VENTUS_FEATURE_{key}={value}\nVENTUS_FEATURE_{key}_REASON={shlex.quote(reason)}"


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser()
    ap.add_argument("--feature", choices=FEATURES, action="append", help="Feature to probe; repeatable")
    ap.add_argument("--summary", action="store_true", help="Probe every known feature")
    ap.add_argument("--format", choices=("text", "shell"), default="text")
    ap.add_argument("--spike-root", type=Path, default=DEFAULT_SPIKE_ROOT)
    ap.add_argument("--clang", type=Path, default=DEFAULT_CLANG)
    return ap.parse_args()


def main() -> int:
    args = parse_args()
    selected = tuple(dict.fromkeys(args.feature or ()))
    if args.summary or not selected:
        selected = FEATURES
    statuses = probe_features(selected, spike_root=args.spike_root.resolve(), clang=args.clang.resolve())
    formatter = format_shell if args.format == "shell" else format_text
    for status in statuses.values():
        print(formatter(status))
    return 0 if all(status.available for status in statuses.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
