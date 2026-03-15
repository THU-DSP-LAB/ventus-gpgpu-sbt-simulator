#!/usr/bin/env python3
# 背景：
# 本实验要回答的是：如果一批状态只在 PTX call 边界以 blob / 指针 backing store 形式出现，
# 但不再进入真正可观察的计算链路，ptxas 是否会把这部分冷状态有效消去。
#
# 需求/作用：
# 生成手写 PTX 微基准，对比 value_blob 与 pointer_store 两类 ABI，
# 编译并反汇编后采集 ptxas / SASS 指标，最后输出 summary.csv 和 REPORT.md。
#
# 用法：
#   python3 tools/run_experiment.py --out-dir build
#   python3 tools/run_experiment.py --match value_blob_input_dead_s512
#   python3 tools/run_experiment.py --state-words 256 512 --helper-ops 4 32
#
# 实现原理/处理步骤：
# 1. 生成四类 dead-state 场景的最小 PTX。
# 2. 调用 ptxas / cuobjdump 获取资源与 SASS。
# 3. 统计 `.param`、`ld/st.local`、CALL 邻域 LDL/STL 等指标。
# 4. 写出 CSV 与 Markdown 报告，保证默认矩阵可复现。
from __future__ import annotations

import argparse
import csv
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

DEFAULT_ARCH = "sm_89"
PTX_VERSION = "7.8"
ADDRESS_SIZE = 64
DEFAULT_STATE_WORDS = (32, 128, 256, 512, 600)
DEFAULT_HELPER_OPS = (4, 32)
ABI_MODES = ("value_blob", "pointer_store")
SCENARIOS = ("input_dead", "passthrough_dead", "ret_dead", "live_subset")
HOT_WORDS = 4
CALL_WINDOW_RADIUS = 12
PTXAS = "ptxas"
CUOBJDUMP = "cuobjdump"
SASS_MNEMONIC_RE = re.compile(r"/\*[0-9a-fA-F]+\*/\s+([A-Z][A-Z0-9_.]*)")
PTXAS_STACK_RE = re.compile(
    r"([0-9]+) bytes stack frame, ([0-9]+) bytes spill stores, ([0-9]+) bytes spill loads"
)
PTXAS_REG_RE = re.compile(r"Used ([0-9]+) registers")
PTXAS_LMEM_RE = re.compile(
    r"Used [0-9]+ registers, used [0-9]+ barriers, [0-9]+ bytes cmem\[0\](?:, ([0-9]+) bytes lmem)?"
)
SCENARIO_DESCRIPTIONS = {
    "input_dead": "全状态进入 call 输入 ABI，但 callee 与 caller 只观察热子集。",
    "passthrough_dead": "冷状态形式上输入并返回，只在 helper 内做 ABI copy，caller 不再观察。",
    "ret_dead": "callee 形式上返回整块状态，但 caller 只读取热子集。",
    "live_subset": "热子集真实计算，冷子集必须正确 passthrough，caller 对全状态做校验。",
}


@dataclass(frozen=True)
class BenchCase:
    abi: str
    scenario: str
    state_words: int
    helper_ops: int

    @property
    def name(self) -> str:
        return f"{self.abi}_{self.scenario}_s{self.state_words}_o{self.helper_ops}"

    @property
    def state_bytes(self) -> int:
        return self.state_words * 4

    @property
    def hot_words(self) -> int:
        return min(HOT_WORDS, self.state_words)


@dataclass(frozen=True)
class CaseResult:
    case: BenchCase
    ptx_path: Path
    sass_path: Path
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

    @property
    def spill_total_bytes(self) -> int:
        return self.spill_stores + self.spill_loads

    @property
    def local_footprint_bytes(self) -> int:
        return self.lmem_bytes + self.stack_frame

    @property
    def ldl_stl_total(self) -> int:
        return self.ldl_count + self.stl_count


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run PTX call-boundary dead-state micro-benchmarks.")
    parser.add_argument("--arch", default=DEFAULT_ARCH)
    parser.add_argument("--state-words", nargs="+", type=int, default=list(DEFAULT_STATE_WORDS))
    parser.add_argument("--helper-ops", nargs="+", type=int, default=list(DEFAULT_HELPER_OPS))
    parser.add_argument("--abis", nargs="+", choices=ABI_MODES, default=list(ABI_MODES))
    parser.add_argument("--scenarios", nargs="+", choices=SCENARIOS, default=list(SCENARIOS))
    parser.add_argument("--match", default="")
    parser.add_argument("--out-dir", default="build")
    parser.add_argument("--ptxas-opt-level", default="-O3", choices=("-O0", "-O1", "-O2", "-O3"))
    return parser.parse_args()


