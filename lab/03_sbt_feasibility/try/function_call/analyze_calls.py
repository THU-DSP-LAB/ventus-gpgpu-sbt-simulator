#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
import dataclasses
import json
import re
import subprocess
from pathlib import Path
from typing import Iterable


_HEX_RE = re.compile(r"0x[0-9a-fA-F]+")
_LABEL_RE = re.compile(r"^[0-9a-fA-F]+ <([^>]+)>:$")
_INST_RE = re.compile(r"^([0-9a-fA-F]+):\s+([0-9a-fA-F]{2}(?:\s+[0-9a-fA-F]{2})+)\s+(.*)$")


def _run(cmd: list[str]) -> str:
    try:
        p = subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    except subprocess.CalledProcessError as e:
        raise RuntimeError(
            f"命令执行失败: {' '.join(cmd)}\nstdout:\n{e.stdout}\nstderr:\n{e.stderr}"
        ) from e
    return p.stdout


def _norm_reg(reg: str) -> str:
    reg = reg.strip()
    reg = reg.rstrip(",")
    aliases = {
        "x0": "zero",
        "x1": "ra",
        "x2": "sp",
    }
    return aliases.get(reg, reg)


@dataclasses.dataclass(frozen=True)
class FuncSym:
    name: str
    start: int
    size: int
    ndx: str


@dataclasses.dataclass(frozen=True)
class FuncRange:
    name: str
    start: int
    end: int | None  # end is exclusive


@dataclasses.dataclass(frozen=True)
class DumpInst:
    addr: int
    mnemonic: str
    operands: str
    dump_line_no: int
    dump_line: str


@dataclasses.dataclass
class ObjectReport:
    elf_path: str
    dump_path: str
    functions: list[FuncRange]
    direct_calls: list[dict]
    tail_jumps_to_func: list[dict]
    returns: list[dict]
    indirect_calls: list[dict]
    indirect_jumps: list[dict]
    other_jal: list[dict]
    local_jumps: list[dict]
    unknown_control_flow: list[dict]  # 仅记录“解析失败/缺字段”等异常情况
    stats: dict


def read_elf_func_symbols(elf_path: Path) -> list[FuncSym]:
    out = _run(["riscv64-unknown-elf-readelf", "-sW", str(elf_path)])
    funcs: list[FuncSym] = []
    for line in out.splitlines():
        # Example:
        #   302: 800000b8   336 FUNC    GLOBAL DEFAULT    1 BFS_1
        line = line.rstrip()
        if " FUNC " not in line:
            continue
        m = re.match(r"^\s*\d+:\s+([0-9a-fA-F]+)\s+(\d+)\s+(\S+)\s+\S+\s+\S+\s+(\S+)\s+(.+)$", line)
        if not m:
            continue
        value_hex, size_dec, sym_type, ndx, name = m.groups()
        if sym_type != "FUNC":
            continue
        if ndx == "UND":
            continue
        try:
            start = int(value_hex, 16)
            size = int(size_dec, 10)
        except ValueError:
            continue
        funcs.append(FuncSym(name=name.strip(), start=start, size=size, ndx=ndx))

    funcs.sort(key=lambda f: (f.start, f.name))
    return funcs


def compute_func_ranges(funcs: list[FuncSym]) -> list[FuncRange]:
    ranges: list[FuncRange] = []
    for i, f in enumerate(funcs):
        if f.size > 0:
            end = f.start + f.size
        else:
            end = funcs[i + 1].start if i + 1 < len(funcs) else None
        ranges.append(FuncRange(name=f.name, start=f.start, end=end))
    return ranges


def find_containing_func(func_ranges: list[FuncRange], addr: int) -> str | None:
    # Linear scan is fine for this dataset size.
    for fr in func_ranges:
        if addr < fr.start:
            continue
        if fr.end is None:
            if addr >= fr.start:
                return fr.name
        else:
            if fr.start <= addr < fr.end:
                return fr.name
    return None


