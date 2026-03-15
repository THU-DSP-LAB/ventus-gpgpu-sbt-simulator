#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import html
import math
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

DEFAULT_ARCH = "sm_75"
PTX_VERSION = "7.8"
ADDRESS_SIZE = 64
STATE_WORDS_DEFAULT = (32, 128, 256, 512, 600)
HELPER_OPS_DEFAULT = (4, 32)
ABI_MODES = ("vctx", "value_params", "value_blob")
DEFAULT_USE_MODES = ("pct25", "pct50", "pct75", "full")
LEGACY_USE_MODES = ("hot4", "hot4_dead")
USE_MODES = DEFAULT_USE_MODES + LEGACY_USE_MODES
HOT_WORDS = 4
CALL_WINDOW_RADIUS = 12
PTXAS = "ptxas"
CUOBJDUMP = "cuobjdump"
MODE_RATIOS = {"pct25": 0.25, "pct50": 0.5, "pct75": 0.75, "full": 1.0}
MODE_LABELS = {
    "pct25": "25%",
    "pct50": "50%",
    "pct75": "75%",
    "full": "100%",
    "hot4": "hot4",
    "hot4_dead": "hot4_dead",
}
MODE_ORDER = {name: index for index, name in enumerate(USE_MODES)}
ABI_COLORS = {"vctx": "#c0392b", "value_params": "#2980b9", "value_blob": "#16a085"}
CHART_SPECS = (
    ("registers", "Registers", "registers"),
    ("ldl_stl_total", "LDL+STL Instructions", "count"),
    ("call_window_ldl_stl", "CALL Window LDL/STL", "count"),
    ("local_footprint_bytes", "Local Footprint", "bytes"),
    ("spill_total_bytes", "Spill Traffic", "bytes"),
)
CHART_EXPLANATIONS = {
    "registers": "寄存器使用量。越高表示寄存器压力越大，更容易逼近寄存器上限并触发后续 local/spill 问题。",
    "ldl_stl_total": "整份 SASS 中 `LDL/STL` 指令总条数，用来观察整体 local memory 访存流量。",
    "call_window_ldl_stl": "每个 `CALL` 邻域窗口中的 `LDL/STL` 总条数，用来观察 direct-call 边界附近是否存在明显 marshalling。",
    "local_footprint_bytes": "每线程 local 空间占用，定义为 `lmem_bytes + stack_frame`。越高表示 local 容量压力越大。",
    "spill_total_bytes": "总 spill 字节数，定义为 `spill_stores_total_bytes + spill_loads_total_bytes`。越高表示寄存器压力已经转化为真实 spill 成本。",
}
SVG_WIDTH = 1600
SVG_HEIGHT = 860
SVG_MARGIN_LEFT = 70
SVG_MARGIN_RIGHT = 32
SVG_MARGIN_TOP = 110
SVG_MARGIN_BOTTOM = 56
SVG_PANEL_GAP_X = 28
SVG_PANEL_GAP_Y = 48
SVG_POINT_RADIUS = 4
SVG_STROKE_WIDTH = 2.5
SASS_MNEMONIC_RE = re.compile(r"/\*[0-9a-fA-F]+\*/\s+([A-Z][A-Z0-9_.]*)")
PTXAS_STACK_RE = re.compile(
    r"([0-9]+) bytes stack frame, ([0-9]+) bytes spill stores, ([0-9]+) bytes spill loads"
)
PTXAS_REG_RE = re.compile(r"Used ([0-9]+) registers")
PTXAS_LMEM_RE = re.compile(
    r"Used [0-9]+ registers, used [0-9]+ barriers, [0-9]+ bytes cmem\[0\](?:, ([0-9]+) bytes lmem)?"
)


@dataclass(frozen=True)
class BenchCase:
    abi: str
    state_words: int
    use_mode: str
    helper_ops: int

    @property
    def name(self) -> str:
        return f"{self.abi}_s{self.state_words}_{self.use_mode}_o{self.helper_ops}"

    @property
    def state_bytes(self) -> int:
        return self.state_words * 4

    @property
    def used_words(self) -> int:
        return used_words_for_mode(self.use_mode, self.state_words)

    @property
    def checksum_words(self) -> int:
        return checksum_words_for_mode(self.use_mode, self.state_words)

    @property
    def use_ratio_percent(self) -> int:
        return ratio_percent_for_mode(self.use_mode, self.state_words)


