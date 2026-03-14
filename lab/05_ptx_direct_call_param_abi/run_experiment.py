#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

DEFAULT_ARCH = "sm_75"
PTX_VERSION = "7.8"
ADDRESS_SIZE = 64
STATE_WORDS_DEFAULT = (32, 128, 256)
HELPER_OPS_DEFAULT = (4, 32)
ABI_MODES = ("vctx", "value_params", "value_blob")
USE_MODES = ("hot4", "hot4_dead", "full")
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
        return HOT_WORDS if self.use_mode in ("hot4", "hot4_dead") else self.state_words


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
        default=list(USE_MODES),
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
    lines.extend(
        [
            ".func (",
            blob_ret,
            ") helper_value_blob(",
            blob_decl,
            ")",
            "{",
        ]
    )
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
    lines: list[str] = []
    for index in range(count):
        suffix = "," if index != count - 1 else ""
        lines.append(f"    .reg .b32 {prefix}_{index}{suffix}")
    return lines


def join_param_lines(prefix: str, count: int, trailing_comma: bool) -> list[str]:
    lines: list[str] = []
    for index in range(count):
        suffix = "," if index != count - 1 or trailing_comma else ""
        lines.append(f"    .param .b32 {prefix}_{index}{suffix}")
    return lines


def render_common_regs(case: BenchCase) -> list[str]:
    reg_count = case.state_words + 32
    return [
        f"    .reg .b32 %r<{reg_count}>;",
        "    .reg .b64 %rd<8>;",
    ]


def common_entry_preamble() -> list[str]:
    return [
        "    mov.u32 %r0, %tid.x;",
        "    ld.param.u32 %r1, [seed];",
    ]


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
    mod = op_index % 3
    if mod == 0:
        return f"add.u32 {reg}, {reg}, {add_imm};"
    if mod == 1:
        return f"xor.b32 {reg}, {reg}, 0x{xor_imm & 0xFFFFFFFF:08x};"
    return f"mad.lo.u32 {reg}, {reg}, 3, {mad_add};"


def store_state_to_local(base: str, case: BenchCase) -> list[str]:
    return store_state_with_offsets(case, lambda index: f"    st.local.u32 [{base}+{index * 4}], {state_reg(index)};")


def load_state_from_local(base: str, case: BenchCase) -> list[str]:
    return load_state_with_offsets(case, lambda index: f"    ld.local.u32 {state_reg(index)}, [{base}+{index * 4}];")


def declare_scalar_param_block(prefix: str, count: int) -> list[str]:
    return [f"    .param .b32 {prefix}_{index};" for index in range(count)]


def store_state_to_scalar_params(prefix: str, case: BenchCase) -> list[str]:
    return store_state_with_offsets(case, lambda index: f"    st.param.b32 [{prefix}_{index}], {state_reg(index)};")


def load_state_from_scalar_params(prefix: str, case: BenchCase) -> list[str]:
    return load_state_with_offsets(case, lambda index: f"    ld.param.b32 {state_reg(index)}, [{prefix}_{index}];")


def move_state_to_return_regs(case: BenchCase) -> list[str]:
    return [f"    mov.u32 out_state_{index}, {state_reg(index)};" for index in range(case.state_words)]


def store_state_to_blob_params(prefix: str, case: BenchCase) -> list[str]:
    return store_state_with_offsets(case, lambda index: f"    st.param.b32 [{prefix}+{index * 4}], {state_reg(index)};")


def load_state_from_blob_params(prefix: str, case: BenchCase) -> list[str]:
    return load_state_with_offsets(case, lambda index: f"    ld.param.b32 {state_reg(index)}, [{prefix}+{index * 4}];")


def store_state_with_offsets(case: BenchCase, render_one) -> list[str]:
    return [render_one(index) for index in range(case.state_words)]


def load_state_with_offsets(case: BenchCase, render_one) -> list[str]:
    return [render_one(index) for index in range(case.state_words)]


def render_scalar_call(case: BenchCase) -> list[str]:
    ret_lines = ", ".join(state_reg(index) for index in range(case.state_words))
    arg_lines = ", ".join(f"arg_state_{index}" for index in range(case.state_words))
    return [
        "    call.uni (",
        f"        {ret_lines}",
        "    ), helper_value_params, (",
        f"        {arg_lines}",
        "    );",
    ]