def require_tool(name: str) -> None:
    if shutil.which(name) is None:
        raise SystemExit(f"missing required tool: {name}")


def build_cases(args: argparse.Namespace) -> list[BenchCase]:
    cases: list[BenchCase] = []
    for abi in args.abis:
        for scenario in args.scenarios:
            for state_words in args.state_words:
                for helper_ops in args.helper_ops:
                    case = BenchCase(abi=abi, scenario=scenario, state_words=state_words, helper_ops=helper_ops)
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
    }
    for path in paths.values():
        path.mkdir(parents=True, exist_ok=True)
    return paths


def render_ptx(case: BenchCase, arch: str) -> str:
    header = [f".version {PTX_VERSION}", f".target {arch}", f".address_size {ADDRESS_SIZE}", ""]
    body = render_value_blob_case(case) if case.abi == "value_blob" else render_pointer_store_case(case)
    return "\n".join(header + body) + "\n"


def render_value_blob_case(case: BenchCase) -> list[str]:
    helper_name = helper_symbol(case)
    kernel_name = kernel_symbol(case)
    lines = declare_blob_helper(case, helper_name)
    lines.extend(["", f".visible .entry {kernel_name}(", "    .param .u64 out_ptr,", "    .param .u32 seed", ")", "{"])
    lines.extend(common_regs())
    lines.extend(kernel_preamble())
    lines.extend(render_blob_kernel_body(case, helper_name))
    lines.extend(store_checksum_and_ret())
    lines.append("}")
    lines.append("")
    lines.extend(define_blob_helper(case, helper_name))
    return lines


def render_pointer_store_case(case: BenchCase) -> list[str]:
    helper_name = helper_symbol(case)
    kernel_name = kernel_symbol(case)
    lines = declare_pointer_helper(case, helper_name)
    lines.extend(["", f".visible .entry {kernel_name}(", "    .param .u64 out_ptr,", "    .param .u32 seed", ")", "{"])
    lines.extend(common_regs())
    lines.extend(kernel_preamble())
    lines.extend(render_pointer_kernel_body(case, helper_name))
    lines.extend(store_checksum_and_ret())
    lines.append("}")
    lines.append("")
    lines.extend(define_pointer_helper(case, helper_name))
    return lines


def declare_blob_helper(case: BenchCase, helper_name: str) -> list[str]:
    if case.scenario == "input_dead":
        return [".func (", "    .reg .b32 out_scalar", f") {helper_name}(", f"    .param .align 4 .b8 in_blob[{case.state_bytes}]", ");"]
    if case.scenario == "ret_dead":
        return [".func (", f"    .param .align 4 .b8 out_blob[{case.state_bytes}]", f") {helper_name}(", "    .param .u32 seed", ");"]
    return [
        ".func (",
        f"    .param .align 4 .b8 out_blob[{case.state_bytes}]",
        f") {helper_name}(",
        f"    .param .align 4 .b8 in_blob[{case.state_bytes}]",
        ");",
    ]