@dataclass(frozen=True)
class CaseResult:
    case: BenchCase
    ptx_bytes: int
    ptx_lines: int
    ptx_param_defs: int
    ptx_ld_param: int
    ptx_st_param: int
    ptx_ld_local: int
    ptx_st_local: int
    registers: int
    lmem_bytes: int
    stack_frame: int
    spill_stores: int
    spill_loads: int
    call_count: int
    pret_count: int
    ldl_count: int
    stl_count: int
    call_window_ldl_stl: int
    sass_path: Path
    ptx_path: Path

    @property
    def ldl_stl_total(self) -> int:
        return self.ldl_count + self.stl_count

    @property
    def spill_total_bytes(self) -> int:
        return self.spill_stores + self.spill_loads

    @property
    def local_footprint_bytes(self) -> int:
        return self.lmem_bytes + self.stack_frame


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run direct-call ABI PTX micro-benchmarks for vctx vs value ABI."
    )
    parser.add_argument("--arch", default=DEFAULT_ARCH, help="PTX target arch passed to ptxas.")
    parser.add_argument(
        "--state-words",
        nargs="+",
        type=int,
        default=list(STATE_WORDS_DEFAULT),
        help="State vector sizes in 32-bit words.",
    )
    parser.add_argument(
        "--helper-ops",
        nargs="+",
        type=int,
        default=list(HELPER_OPS_DEFAULT),
        help="Operation counts per helper body.",
    )
    parser.add_argument(
        "--abis",
        nargs="+",
        choices=ABI_MODES,
        default=list(ABI_MODES),
        help="ABI variants to generate.",
    )
    parser.add_argument(
        "--modes",
        nargs="+",
        choices=USE_MODES,
        default=list(DEFAULT_USE_MODES),
        help="Usage patterns for the callee.",
    )
    parser.add_argument(
        "--match",
        default="",
        help="Only run cases whose generated name contains this substring.",
    )
    parser.add_argument(
        "--out-dir",
        default="build",
        help="Output directory for generated PTX, cubin, SASS and reports.",
    )
    parser.add_argument(
        "--ptxas-opt-level",
        default="-O3",
        choices=("-O0", "-O1", "-O2", "-O3"),
        help="Optimization level passed through to ptxas.",
    )
    return parser.parse_args()


def require_tool(name: str) -> None:
    if shutil.which(name) is None:
        raise SystemExit(f"missing required tool: {name}")


def used_words_for_mode(mode: str, state_words: int) -> int:
    if mode in ("hot4", "hot4_dead"):
        return min(HOT_WORDS, state_words)
    ratio = MODE_RATIOS.get(mode)
    if ratio is None:
        raise ValueError(f"unsupported mode: {mode}")
    return max(1, math.ceil(state_words * ratio))


def checksum_words_for_mode(mode: str, state_words: int) -> int:
    return min(HOT_WORDS, state_words) if mode == "hot4_dead" else state_words


def ratio_percent_for_mode(mode: str, state_words: int) -> int:
    if mode in MODE_RATIOS:
        return int(MODE_RATIOS[mode] * 100)
    return math.ceil(used_words_for_mode(mode, state_words) * 100 / state_words)


def mode_label(mode: str) -> str:
    return MODE_LABELS.get(mode, mode)


def result_sort_key(result: CaseResult) -> tuple[int, int, int, int]:
    abi_index = ABI_MODES.index(result.case.abi)
    return (result.case.helper_ops, MODE_ORDER[result.case.use_mode], result.case.state_words, abi_index)


def build_cases(args: argparse.Namespace) -> list[BenchCase]:
    cases: list[BenchCase] = []
    for abi in args.abis:
        for state_words in args.state_words:
            for use_mode in args.modes:
                for helper_ops in args.helper_ops:
                    case = BenchCase(abi=abi, state_words=state_words, use_mode=use_mode, helper_ops=helper_ops)
                    if args.match and args.match not in case.name:
                        continue
                    cases.append(case)
    if not cases:
        raise SystemExit("no cases selected")
    return cases


def ensure_dirs(out_dir: Path) -> dict[str, Path]:
    paths = {
        "root": out_dir,
        "ptx": out_dir / "generated_ptx",
        "cubin": out_dir / "cubin",
        "sass": out_dir / "sass",
        "call_windows": out_dir / "call_windows",
        "charts": out_dir / "charts",
    }
    for path in paths.values():
        path.mkdir(parents=True, exist_ok=True)
    return paths


def render_ptx(case: BenchCase, arch: str) -> str:
    header = [
        f".version {PTX_VERSION}",
        f".target {arch}",
        f".address_size {ADDRESS_SIZE}",
        "",
    ]
    if case.abi == "vctx":
        return "\n".join(header + render_vctx_case(case))
    if case.abi == "value_params":
        return "\n".join(header + render_value_params_case(case))
    if case.abi == "value_blob":
        return "\n".join(header + render_value_blob_case(case))
    raise ValueError(f"unsupported abi: {case.abi}")