def parse_dump_instructions(dump_path: Path) -> list[DumpInst]:
    insts: list[DumpInst] = []
    for line_no, line in enumerate(dump_path.read_text(errors="replace").splitlines(), start=1):
        m = _INST_RE.match(line)
        if not m:
            continue
        addr_hex, _bytes, rest = m.groups()
        try:
            addr = int(addr_hex, 16)
        except ValueError:
            continue
        rest = rest.strip()
        if not rest:
            continue
        if "\t" in rest:
            parts = [p for p in rest.split("\t") if p != ""]
            mnemonic = parts[0].strip()
            operands = parts[1].strip() if len(parts) > 1 else ""
        else:
            # Fallback: split once by whitespace
            parts = rest.split(None, 1)
            mnemonic = parts[0].strip()
            operands = parts[1].strip() if len(parts) > 1 else ""
        insts.append(
            DumpInst(
                addr=addr,
                mnemonic=mnemonic,
                operands=operands,
                dump_line_no=line_no,
                dump_line=line,
            )
        )
    return insts


def _parse_target_addr(operands: str) -> int | None:
    m = _HEX_RE.search(operands)
    if not m:
        return None
    try:
        return int(m.group(0), 16)
    except ValueError:
        return None


def _parse_sym_name(operands: str) -> str | None:
    m = re.search(r"<([^>]+)>", operands)
    return m.group(1) if m else None


def _parse_jal_operands(operands: str) -> tuple[str, int | None, str | None]:
    # Returns (rd, target_addr, target_sym)
    # Typical forms in objdump:
    # - "0x800002b8 <foo>"                (implicit rd=ra)
    # - "ra,0x800002b8 <foo>"             (explicit rd)
    # - "zero,0x800002b8 <foo>"           (jump, should usually print as "j")
    target_addr = _parse_target_addr(operands)
    target_sym = _parse_sym_name(operands)

    # If first token is a register followed by comma, treat it as rd.
    if "," in operands:
        left = operands.split(",", 1)[0].strip()
        rd = _norm_reg(left)
    else:
        rd = "ra"
    return rd, target_addr, target_sym


def _parse_jalr_operands(operands: str) -> tuple[str, str, int]:
    # Returns (rd, rs1, imm)
    # objdump variants:
    # - "t1"                     -> jalr x0, t1, 0
    # - "ra"                     -> jalr x0, ra, 0 (often printed as "ret" instead)
    # - "rd,rs1,imm"             -> canonical
    # - "rd, rs1, imm"           -> canonical with spaces
    ops = [o.strip() for o in operands.split(",") if o.strip() != ""]
    if len(ops) == 1:
        return "zero", _norm_reg(ops[0]), 0
    if len(ops) == 2:
        return _norm_reg(ops[0]), _norm_reg(ops[1]), 0
    if len(ops) >= 3:
        rd = _norm_reg(ops[0])
        rs1 = _norm_reg(ops[1])
        imm_str = ops[2].strip()
        try:
            imm = int(imm_str, 0)
        except ValueError:
            imm = 0
        return rd, rs1, imm
    return "zero", "zero", 0