def define_blob_helper(case: BenchCase, helper_name: str) -> list[str]:
    lines = declare_blob_helper(case, helper_name)
    lines[-1] = ")"
    lines.append("{")
    lines.extend(common_regs())
    if case.scenario == "input_dead":
        lines.extend(load_hot_from_param_blob("in_blob", case))
        lines.extend(apply_helper_ops(case))
        lines.extend(init_checksum())
        lines.extend(add_hot_to_checksum(case))
        lines.append("    mov.u32 out_scalar, %r2;")
    elif case.scenario == "ret_dead":
        lines.append("    ld.param.u32 %r1, [seed];")
        lines.extend(write_generated_blob("out_blob", case, include_cold=True))
    else:
        lines.extend(load_hot_from_param_blob("in_blob", case))
        lines.extend(apply_helper_ops(case))
        if case.scenario == "passthrough_dead":
            lines.extend(write_blob_with_cold_copy("in_blob", "out_blob", case, cold_copy=True))
        else:
            lines.extend(write_blob_with_cold_copy("in_blob", "out_blob", case, cold_copy=True))
    lines.append("    ret;")
    lines.append("}")
    return lines


def declare_pointer_helper(case: BenchCase, helper_name: str) -> list[str]:
    if case.scenario == "input_dead":
        return [".func (", "    .reg .b32 out_scalar", f") {helper_name}(", "    .param .u64 in_ptr", ");"]
    if case.scenario == "ret_dead":
        return [f".func {helper_name}(", "    .param .u64 out_ptr,", "    .param .u32 seed", ");"]
    return [f".func {helper_name}(", "    .param .u64 in_ptr,", "    .param .u64 out_ptr", ");"]


def define_pointer_helper(case: BenchCase, helper_name: str) -> list[str]:
    lines = declare_pointer_helper(case, helper_name)
    lines[-1] = ")"
    lines.append("{")
    lines.extend(common_regs())
    if case.scenario == "input_dead":
        lines.append("    ld.param.u64 %rd4, [in_ptr];")
        lines.extend(load_hot_from_local("%rd4", case))
        lines.extend(apply_helper_ops(case))
        lines.extend(init_checksum())
        lines.extend(add_hot_to_checksum(case))
        lines.append("    mov.u32 out_scalar, %r2;")
    elif case.scenario == "ret_dead":
        lines.extend(["    ld.param.u64 %rd5, [out_ptr];", "    ld.param.u32 %r1, [seed];"])
        lines.extend(write_generated_local("%rd5", case, include_cold=True))
    else:
        lines.extend(["    ld.param.u64 %rd4, [in_ptr];", "    ld.param.u64 %rd5, [out_ptr];"])
        lines.extend(load_hot_from_local("%rd4", case))
        lines.extend(apply_helper_ops(case))
        lines.extend(write_local_with_cold_copy("%rd4", "%rd5", case))
    lines.append("    ret;")
    lines.append("}")
    return lines


def common_regs() -> list[str]:
    return ["    .reg .b32 %r<32>;", "    .reg .b64 %rd<16>;"]


def kernel_preamble() -> list[str]:
    return ["    mov.u32 %r0, %tid.x;", "    ld.param.u32 %r1, [seed];", "    mov.u32 %r2, 0;"]


def render_blob_kernel_body(case: BenchCase, helper_name: str) -> list[str]:
    if case.scenario == "input_dead":
        lines = [f"    .param .align 4 .b8 arg_blob[{case.state_bytes}];"]
        lines.extend(write_generated_blob("arg_blob", case, include_cold=True))
        lines.append(f"    call.uni (%r16), {helper_name}, (arg_blob);")
        lines.append("    add.u32 %r2, %r2, %r16;")
        return lines
    if case.scenario == "ret_dead":
        lines = [f"    .param .align 4 .b8 ret_blob[{case.state_bytes}];"]
        lines.append(f"    call.uni (ret_blob), {helper_name}, (%r1);")
        lines.extend(add_blob_to_checksum("ret_blob", case, hot_only=True))
        return lines
    lines = [f"    .param .align 4 .b8 arg_blob[{case.state_bytes}];", f"    .param .align 4 .b8 ret_blob[{case.state_bytes}];"]
    lines.extend(write_generated_blob("arg_blob", case, include_cold=True))
    lines.append(f"    call.uni (ret_blob), {helper_name}, (arg_blob);")
    lines.extend(add_blob_to_checksum("ret_blob", case, hot_only=case.scenario != "live_subset"))
    return lines