def render_vctx_case(case: BenchCase) -> list[str]:
    lines = [
        ".func helper_vctx(",
        "    .param .u64 in_vctx",
        ");",
        "",
        ".visible .entry kernel_vctx(",
        "    .param .u64 out_ptr,",
        "    .param .u32 seed",
        ")",
        "{",
    ]
    lines.extend(render_common_regs(case))
    lines.append(f"    .local .align 4 .b8 vctx[{case.state_bytes}];")
    lines.extend(common_entry_preamble())
    lines.append("    mov.u64 %rd3, vctx;")
    lines.extend(init_state_regs(case))
    lines.extend(store_state_to_local("%rd3", case))
    lines.append("    .param .u64 arg_vctx;")
    lines.append("    st.param.u64 [arg_vctx], %rd3;")
    lines.append("    call.uni helper_vctx, (arg_vctx);")
    lines.extend(load_state_from_local("%rd3", case))
    lines.extend(final_store_sequence(case))
    lines.append("}")
    lines.append("")
    lines.extend(render_vctx_helper(case))
    return lines


def render_vctx_helper(case: BenchCase) -> list[str]:
    lines = [
        ".func helper_vctx(",
        "    .param .u64 in_vctx",
        ")",
        "{",
    ]
    lines.extend(render_common_regs(case))
    lines.append("    ld.param.u64 %rd3, [in_vctx];")
    lines.extend(load_state_from_local("%rd3", case))
    lines.extend(apply_helper_ops(case))
    lines.extend(store_state_to_local("%rd3", case))
    lines.append("    ret;")
    lines.append("}")
    return lines


def render_value_params_case(case: BenchCase) -> list[str]:
    proto = render_value_params_signature(case, with_semicolon=True)
    lines = proto + ["", ".visible .entry kernel_value_params(", "    .param .u64 out_ptr,", "    .param .u32 seed", ")", "{"]
    lines.extend(render_common_regs(case))
    lines.extend(common_entry_preamble())
    lines.extend(init_state_regs(case))
    lines.extend(declare_scalar_param_block("arg_state", case.state_words))
    lines.extend(store_state_to_scalar_params("arg_state", case))
    lines.extend(render_scalar_call(case))
    lines.extend(final_store_sequence(case))
    lines.append("}")
    lines.append("")
    lines.extend(render_value_params_signature(case, with_semicolon=False))
    lines.append("{")
    lines.extend(render_common_regs(case))
    lines.extend(load_state_from_scalar_params("in_state", case))
    lines.extend(apply_helper_ops(case))
    lines.extend(move_state_to_return_regs(case))
    lines.append("    ret;")
    lines.append("}")
    return lines


def render_value_blob_case(case: BenchCase) -> list[str]:
    blob_decl = f"    .param .align 4 .b8 in_blob[{case.state_bytes}]"
    blob_ret = f"    .param .align 4 .b8 out_blob[{case.state_bytes}]"
    lines = [
        ".func (",
        blob_ret,
        ") helper_value_blob(",
        blob_decl,
        ");",
        "",
        ".visible .entry kernel_value_blob(",
        "    .param .u64 out_ptr,",
        "    .param .u32 seed",
        ")",
        "{",
    ]
    lines.extend(render_common_regs(case))
    lines.extend(common_entry_preamble())
    lines.extend(init_state_regs(case))
    lines.append(f"    .param .align 4 .b8 arg_blob[{case.state_bytes}];")
    lines.append(f"    .param .align 4 .b8 ret_blob[{case.state_bytes}];")
    lines.extend(store_state_to_blob_params("arg_blob", case))
    lines.append("    call.uni (ret_blob), helper_value_blob, (arg_blob);")
    lines.extend(load_state_from_blob_params("ret_blob", case))
    lines.extend(final_store_sequence(case))
    lines.append("}")
    lines.append("")
    lines.extend([".func (", blob_ret, ") helper_value_blob(", blob_decl, ")", "{"])
    lines.extend(render_common_regs(case))
    lines.extend(load_state_from_blob_params("in_blob", case))
    lines.extend(apply_helper_ops(case))
    lines.extend(store_state_to_blob_params("out_blob", case))
    lines.append("    ret;")
    lines.append("}")
    return lines


def render_value_params_signature(case: BenchCase, with_semicolon: bool) -> list[str]:
    lines = [".func ("]
    lines.extend(join_return_reg_lines("out_state", case.state_words))
    lines.append(") helper_value_params(")
    lines.extend(join_param_lines("in_state", case.state_words, trailing_comma=False))
    lines.append(")")
    if with_semicolon:
        lines[-1] += ";"
    return lines


def join_return_reg_lines(prefix: str, count: int) -> list[str]:
    return [f"    .reg .b32 {prefix}_{index}{',' if index != count - 1 else ''}" for index in range(count)]


def join_param_lines(prefix: str, count: int, trailing_comma: bool) -> list[str]:
    lines: list[str] = []
    for index in range(count):
        suffix = "," if index != count - 1 or trailing_comma else ""
        lines.append(f"    .param .b32 {prefix}_{index}{suffix}")
    return lines


def render_common_regs(case: BenchCase) -> list[str]:
    return [f"    .reg .b32 %r<{case.state_words + 32}>;", "    .reg .b64 %rd<8>;"]