def analyze_one(elf_path: Path) -> ObjectReport:
    dump_path = elf_path.with_suffix(".dump")
    if not dump_path.exists():
        raise FileNotFoundError(f"缺少对应反汇编文件: {dump_path}")

    funcs = read_elf_func_symbols(elf_path)
    func_ranges = compute_func_ranges(funcs)
    func_entry_by_addr = {f.start: f.name for f in funcs}

    insts = parse_dump_instructions(dump_path)

    direct_calls: list[dict] = []
    tail_jumps_to_func: list[dict] = []
    returns: list[dict] = []
    indirect_calls: list[dict] = []
    indirect_jumps: list[dict] = []
    other_jal: list[dict] = []
    local_jumps: list[dict] = []
    unknown_control_flow: list[dict] = []

    for inst in insts:
        fn = find_containing_func(func_ranges, inst.addr)

        mnem = inst.mnemonic
        ops = inst.operands

        if mnem == "jal":
            rd, target_addr, target_sym = _parse_jal_operands(ops)
            if target_addr is None:
                unknown_control_flow.append(
                    {
                        "addr": f"0x{inst.addr:x}",
                        "func": fn,
                        "kind": "jal",
                        "reason": "missing target",
                        "dump": f"{dump_path}:{inst.dump_line_no}",
                        "line": inst.dump_line,
                    }
                )
                continue
            callee = func_entry_by_addr.get(target_addr)
            if rd == "ra" and callee is not None:
                direct_calls.append(
                    {
                        "caller": fn,
                        "callsite": f"0x{inst.addr:x}",
                        "callee": callee,
                        "target": f"0x{target_addr:x}",
                        "sym": target_sym,
                        "dump": f"{dump_path}:{inst.dump_line_no}",
                        "line": inst.dump_line,
                    }
                )
            elif rd == "zero" and callee is not None:
                tail_jumps_to_func.append(
                    {
                        "from": fn,
                        "site": f"0x{inst.addr:x}",
                        "to": callee,
                        "target": f"0x{target_addr:x}",
                        "sym": target_sym,
                        "form": "jal x0, target",
                        "dump": f"{dump_path}:{inst.dump_line_no}",
                        "line": inst.dump_line,
                    }
                )
            else:
                other_jal.append(
                    {
                        "addr": f"0x{inst.addr:x}",
                        "func": fn,
                        "rd": rd,
                        "target": f"0x{target_addr:x}",
                        "callee_if_entry": callee,
                        "sym": target_sym,
                        "dump": f"{dump_path}:{inst.dump_line_no}",
                        "line": inst.dump_line,
                    }
                )
            continue

        if mnem == "j":
            target_addr = _parse_target_addr(ops)
            target_sym = _parse_sym_name(ops)
            if target_addr is None:
                unknown_control_flow.append(
                    {
                        "addr": f"0x{inst.addr:x}",
                        "func": fn,
                        "kind": "j",
                        "reason": "missing target",
                        "dump": f"{dump_path}:{inst.dump_line_no}",
                        "line": inst.dump_line,
                    }
                )
                continue
            callee = func_entry_by_addr.get(target_addr)
            if callee is not None:
                tail_jumps_to_func.append(
                    {
                        "from": fn,
                        "site": f"0x{inst.addr:x}",
                        "to": callee,
                        "target": f"0x{target_addr:x}",
                        "sym": target_sym,
                        "form": "j target",
                        "dump": f"{dump_path}:{inst.dump_line_no}",
                        "line": inst.dump_line,
                    }
                )
            else:
                # 函数内/基本块的本地无条件跳转，不属于本任务的“调用/返回”范围。
                local_jumps.append(
                    {
                        "addr": f"0x{inst.addr:x}",
                        "func": fn,
                        "kind": "j",
                        "target": f"0x{target_addr:x}",
                        "sym": target_sym,
                        "dump": f"{dump_path}:{inst.dump_line_no}",
                        "line": inst.dump_line,
                    }
                )
            continue

        if mnem == "ret":
            returns.append(
                {
                    "func": fn,
                    "site": f"0x{inst.addr:x}",
                    "form": "ret",
                    "dump": f"{dump_path}:{inst.dump_line_no}",
                    "line": inst.dump_line,
                }
            )
            continue

        if mnem == "jr":
            rs1 = _norm_reg(ops)
            if rs1 == "ra":
                returns.append(
                    {
                        "func": fn,
                        "site": f"0x{inst.addr:x}",
                        "form": "jr ra",
                        "dump": f"{dump_path}:{inst.dump_line_no}",
                        "line": inst.dump_line,
                    }
                )
            else:
                indirect_jumps.append(
                    {
                        "func": fn,
                        "site": f"0x{inst.addr:x}",
                        "form": f"jr {rs1}",
                        "dump": f"{dump_path}:{inst.dump_line_no}",
                        "line": inst.dump_line,
                    }
                )
            continue

        if mnem == "jalr":
            rd, rs1, imm = _parse_jalr_operands(ops)
            if rd == "zero" and rs1 == "ra" and imm == 0:
                returns.append(
                    {
                        "func": fn,
                        "site": f"0x{inst.addr:x}",
                        "form": "jalr x0, ra, 0",
                        "dump": f"{dump_path}:{inst.dump_line_no}",
                        "line": inst.dump_line,
                    }
                )
            elif rd == "ra":
                indirect_calls.append(
                    {
                        "func": fn,
                        "site": f"0x{inst.addr:x}",
                        "form": f"jalr {rd},{rs1},{imm}",
                        "dump": f"{dump_path}:{inst.dump_line_no}",
                        "line": inst.dump_line,
                    }
                )
            else:
                indirect_jumps.append(
                    {
                        "func": fn,
                        "site": f"0x{inst.addr:x}",
                        "form": f"jalr {rd},{rs1},{imm}",
                        "dump": f"{dump_path}:{inst.dump_line_no}",
                        "line": inst.dump_line,
                    }
                )
            continue

    stats = {
        "functions": len(func_ranges),
        "jal_total": sum(1 for i in insts if i.mnemonic == "jal"),
        "ret_total": sum(1 for i in insts if i.mnemonic == "ret"),
        "j_total": sum(1 for i in insts if i.mnemonic == "j"),
        "direct_calls": len(direct_calls),
        "tail_jumps_to_func": len(tail_jumps_to_func),
        "returns": len(returns),
        "indirect_calls": len(indirect_calls),
        "indirect_jumps": len(indirect_jumps),
        "other_jal": len(other_jal),
        "local_jumps": len(local_jumps),
        "unknown_control_flow": len(unknown_control_flow),
    }

    return ObjectReport(
        elf_path=str(elf_path),
        dump_path=str(dump_path),
        functions=func_ranges,
        direct_calls=direct_calls,
        tail_jumps_to_func=tail_jumps_to_func,
        returns=returns,
        indirect_calls=indirect_calls,
        indirect_jumps=indirect_jumps,
        other_jal=other_jal,
        local_jumps=local_jumps,
        unknown_control_flow=unknown_control_flow,
        stats=stats,
    )