def render_pointer_kernel_body(case: BenchCase, helper_name: str) -> list[str]:
    lines: list[str] = []
    if case.scenario == "input_dead":
        lines.append(f"    .local .align 4 .b8 in_state[{case.state_bytes}];")
        lines.append("    mov.u64 %rd4, in_state;")
        lines.extend(write_generated_local("%rd4", case, include_cold=True))
        lines.append("    .param .u64 arg_in_ptr;")
        lines.append("    st.param.u64 [arg_in_ptr], %rd4;")
        lines.append(f"    call.uni (%r16), {helper_name}, (arg_in_ptr);")
        lines.append("    add.u32 %r2, %r2, %r16;")
        return lines
    if case.scenario == "ret_dead":
        lines.extend([f"    .local .align 4 .b8 out_state[{case.state_bytes}];", "    mov.u64 %rd5, out_state;", "    .param .u64 arg_out_ptr;", "    .param .u32 arg_seed;", "    st.param.u64 [arg_out_ptr], %rd5;", "    st.param.u32 [arg_seed], %r1;"])
        lines.append(f"    call.uni {helper_name}, (arg_out_ptr, arg_seed);")
        lines.extend(add_local_to_checksum("%rd5", case, hot_only=True))
        return lines
    lines.extend([f"    .local .align 4 .b8 in_state[{case.state_bytes}];", f"    .local .align 4 .b8 out_state[{case.state_bytes}];", "    mov.u64 %rd4, in_state;", "    mov.u64 %rd5, out_state;"])
    lines.extend(write_generated_local("%rd4", case, include_cold=True))
    lines.extend(["    .param .u64 arg_in_ptr;", "    .param .u64 arg_out_ptr;", "    st.param.u64 [arg_in_ptr], %rd4;", "    st.param.u64 [arg_out_ptr], %rd5;"])
    lines.append(f"    call.uni {helper_name}, (arg_in_ptr, arg_out_ptr);")
    lines.extend(add_local_to_checksum("%rd5", case, hot_only=case.scenario != "live_subset"))
    return lines


def helper_symbol(case: BenchCase) -> str:
    return f"helper_{case.abi}_{case.scenario}"


def kernel_symbol(case: BenchCase) -> str:
    return f"kernel_{case.abi}_{case.scenario}"


def write_generated_blob(target: str, case: BenchCase, include_cold: bool) -> list[str]:
    lines: list[str] = []
    for index in range(case.state_words):
        if not include_cold and index >= case.hot_words:
            continue
        lines.extend(load_slot_value("%r8", index))
        lines.append(f"    st.param.b32 [{target}+{index * 4}], %r8;")
    return lines


def write_generated_local(base: str, case: BenchCase, include_cold: bool) -> list[str]:
    lines: list[str] = []
    for index in range(case.state_words):
        if not include_cold and index >= case.hot_words:
            continue
        lines.extend(load_slot_value("%r8", index))
        lines.append(f"    st.local.u32 [{base}+{index * 4}], %r8;")
    return lines


def load_slot_value(dst: str, index: int) -> list[str]:
    bias = 13 * (index + 1)
    return [f"    add.u32 {dst}, %r0, {bias};", f"    add.u32 {dst}, {dst}, %r1;"]


def load_hot_from_param_blob(source: str, case: BenchCase) -> list[str]:
    return [f"    ld.param.b32 {hot_reg(index)}, [{source}+{index * 4}];" for index in range(case.hot_words)]


def load_hot_from_local(base: str, case: BenchCase) -> list[str]:
    return [f"    ld.local.u32 {hot_reg(index)}, [{base}+{index * 4}];" for index in range(case.hot_words)]


def hot_reg(index: int) -> str:
    return f"%r{16 + index}"


def apply_helper_ops(case: BenchCase) -> list[str]:
    lines: list[str] = []
    for index in range(case.hot_words):
        reg = hot_reg(index)
        for op_index in range(case.helper_ops):
            lines.append("    " + helper_op(reg, index, op_index))
    return lines