def common_entry_preamble() -> list[str]:
    return ["    mov.u32 %r0, %tid.x;", "    ld.param.u32 %r1, [seed];"]


def init_state_regs(case: BenchCase) -> list[str]:
    lines: list[str] = []
    for index in range(case.state_words):
        reg = state_reg(index)
        bias = 17 * (index + 1)
        lines.append(f"    add.u32 {reg}, %r0, {bias};")
        lines.append(f"    add.u32 {reg}, {reg}, %r1;")
    return lines


def apply_helper_ops(case: BenchCase) -> list[str]:
    lines: list[str] = []
    for index in range(case.used_words):
        reg = state_reg(index)
        for op_index in range(case.helper_ops):
            lines.append("    " + helper_op(reg, index, op_index))
    return lines


def helper_op(reg: str, state_index: int, op_index: int) -> str:
    add_imm = 3 + ((state_index + op_index) % 29)
    xor_imm = ((state_index + 1) * 0x45D9F3B) ^ (op_index * 0x9E3779B1)
    mad_add = 7 + ((state_index * 3 + op_index) % 19)
    if op_index % 3 == 0:
        return f"add.u32 {reg}, {reg}, {add_imm};"
    if op_index % 3 == 1:
        return f"xor.b32 {reg}, {reg}, 0x{xor_imm & 0xFFFFFFFF:08x};"
    return f"mad.lo.u32 {reg}, {reg}, 3, {mad_add};"


def store_state_to_local(base: str, case: BenchCase) -> list[str]:
    return [f"    st.local.u32 [{base}+{index * 4}], {state_reg(index)};" for index in range(case.state_words)]


def load_state_from_local(base: str, case: BenchCase) -> list[str]:
    return [f"    ld.local.u32 {state_reg(index)}, [{base}+{index * 4}];" for index in range(case.state_words)]


def declare_scalar_param_block(prefix: str, count: int) -> list[str]:
    return [f"    .param .b32 {prefix}_{index};" for index in range(count)]


def store_state_to_scalar_params(prefix: str, case: BenchCase) -> list[str]:
    return [f"    st.param.b32 [{prefix}_{index}], {state_reg(index)};" for index in range(case.state_words)]


def load_state_from_scalar_params(prefix: str, case: BenchCase) -> list[str]:
    return [f"    ld.param.b32 {state_reg(index)}, [{prefix}_{index}];" for index in range(case.state_words)]


def move_state_to_return_regs(case: BenchCase) -> list[str]:
    return [f"    mov.u32 out_state_{index}, {state_reg(index)};" for index in range(case.state_words)]


def store_state_to_blob_params(prefix: str, case: BenchCase) -> list[str]:
    return [f"    st.param.b32 [{prefix}+{index * 4}], {state_reg(index)};" for index in range(case.state_words)]


def load_state_from_blob_params(prefix: str, case: BenchCase) -> list[str]:
    return [f"    ld.param.b32 {state_reg(index)}, [{prefix}+{index * 4}];" for index in range(case.state_words)]


def render_scalar_call(case: BenchCase) -> list[str]:
    ret_lines = ", ".join(state_reg(index) for index in range(case.state_words))
    arg_lines = ", ".join(f"arg_state_{index}" for index in range(case.state_words))
    return ["    call.uni (", f"        {ret_lines}", "    ), helper_value_params, (", f"        {arg_lines}", "    );"]


def final_store_sequence(case: BenchCase) -> list[str]:
    lines = ["    mov.u32 %r2, 0;"]
    for index in range(case.checksum_words):
        lines.append(f"    add.u32 %r2, %r2, {state_reg(index)};")
    lines.extend(
        [
            "    ld.param.u64 %rd0, [out_ptr];",
            "    cvta.to.global.u64 %rd1, %rd0;",
            "    mul.wide.u32 %rd2, %r0, 4;",
            "    add.u64 %rd1, %rd1, %rd2;",
            "    st.global.u32 [%rd1], %r2;",
            "    ret;",
        ]
    )
    return lines


def state_reg(index: int) -> str:
    return f"%r{16 + index}"


def write_ptx(case: BenchCase, arch: str, out_path: Path) -> None:
    out_path.write_text(render_ptx(case, arch) + "\n", encoding="utf-8")