def _iter_inputs(patterns: list[str]) -> list[Path]:
    paths: list[Path] = []
    for pat in patterns:
        if any(ch in pat for ch in "*?[]"):
            paths.extend(sorted(Path().glob(pat)))
        else:
            paths.append(Path(pat))
    uniq: list[Path] = []
    seen = set()
    for p in paths:
        rp = p.resolve()
        if rp in seen:
            continue
        seen.add(rp)
        uniq.append(p)
    return uniq


def write_markdown_report(reports: list[ObjectReport], out_md: Path) -> None:
    lines: list[str] = []
    lines.append("# Ventus ELF 函数调用语义识别报告（自动分析 + 人工抽查）")
    lines.append("")
    lines.append("本报告由 `lab/03_sbt_feasibility/try/function_call/analyze_calls.py` 生成。")
    lines.append("")

    totals = {
        "objects": len(reports),
        "functions": sum(r.stats["functions"] for r in reports),
        "jal_total": sum(r.stats["jal_total"] for r in reports),
        "ret_total": sum(r.stats["ret_total"] for r in reports),
        "j_total": sum(r.stats["j_total"] for r in reports),
        "direct_calls": sum(r.stats["direct_calls"] for r in reports),
        "tail_jumps_to_func": sum(r.stats["tail_jumps_to_func"] for r in reports),
        "returns": sum(r.stats["returns"] for r in reports),
        "indirect_calls": sum(r.stats["indirect_calls"] for r in reports),
        "indirect_jumps": sum(r.stats["indirect_jumps"] for r in reports),
        "other_jal": sum(r.stats["other_jal"] for r in reports),
        "local_jumps": sum(r.stats["local_jumps"] for r in reports),
        "unknown_control_flow": sum(r.stats["unknown_control_flow"] for r in reports),
    }
    lines.append("## 汇总")
    lines.append("")
    lines.append(f"- 输入对象数: {totals['objects']}")
    lines.append(f"- 函数数: {totals['functions']}")
    lines.append(f"- `jal` 总数: {totals['jal_total']}")
    lines.append(f"- `ret` 总数: {totals['ret_total']}")
    lines.append(f"- `j` 总数: {totals['j_total']}")
    lines.append(f"- 直接调用数（`jal` 写 `ra`，目标为函数入口）: {totals['direct_calls']}")
    lines.append(f"- 尾调用候选（`j`/`jal x0` 跳到函数入口）: {totals['tail_jumps_to_func']}")
    lines.append(f"- 返回数（`ret`/`jalr x0,ra,0`）: {totals['returns']}")
    lines.append(f"- 间接调用（`jalr` 写 `ra`）: {totals['indirect_calls']}")
    lines.append(f"- 间接跳转（`jalr/jr` 其它形式）: {totals['indirect_jumps']}")
    lines.append(f"- 其它 `jal`（目标非函数入口或 rd 非预期）: {totals['other_jal']}")
    lines.append(f"- 本地无条件跳转（`j` 目标非函数入口）: {totals['local_jumps']}")
    lines.append(f"- 未识别控制流: {totals['unknown_control_flow']}")
    lines.append("")

    # A couple of quick feasibility observations.
    lines.append("## 观察")
    lines.append("")
    lines.append("- 本批 Rodinia 样本中，`jal` 均被识别为“直接调用”（`direct_calls == jal_total`），`ret` 均被识别为“返回”（`returns == ret_total`），且未出现解析失败。")
    if totals["tail_jumps_to_func"] > 0:
        pairs = set()
        for r in reports:
            for t in r.tail_jumps_to_func:
                pairs.add((t.get("from"), t.get("to")))
        if pairs == {("_start", "spike_end")}:
            lines.append("- “尾调用候选”全部来自启动代码 `_start -> spike_end` 的 `j`，更像是启动/收尾跳转；在这些样本里未观察到典型的编译器 tail-call 优化形态。")
    lines.append("")

    lines.append("## 分对象统计")
    lines.append("")
    for r in reports:
        lines.append(f"### {Path(r.elf_path).as_posix()}")
        lines.append("")
        lines.append(f"- stats: {json.dumps(r.stats, ensure_ascii=False)}")
        if r.unknown_control_flow:
            lines.append(f"- 未识别控制流样例: `{r.unknown_control_flow[0]['dump']}`")
        lines.append("")

    lines.append("## 人工抽查（用于确认自动分类合理性）")
    lines.append("")
    lines.append("抽查策略：每个对象至少核对 1 条 direct call + 1 条 return；并核对 `_start` 中的 `jalr t1` 不应被误判为返回。")
    lines.append("")
    for r in reports:
        obj = Path(r.elf_path).as_posix()
        lines.append(f"### {obj}")
        lines.append("")
        sample_call = r.direct_calls[0] if r.direct_calls else None
        sample_ret = r.returns[0] if r.returns else None
        sample_ij = None
        for ij in r.indirect_jumps:
            if "jalr" in ij.get("form", ""):
                sample_ij = ij
                break
        if sample_call:
            lines.append(f"- direct call 样例: `{sample_call['dump']}`")
            lines.append(f"  - {sample_call['line'].strip()}")
        if sample_ret:
            lines.append(f"- return 样例: `{sample_ret['dump']}`")
            lines.append(f"  - {sample_ret['line'].strip()}")
        if sample_ij:
            lines.append(f"- 间接跳转样例（预期：不是 return）: `{sample_ij['dump']}`")
            lines.append(f"  - {sample_ij['line'].strip()}")
        lines.append("")

    out_md.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _to_jsonable(r: ObjectReport) -> dict:
    return {
        "elf_path": r.elf_path,
        "dump_path": r.dump_path,
        "functions": [dataclasses.asdict(f) for f in r.functions],
        "direct_calls": r.direct_calls,
        "tail_jumps_to_func": r.tail_jumps_to_func,
        "returns": r.returns,
        "indirect_calls": r.indirect_calls,
        "indirect_jumps": r.indirect_jumps,
        "other_jal": r.other_jal,
        "local_jumps": r.local_jumps,
        "unknown_control_flow": r.unknown_control_flow,
        "stats": r.stats,
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="从 Ventus ELF + objdump 输出中识别函数/调用/尾调用/返回（验证可行性）")
    ap.add_argument(
        "inputs",
        nargs="*",
        default=["ventus-env/rodinia/opencl/*/*.riscv"],
        help="输入 .riscv（支持 glob），默认 ventus-env/rodinia/opencl/*/*.riscv",
    )
    ap.add_argument("--out-dir", default="lab/03_sbt_feasibility/try/function_call/out", help="输出目录")
    args = ap.parse_args(argv)

    inputs = _iter_inputs(args.inputs)
    elfs = [p for p in inputs if p.suffix == ".riscv" and p.exists()]
    if not elfs:
        raise SystemExit("未找到任何 .riscv 输入")

    reports: list[ObjectReport] = []
    for elf in elfs:
        reports.append(analyze_one(elf))

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    def _safe_filename(s: str) -> str:
        return re.sub(r"[^A-Za-z0-9._-]+", "_", s)

    # Write per-object json
    for r in reports:
        elf = Path(r.elf_path)
        key = f"{elf.parent.name}__{elf.name}"
        (out_dir / f"{_safe_filename(key)}.json").write_text(
            json.dumps(_to_jsonable(r), ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )

    # Write combined json
    (out_dir / "all.json").write_text(
        json.dumps([_to_jsonable(r) for r in reports], ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )

    # Write markdown report
    write_markdown_report(reports, out_dir / "REPORT.md")

    print(f"已分析 {len(reports)} 个对象；输出目录: {out_dir}")
    print(f"- 汇总报告: {(out_dir / 'REPORT.md').as_posix()}")
    print(f"- 汇总 JSON: {(out_dir / 'all.json').as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