def helper_op(reg: str, state_index: int, op_index: int) -> str:
    add_imm = 3 + ((state_index + op_index) % 23)
    xor_imm = (((state_index + 1) * 0x45D9F3B) ^ (op_index * 0x9E3779B1)) & 0xFFFFFFFF
    mad_add = 7 + ((state_index * 5 + op_index) % 19)
    if op_index % 3 == 0:
        return f"add.u32 {reg}, {reg}, {add_imm};"
    if op_index % 3 == 1:
        return f"xor.b32 {reg}, {reg}, 0x{xor_imm:08x};"
    return f"mad.lo.u32 {reg}, {reg}, 3, {mad_add};"


def init_checksum() -> list[str]:
    return ["    mov.u32 %r2, 0;"]


def add_hot_to_checksum(case: BenchCase) -> list[str]:
    return [f"    add.u32 %r2, %r2, {hot_reg(index)};" for index in range(case.hot_words)]


def write_blob_with_cold_copy(source: str, target: str, case: BenchCase, cold_copy: bool) -> list[str]:
    lines = [f"    st.param.b32 [{target}+{index * 4}], {hot_reg(index)};" for index in range(case.hot_words)]
    for index in range(case.hot_words, case.state_words):
        if cold_copy:
            lines.append(f"    ld.param.b32 %r8, [{source}+{index * 4}];")
            lines.append(f"    st.param.b32 [{target}+{index * 4}], %r8;")
    return lines


def write_local_with_cold_copy(in_base: str, out_base: str, case: BenchCase) -> list[str]:
    lines = [f"    st.local.u32 [{out_base}+{index * 4}], {hot_reg(index)};" for index in range(case.hot_words)]
    for index in range(case.hot_words, case.state_words):
        lines.append(f"    ld.local.u32 %r8, [{in_base}+{index * 4}];")
        lines.append(f"    st.local.u32 [{out_base}+{index * 4}], %r8;")
    return lines


def add_blob_to_checksum(source: str, case: BenchCase, hot_only: bool) -> list[str]:
    stop = case.hot_words if hot_only else case.state_words
    lines: list[str] = []
    for index in range(stop):
        lines.append(f"    ld.param.b32 %r8, [{source}+{index * 4}];")
        lines.append("    add.u32 %r2, %r2, %r8;")
    return lines


def add_local_to_checksum(base: str, case: BenchCase, hot_only: bool) -> list[str]:
    stop = case.hot_words if hot_only else case.state_words
    lines: list[str] = []
    for index in range(stop):
        lines.append(f"    ld.local.u32 %r8, [{base}+{index * 4}];")
        lines.append("    add.u32 %r2, %r2, %r8;")
    return lines


def store_checksum_and_ret() -> list[str]:
    return [
        "    ld.param.u64 %rd0, [out_ptr];",
        "    cvta.to.global.u64 %rd1, %rd0;",
        "    mul.wide.u32 %rd2, %r0, 4;",
        "    add.u64 %rd1, %rd1, %rd2;",
        "    st.global.u32 [%rd1], %r2;",
        "    ret;",
    ]


def write_ptx(case: BenchCase, arch: str, path: Path) -> None:
    path.write_text(render_ptx(case, arch), encoding="utf-8")