def run_case(case: BenchCase, arch: str, ptxas_opt_level: str, paths: dict[str, Path]) -> CaseResult:
    ptx_path = paths["ptx"] / f"{case.name}.ptx"
    cubin_path = paths["cubin"] / f"{case.name}.cubin"
    sass_path = paths["sass"] / f"{case.name}.sass"
    write_ptx(case, arch, ptx_path)
    ptxas_output = run_command([PTXAS, "-v", ptxas_opt_level, f"-arch={arch}", str(ptx_path), "-o", str(cubin_path)])
    sass_text = run_command([CUOBJDUMP, "--dump-sass", str(cubin_path)])
    sass_path.write_text(sass_text, encoding="utf-8")
    call_window_ldl_stl, call_count = write_call_windows(case, sass_text, paths["call_windows"])
    ptx_text = ptx_path.read_text(encoding="utf-8")
    registers, lmem_bytes, stack_frame, spill_stores, spill_loads = parse_ptxas_info(ptxas_output)
    pret_count, ldl_count, stl_count = count_sass_features(sass_text)
    return CaseResult(
        case=case,
        ptx_bytes=len(ptx_text.encode("utf-8")),
        ptx_lines=len(ptx_text.splitlines()),
        ptx_param_defs=ptx_text.count(".param "),
        ptx_ld_param=ptx_text.count("ld.param"),
        ptx_st_param=ptx_text.count("st.param"),
        ptx_ld_local=ptx_text.count("ld.local"),
        ptx_st_local=ptx_text.count("st.local"),
        registers=registers,
        lmem_bytes=lmem_bytes,
        stack_frame=stack_frame,
        spill_stores=spill_stores,
        spill_loads=spill_loads,
        call_count=call_count,
        pret_count=pret_count,
        ldl_count=ldl_count,
        stl_count=stl_count,
        call_window_ldl_stl=call_window_ldl_stl,
        sass_path=sass_path,
        ptx_path=ptx_path,
    )