def final_store_sequence(case: BenchCase) -> list[str]:
    if case.use_mode == "hot4_dead":
        indices = range(HOT_WORDS)
    else:
        # Caller always observes the full state vector so that `hot4`
        # keeps the cold state words semantically live as passthrough values.
        indices = range(case.state_words)
    lines = ["    mov.u32 %r2, 0;"]
    for index in indices:
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
        raise SystemExit(
            f"command failed ({proc.returncode}): {' '.join(argv)}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    return proc.stdout + proc.stderr


def parse_ptxas_info(text: str) -> tuple[int, int, int, int, int]:
    stack_matches = PTXAS_STACK_RE.findall(text)
    reg_match = PTXAS_REG_RE.search(text)
    lmem_match = PTXAS_LMEM_RE.search(text)
    if reg_match is None:
        raise SystemExit(f"failed to parse ptxas output:\n{text}")
    if not stack_matches:
        stack_frame = 0
        spill_stores = 0
        spill_loads = 0
    else:
        stack_frame = int(stack_matches[0][0])
        spill_stores = sum(int(match[1]) for match in stack_matches)
        spill_loads = sum(int(match[2]) for match in stack_matches)
    registers = int(reg_match.group(1))
    lmem_bytes = 0 if lmem_match is None or lmem_match.group(1) is None else int(lmem_match.group(1))
    return registers, lmem_bytes, stack_frame, spill_stores, spill_loads


def count_sass_features(text: str) -> tuple[int, int, int]:
    mnemonics = extract_sass_mnemonics(text)
    pret_count = sum(1 for mnemonic in mnemonics if mnemonic == "PRET")
    ldl_count = sum(1 for mnemonic in mnemonics if mnemonic.startswith("LDL"))
    stl_count = sum(1 for mnemonic in mnemonics if mnemonic.startswith("STL"))
    return pret_count, ldl_count, stl_count


def extract_sass_mnemonics(text: str) -> list[str]:
    return [match.group(1) for match in SASS_MNEMONIC_RE.finditer(text)]


def write_call_windows(case: BenchCase, sass_text: str, out_dir: Path) -> tuple[int, int]:
    lines = sass_text.splitlines()
    call_indices = [index for index, line in enumerate(lines) if "CALL." in line or re.search(r"\bCALL\b", line)]
    total_ldl_stl = 0
    for window_id, call_index in enumerate(call_indices):
        start = max(0, call_index - CALL_WINDOW_RADIUS)
        end = min(len(lines), call_index + CALL_WINDOW_RADIUS + 1)
        window_lines = lines[start:end]
        window_text = "\n".join(window_lines) + "\n"
        (out_dir / f"{case.name}.call{window_id}.txt").write_text(window_text, encoding="utf-8")
        total_ldl_stl += sum(1 for line in window_lines if " LDL" in f" {line}" or " STL" in f" {line}")
    return total_ldl_stl, len(call_indices)


def write_summary(results: list[CaseResult], out_dir: Path, arch: str, ptxas_opt_level: str) -> None:
    csv_path = out_dir / "summary.csv"
    with csv_path.open("w", encoding="utf-8", newline="") as fp:
        writer = csv.writer(fp)
        writer.writerow(
            [
                "case",
                "abi",
                "state_words",
                "use_mode",
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
                "spill_stores_total_bytes",
                "spill_loads_total_bytes",
                "call_count",
                "pret_count",
                "ldl_count",
                "stl_count",
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
                    result.case.state_words,
                    result.case.use_mode,
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
                    result.spill_stores,
                    result.spill_loads,
                    result.call_count,
                    result.pret_count,
                    result.ldl_count,
                    result.stl_count,
                    result.call_window_ldl_stl,
                    result.ptx_path.as_posix(),
                    result.sass_path.as_posix(),
                ]
            )
    write_report(results, out_dir / "REPORT.md", arch, ptxas_opt_level)


def write_report(results: list[CaseResult], report_path: Path, arch: str, ptxas_opt_level: str) -> None:
    lines = [
        "# PTX Direct Call 参数 ABI 实验汇总",
        "",
        f"- `arch`: `{arch}`",
        f"- `ptxas_opt_level`: `{ptxas_opt_level}`",
        "",
        "## 结果表",
        "",
        "| case | regs | lmem | stack | spill_total(st/ld bytes) | calls | LDL/STL | call-window LDL/STL | PTX bytes | PTX local(ld/st) | PTX param(ld/st/defs) |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for result in results:
        spill = f"{result.spill_stores}/{result.spill_loads}"
        local_ops = f"{result.ptx_ld_local}/{result.ptx_st_local}"
        param_ops = f"{result.ptx_ld_param}/{result.ptx_st_param}/{result.ptx_param_defs}"
        ldl_stl = result.ldl_count + result.stl_count
        lines.append(
            f"| `{result.case.name}` | {result.registers} | {result.lmem_bytes} | {result.stack_frame} | {spill} | {result.call_count} | "
            f"{ldl_stl} | {result.call_window_ldl_stl} | {result.ptx_bytes} | {local_ops} | {param_ops} |"
        )
    lines.extend(
        [
            "",
            "## 解读提示",
            "",
            "- `spill_total(st/ld bytes)` 是 caller + callee 全部函数属性块的 spill 字节数之和，不再只看 entry。",
            "- `LDL/STL` 是整份 SASS 的 local 指令条数，因此它会包含 spill，也会包含显式 local 访存与 ABI scratch。",
            "- `call-window LDL/STL` 比总 `LDL/STL` 更接近“call 边界附近的 local-memory 型搬运成本”，但不能代表全部 marshalling 成本。",
            "- `hot4` 重点看 `.param` 方案是否能压掉大接口中的冷状态；`full` 重点看 worst-case 是否仍优于 `vctx`。",
            "",
        ]
    )
    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


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
    write_summary(results, paths["root"], args.arch, args.ptxas_opt_level)
    print(f"wrote {paths['root'] / 'summary.csv'}")
    print(f"wrote {paths['root'] / 'REPORT.md'}")


if __name__ == "__main__":
    main()