def run_command(argv: list[str]) -> str:
    proc = subprocess.run(argv, check=False, capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(
            f"command failed ({proc.returncode}): {' '.join(argv)}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    return proc.stdout + proc.stderr


def parse_ptxas_info(text: str) -> tuple[int, int, int, int, int]:
    reg_match = PTXAS_REG_RE.search(text)
    if reg_match is None:
        raise SystemExit(f"failed to parse ptxas output:\n{text}")
    stack_matches = PTXAS_STACK_RE.findall(text)
    lmem_match = PTXAS_LMEM_RE.search(text)
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
    call_indices = [index for index, line in enumerate(lines) if "CALL" in line]
    total = 0
    for window_id, call_index in enumerate(call_indices):
        start = max(0, call_index - CALL_WINDOW_RADIUS)
        end = min(len(lines), call_index + CALL_WINDOW_RADIUS + 1)
        window = lines[start:end]
        (out_dir / f"{case.name}.call{window_id}.txt").write_text("\n".join(window) + "\n", encoding="utf-8")
        total += sum(1 for line in window if " LDL" in f" {line}" or " STL" in f" {line}")
    return total, len(call_indices)


def run_case(case: BenchCase, arch: str, opt_level: str, paths: dict[str, Path]) -> CaseResult:
    ptx_path = paths["ptx"] / f"{case.name}.ptx"
    cubin_path = paths["cubin"] / f"{case.name}.cubin"
    sass_path = paths["sass"] / f"{case.name}.sass"
    write_ptx(case, arch, ptx_path)
    ptxas_output = run_command([PTXAS, "-v", opt_level, f"-arch={arch}", str(ptx_path), "-o", str(cubin_path)])
    sass_text = run_command([CUOBJDUMP, "--dump-sass", str(cubin_path)])
    sass_path.write_text(sass_text, encoding="utf-8")
    registers, lmem_bytes, stack_frame, spill_stores, spill_loads = parse_ptxas_info(ptxas_output)
    pret_count, ldl_count, stl_count = count_sass_features(sass_text)
    call_window_ldl_stl, call_count = write_call_windows(case, sass_text, paths["call_windows"])
    ptx_text = ptx_path.read_text(encoding="utf-8")
    return CaseResult(
        case=case,
        ptx_path=ptx_path,
        sass_path=sass_path,
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
    )


def write_summary(results: list[CaseResult], out_dir: Path, arch: str, opt_level: str) -> None:
    results = sorted(results, key=lambda r: (r.case.scenario, r.case.state_words, r.case.helper_ops, r.case.abi))
    write_summary_csv(results, out_dir / "summary.csv")
    write_report(results, out_dir / "REPORT.md", arch, opt_level)


def write_summary_csv(results: list[CaseResult], path: Path) -> None:
    with path.open("w", encoding="utf-8", newline="") as fp:
        writer = csv.writer(fp)
        writer.writerow(
            [
                "case",
                "abi",
                "scenario",
                "state_words",
                "hot_words",
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
        for result in results:
            writer.writerow(
                [
                    result.case.name,
                    result.case.abi,
                    result.case.scenario,
                    result.case.state_words,
                    result.case.hot_words,
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


def write_report(results: list[CaseResult], path: Path, arch: str, opt_level: str) -> None:
    lines = [
        "# PTX Call Boundary Dead-State 实验汇总",
        "",
        f"- `arch`: `{arch}`",
        f"- `ptxas_opt_level`: `{opt_level}`",
        f"- `state_words`: `{', '.join(str(v) for v in sorted({r.case.state_words for r in results}))}`",
        f"- `helper_ops`: `{', '.join(str(v) for v in sorted({r.case.helper_ops for r in results}))}`",
        f"- `hot_words`: `{HOT_WORDS}`",
        "",
        "## 场景说明",
        "",
    ]
    for scenario in SCENARIOS:
        lines.append(f"- `{scenario}`：{SCENARIO_DESCRIPTIONS[scenario]}")
    lines.extend(["", "## 结果表", "", "| case | regs | local_footprint | spill_total | LDL/STL | call-window | PTX bytes |", "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"])
    for result in results:
        lines.append(
            f"| `{result.case.name}` | {result.registers} | {result.local_footprint_bytes} | {result.spill_total_bytes} | "
            f"{result.ldl_stl_total} | {result.call_window_ldl_stl} | {result.ptx_bytes} |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    args = parse_args()
    require_tool(PTXAS)
    require_tool(CUOBJDUMP)
    paths = ensure_dirs(Path(args.out_dir))
    results: list[CaseResult] = []
    for case in build_cases(args):
        print(f"[run] {case.name}")
        results.append(run_case(case, args.arch, args.ptxas_opt_level, paths))
    write_summary(results, paths["root"], args.arch, args.ptxas_opt_level)
    print(f"wrote {paths['root'] / 'summary.csv'}")
    print(f"wrote {paths['root'] / 'REPORT.md'}")


if __name__ == "__main__":
    main()