def run_command(argv: list[str]) -> str:
    proc = subprocess.run(argv, check=False, capture_output=True, text=True)
    if proc.returncode != 0:
        joined = " ".join(argv)
        raise SystemExit(f"command failed ({proc.returncode}): {joined}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
    return proc.stdout + proc.stderr


def parse_ptxas_info(text: str) -> tuple[int, int, int, int, int]:
    stack_matches = PTXAS_STACK_RE.findall(text)
    reg_match = PTXAS_REG_RE.search(text)
    lmem_match = PTXAS_LMEM_RE.search(text)
    if reg_match is None:
        raise SystemExit(f"failed to parse ptxas output:\n{text}")
    stack_frame = int(stack_matches[0][0]) if stack_matches else 0
    spill_stores = sum(int(match[1]) for match in stack_matches)
    spill_loads = sum(int(match[2]) for match in stack_matches)
    lmem_bytes = 0 if lmem_match is None or lmem_match.group(1) is None else int(lmem_match.group(1))
    return int(reg_match.group(1)), lmem_bytes, stack_frame, spill_stores, spill_loads


def count_sass_features(text: str) -> tuple[int, int, int]:
    mnemonics = [match.group(1) for match in SASS_MNEMONIC_RE.finditer(text)]
    pret_count = sum(1 for mnemonic in mnemonics if mnemonic == "PRET")
    ldl_count = sum(1 for mnemonic in mnemonics if mnemonic.startswith("LDL"))
    stl_count = sum(1 for mnemonic in mnemonics if mnemonic.startswith("STL"))
    return pret_count, ldl_count, stl_count


def write_call_windows(case: BenchCase, sass_text: str, out_dir: Path) -> tuple[int, int]:
    lines = sass_text.splitlines()
    call_indices = [index for index, line in enumerate(lines) if "CALL." in line or re.search(r"\bCALL\b", line)]
    total_ldl_stl = 0
    for window_id, call_index in enumerate(call_indices):
        start = max(0, call_index - CALL_WINDOW_RADIUS)
        end = min(len(lines), call_index + CALL_WINDOW_RADIUS + 1)
        window_lines = lines[start:end]
        (out_dir / f"{case.name}.call{window_id}.txt").write_text("\n".join(window_lines) + "\n", encoding="utf-8")
        total_ldl_stl += sum(1 for line in window_lines if " LDL" in f" {line}" or " STL" in f" {line}")
    return total_ldl_stl, len(call_indices)


def write_summary(results: list[CaseResult], out_dir: Path, charts_dir: Path, arch: str, ptxas_opt_level: str) -> None:
    ordered_results = sorted(results, key=result_sort_key)
    csv_path = out_dir / "summary.csv"
    with csv_path.open("w", encoding="utf-8", newline="") as fp:
        writer = csv.writer(fp)
        writer.writerow(
            [
                "case",
                "abi",
                "state_words",
                "state_bytes",
                "use_mode",
                "use_ratio_percent",
                "used_words",
                "checksum_words",
                "helper_ops",
                "ptx_bytes",
                "ptx_lines",
                "ptx_param_defs",
                "ptx_ld_param",
                "ptx_st_param",
                "ptx_ld_local",
                "ptx_st_local",
                "registers",
                "lmem_bytes",
                "stack_frame",
                "local_footprint_bytes",
                "spill_stores_total_bytes",
                "spill_loads_total_bytes",
                "spill_total_bytes",
                "call_count",
                "pret_count",
                "ldl_count",
                "stl_count",
                "ldl_stl_total",
                "call_window_ldl_stl",
                "ptx_path",
                "sass_path",
            ]
        )
        for result in ordered_results:
            writer.writerow(
                [
                    result.case.name,
                    result.case.abi,
                    result.case.state_words,
                    result.case.state_bytes,
                    result.case.use_mode,
                    result.case.use_ratio_percent,
                    result.case.used_words,
                    result.case.checksum_words,
                    result.case.helper_ops,
                    result.ptx_bytes,
                    result.ptx_lines,
                    result.ptx_param_defs,
                    result.ptx_ld_param,
                    result.ptx_st_param,
                    result.ptx_ld_local,
                    result.ptx_st_local,
                    result.registers,
                    result.lmem_bytes,
                    result.stack_frame,
                    result.local_footprint_bytes,
                    result.spill_stores,
                    result.spill_loads,
                    result.spill_total_bytes,
                    result.call_count,
                    result.pret_count,
                    result.ldl_count,
                    result.stl_count,
                    result.ldl_stl_total,
                    result.call_window_ldl_stl,
                    result.ptx_path.as_posix(),
                    result.sass_path.as_posix(),
                ]
            )
    chart_files = write_charts(ordered_results, charts_dir)
    write_chart_index(chart_files, charts_dir)
    write_report(ordered_results, out_dir / "REPORT.md", chart_files, arch, ptxas_opt_level)


def write_report(
    results: list[CaseResult], report_path: Path, chart_files: list[Path], arch: str, ptxas_opt_level: str
) -> None:
    state_words_text = ", ".join(str(v) for v in sorted({r.case.state_words for r in results}))
    use_modes = ordered_modes(results)
    use_mode_desc = "；".join(describe_mode(mode) for mode in use_modes)
    helper_ops_text = ", ".join(str(v) for v in sorted({r.case.helper_ops for r in results}))
    lines = [
        "# PTX Direct Call 参数 ABI 实验汇总",
        "",
        f"- `arch`: `{arch}`",
        f"- `ptxas_opt_level`: `{ptxas_opt_level}`",
        f"- `state_words`: `{state_words_text}`",
        f"- `use_modes`: `{', '.join(mode_label(v) for v in use_modes)}`",
        f"- `helper_ops`: `{helper_ops_text}`",
        "",
        "## 参数说明",
        "",
        f"- `state_words`：状态向量规模，单位为 32-bit word。本次结果覆盖 `{state_words_text}`。",
        f"- `use_mode`：callee 真正参与 helper 计算的状态比例。本次结果覆盖：{use_mode_desc}。",
        f"- `helper_ops`：每个活跃状态字在 helper 中执行的操作次数，用来区分短 helper 和长 helper。本次结果覆盖 `{helper_ops_text}`。",
        "",
        "## 图表",
        "",
        "- `local_footprint_bytes = lmem_bytes + stack_frame`",
        "- `spill_total_bytes = spill_stores_total_bytes + spill_loads_total_bytes`",
        "",
    ]
    for chart_file in chart_files:
        lines.append(f"### `{chart_file.stem}`")
        lines.append("")
        lines.append(f"- {CHART_EXPLANATIONS.get(chart_file.stem, '关键指标图。')}")
        lines.append("")
        lines.append(f"![{chart_file.stem}](charts/{chart_file.name})")
        lines.append("")
    lines.extend(
        [
            "## 结果表",
            "",
            "| case | used/checksum | regs | lmem | stack | spill_total | calls | LDL/STL | call-window | PTX bytes |",
            "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for result in results:
        used = f"{result.case.used_words}/{result.case.checksum_words}"
        lines.append(
            f"| `{result.case.name}` | {used} | {result.registers} | {result.lmem_bytes} | {result.stack_frame} | "
            f"{result.spill_total_bytes} | {result.call_count} | {result.ldl_stl_total} | "
            f"{result.call_window_ldl_stl} | {result.ptx_bytes} |"
        )
    lines.extend(
        [
            "",
            "## 解读提示",
            "",
            "- `pct25/pct50/pct75/full` 都保持 caller 对全状态做 checksum，因此冷状态仍需正确 passthrough。",
            "- `hot4_dead` 仅作为历史 deadcode 对照保留，不再属于默认实验矩阵。",
            "- `value_params` 仍可能触发 `ptxas` 的 ABI disable warning，应作为探索性 proxy 解读。",
            "- 先横向比较同一 `state_words/use_mode/helper_ops` 下的三种 ABI，再纵向比较不同 `use_mode` 的拐点。",
            "",
        ]
    )
    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def ordered_modes(results: list[CaseResult]) -> list[str]:
    return sorted({result.case.use_mode for result in results}, key=lambda mode: MODE_ORDER[mode])


def describe_mode(mode: str) -> str:
    if mode == "pct25":
        return "`pct25` = callee 使用前 25% 状态，caller 仍校验全部状态"
    if mode == "pct50":
        return "`pct50` = callee 使用前 50% 状态，caller 仍校验全部状态"
    if mode == "pct75":
        return "`pct75` = callee 使用前 75% 状态，caller 仍校验全部状态"
    if mode == "full":
        return "`full` = callee 使用全部状态，caller 校验全部状态"
    if mode == "hot4":
        return "`hot4` = callee 只使用前 4 个状态，caller 仍校验全部状态"
    if mode == "hot4_dead":
        return "`hot4_dead` = callee 只使用前 4 个状态，caller 也只校验前 4 个状态"
    raise ValueError(f"unsupported mode: {mode}")


def write_charts(results: list[CaseResult], charts_dir: Path) -> list[Path]:
    chart_files: list[Path] = []
    for metric_key, title, unit in CHART_SPECS:
        chart_path = charts_dir / f"{metric_key}.svg"
        chart_path.write_text(render_metric_chart(results, metric_key, title, unit), encoding="utf-8")
        chart_files.append(chart_path)
    return chart_files


def write_chart_index(chart_files: list[Path], charts_dir: Path) -> None:
    lines = ["# 图表索引", ""]
    for chart_file in chart_files:
        lines.append(f"- `{chart_file.name}`")
    (charts_dir / "INDEX.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def render_metric_chart(results: list[CaseResult], metric_key: str, title: str, unit: str) -> str:
    helper_ops = sorted({result.case.helper_ops for result in results})
    modes = ordered_modes(results)
    state_words = sorted({result.case.state_words for result in results})
    values = [metric_value(result, metric_key) for result in results if result.case.use_mode in modes]
    y_max = max(values) if values else 1
    if y_max <= 0:
        y_max = 1
    rows = len(helper_ops)
    cols = len(modes)
    plot_width = SVG_WIDTH - SVG_MARGIN_LEFT - SVG_MARGIN_RIGHT
    plot_height = SVG_HEIGHT - SVG_MARGIN_TOP - SVG_MARGIN_BOTTOM
    panel_width = (plot_width - SVG_PANEL_GAP_X * (cols - 1)) / max(1, cols)
    panel_height = (plot_height - SVG_PANEL_GAP_Y * (rows - 1)) / max(1, rows)
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{SVG_WIDTH}" height="{SVG_HEIGHT}" viewBox="0 0 {SVG_WIDTH} {SVG_HEIGHT}">',
        '<rect width="100%" height="100%" fill="#fcfcfb"/>',
        f'<text x="{SVG_MARGIN_LEFT}" y="42" font-size="28" font-family="sans-serif" fill="#1f2933">{escape_svg(title)}</text>',
        f'<text x="{SVG_MARGIN_LEFT}" y="70" font-size="14" font-family="sans-serif" fill="#52606d">Rows: helper_ops, Columns: use_mode, X: state_words, Y: {escape_svg(unit)}</text>',
    ]
    parts.extend(render_legend())
    for row, helper_op in enumerate(helper_ops):
        for col, mode in enumerate(modes):
            panel_x = SVG_MARGIN_LEFT + col * (panel_width + SVG_PANEL_GAP_X)
            panel_y = SVG_MARGIN_TOP + row * (panel_height + SVG_PANEL_GAP_Y)
            parts.extend(render_panel_frame(panel_x, panel_y, panel_width, panel_height, helper_op, mode))
            for abi in ABI_MODES:
                points = points_for_series(results, abi, helper_op, mode, state_words, panel_x, panel_y, panel_width, panel_height, y_max, metric_key)
                if not points:
                    continue
                color = ABI_COLORS[abi]
                if len(points) >= 2:
                    point_list = " ".join(f"{x:.2f},{y:.2f}" for x, y in points)
                    parts.append(
                        f'<polyline points="{point_list}" fill="none" stroke="{color}" stroke-width="{SVG_STROKE_WIDTH}" />'
                    )
                for x, y in points:
                    parts.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{SVG_POINT_RADIUS}" fill="{color}" />')
            parts.extend(render_axes_labels(panel_x, panel_y, panel_width, panel_height, state_words, y_max))
    parts.append("</svg>")
    return "\n".join(parts) + "\n"


def render_legend() -> list[str]:
    parts: list[str] = []
    legend_x = SVG_MARGIN_LEFT
    legend_y = 92
    gap = 180
    for index, abi in enumerate(ABI_MODES):
        x = legend_x + index * gap
        color = ABI_COLORS[abi]
        parts.append(f'<line x1="{x}" y1="{legend_y}" x2="{x + 28}" y2="{legend_y}" stroke="{color}" stroke-width="{SVG_STROKE_WIDTH}" />')
        parts.append(f'<circle cx="{x + 14}" cy="{legend_y}" r="{SVG_POINT_RADIUS}" fill="{color}" />')
        parts.append(f'<text x="{x + 40}" y="{legend_y + 5}" font-size="14" font-family="sans-serif" fill="#1f2933">{escape_svg(abi)}</text>')
    return parts


def render_panel_frame(panel_x: float, panel_y: float, panel_width: float, panel_height: float, helper_op: int, mode: str) -> list[str]:
    return [
        f'<rect x="{panel_x:.2f}" y="{panel_y:.2f}" width="{panel_width:.2f}" height="{panel_height:.2f}" fill="#ffffff" stroke="#d9e2ec" />',
        f'<text x="{panel_x + 10:.2f}" y="{panel_y + 20:.2f}" font-size="14" font-family="sans-serif" fill="#334e68">o{helper_op} / {escape_svg(mode_label(mode))}</text>',
    ]


def render_axes_labels(panel_x: float, panel_y: float, panel_width: float, panel_height: float, state_words: list[int], y_max: int) -> list[str]:
    parts = []
    plot_left = panel_x + 42
    plot_top = panel_y + 28
    plot_right = panel_x + panel_width - 14
    plot_bottom = panel_y + panel_height - 30
    parts.append(f'<line x1="{plot_left:.2f}" y1="{plot_bottom:.2f}" x2="{plot_right:.2f}" y2="{plot_bottom:.2f}" stroke="#9fb3c8" />')
    parts.append(f'<line x1="{plot_left:.2f}" y1="{plot_top:.2f}" x2="{plot_left:.2f}" y2="{plot_bottom:.2f}" stroke="#9fb3c8" />')
    for state_word, x in zip(state_words, x_positions(plot_left, plot_right, len(state_words)), strict=True):
        parts.append(f'<text x="{x:.2f}" y="{plot_bottom + 18:.2f}" text-anchor="middle" font-size="11" font-family="sans-serif" fill="#52606d">{state_word}</text>')
    tick_values = [0, y_max / 2, y_max]
    for tick in tick_values:
        y = scale_y(tick, plot_top, plot_bottom, y_max)
        parts.append(f'<line x1="{plot_left:.2f}" y1="{y:.2f}" x2="{plot_right:.2f}" y2="{y:.2f}" stroke="#eef2f6" />')
        parts.append(f'<text x="{plot_left - 6:.2f}" y="{y + 4:.2f}" text-anchor="end" font-size="11" font-family="sans-serif" fill="#52606d">{int(round(tick))}</text>')
    return parts


def points_for_series(
    results: list[CaseResult],
    abi: str,
    helper_op: int,
    mode: str,
    state_words: list[int],
    panel_x: float,
    panel_y: float,
    panel_width: float,
    panel_height: float,
    y_max: int,
    metric_key: str,
) -> list[tuple[float, float]]:
    plot_left = panel_x + 42
    plot_top = panel_y + 28
    plot_right = panel_x + panel_width - 14
    plot_bottom = panel_y + panel_height - 30
    result_map = {
        result.case.state_words: result
        for result in results
        if result.case.abi == abi and result.case.helper_ops == helper_op and result.case.use_mode == mode
    }
    positions = x_positions(plot_left, plot_right, len(state_words))
    points: list[tuple[float, float]] = []
    for state_word, x in zip(state_words, positions, strict=True):
        result = result_map.get(state_word)
        if result is None:
            continue
        points.append((x, scale_y(metric_value(result, metric_key), plot_top, plot_bottom, y_max)))
    return points


def x_positions(plot_left: float, plot_right: float, count: int) -> list[float]:
    if count == 1:
        return [(plot_left + plot_right) / 2]
    step = (plot_right - plot_left) / (count - 1)
    return [plot_left + index * step for index in range(count)]


def scale_y(value: float, plot_top: float, plot_bottom: float, y_max: int) -> float:
    if y_max <= 0:
        return plot_bottom
    return plot_bottom - (value / y_max) * (plot_bottom - plot_top)


def metric_value(result: CaseResult, metric_key: str) -> int:
    if metric_key == "registers":
        return result.registers
    if metric_key == "ldl_stl_total":
        return result.ldl_stl_total
    if metric_key == "call_window_ldl_stl":
        return result.call_window_ldl_stl
    if metric_key == "local_footprint_bytes":
        return result.local_footprint_bytes
    if metric_key == "spill_total_bytes":
        return result.spill_total_bytes
    raise ValueError(f"unsupported metric: {metric_key}")


def escape_svg(text: str) -> str:
    return html.escape(text, quote=True)


def main() -> None:
    args = parse_args()
    require_tool(PTXAS)
    require_tool(CUOBJDUMP)
    cases = build_cases(args)
    paths = ensure_dirs(Path(args.out_dir))
    results: list[CaseResult] = []
    for case in cases:
        print(f"[run] {case.name}")
        results.append(run_case(case, args.arch, args.ptxas_opt_level, paths))
    write_summary(results, paths["root"], paths["charts"], args.arch, args.ptxas_opt_level)
    print(f"wrote {paths['root'] / 'summary.csv'}")
    print(f"wrote {paths['root'] / 'REPORT.md'}")
    print(f"wrote {paths['charts'] / 'INDEX.md'}")


if __name__ == "__main__":
    main()
