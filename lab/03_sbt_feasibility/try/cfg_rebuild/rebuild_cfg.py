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
# `.dump`（objdump 反汇编）里 raw bytes 通常是按字节两位十六进制分组输出：`5b 30 c3 0e`
_INST_RE = re.compile(r"^([0-9a-fA-F]+):\s+([0-9a-fA-F]{2}(?:\s+[0-9a-fA-F]{2})+)\s+(.*)$")


def _run(cmd: list[str]) -> str:
    try:
        p = subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    except subprocess.CalledProcessError as e:
        raise RuntimeError(
            f"命令执行失败: {' '.join(cmd)}\nstdout:\n{e.stdout}\nstderr:\n{e.stderr}"
        ) from e
    return p.stdout


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


def _parse_target_addr(operands: str) -> int | None:
    m = _HEX_RE.search(operands)
    if not m:
        return None
    try:
        return int(m.group(0), 16)
    except ValueError:
        return None


def _norm_reg(reg: str) -> str:
    reg = reg.strip().rstrip(",")
    aliases = {
        "x0": "zero",
        "x1": "ra",
        "x2": "sp",
    }
    return aliases.get(reg, reg)


def _parse_jal_operands(operands: str) -> tuple[str, int | None]:
    # Returns (rd, target_addr)
    # objdump variants:
    # - "0x800002b8 <foo>"                (implicit rd=ra)
    # - "ra,0x800002b8 <foo>"             (explicit rd)
    # - "zero,0x800002b8 <foo>"           (jump)
    target_addr = _parse_target_addr(operands)
    if "," in operands:
        left = operands.split(",", 1)[0].strip()
        rd = _norm_reg(left)
    else:
        rd = "ra"
    return rd, target_addr


def _parse_jalr_operands(operands: str) -> tuple[str, str, int]:
    # Returns (rd, rs1, imm)
    # objdump variants:
    # - "t1"                     -> jalr x0, t1, 0
    # - "rd,rs1,imm"             -> canonical
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


def _is_cond_branch_mnemonic(mnemonic: str) -> bool:
    return mnemonic in {"beq", "bne", "blt", "bge", "bltu", "bgeu"}


def _is_vbranch_mnemonic(mnemonic: str) -> bool:
    # Ventus vbranch family: vbeq/vbne/vblt/vbge/...
    return mnemonic.startswith("vb") and mnemonic not in {"vbarrier"}


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
class Inst:
    addr: int
    mnemonic: str
    operands: str
    line_no: int
    line: str


@dataclasses.dataclass(frozen=True)
class Edge:
    src: int
    dst: int
    kind: str  # fallthrough/branch/jump


@dataclasses.dataclass
class BasicBlock:
    start: int
    insts: list[Inst]
    succs: list[Edge]


@dataclasses.dataclass(frozen=True)
class VBranchCheck:
    func: str
    vbranch_addr: int
    vbranch_block: int
    mnemonic: str
    target: int | None
    fallthrough: int | None
    join_pc: int | None
    join_is_join_inst: bool
    loop_like: bool
    postdom_ok: bool
    no_side_exit_ok: bool
    single_entry_ok: bool
    error: str | None


def read_elf_func_symbols(elf_path: Path) -> list[FuncSym]:
    out = _run(["riscv64-unknown-elf-readelf", "-sW", str(elf_path)])
    funcs: list[FuncSym] = []
    for line in out.splitlines():
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


def parse_dump_instructions(dump_path: Path) -> list[Inst]:
    insts: list[Inst] = []
    for line_no, line in enumerate(dump_path.read_text(errors="replace").splitlines(), start=1):
        if _LABEL_RE.match(line.strip()):
            continue
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
            parts = rest.split(None, 1)
            mnemonic = parts[0].strip()
            operands = parts[1].strip() if len(parts) > 1 else ""
        insts.append(Inst(addr=addr, mnemonic=mnemonic, operands=operands, line_no=line_no, line=line))
    insts.sort(key=lambda i: i.addr)
    return insts


def disassemble_text(elf_path: Path) -> list[Inst]:
    """
    说明：
    - 本实验优先使用与 ELF 同名的 `.dump`（仓库内 Rodinia 样本已提供），以获得 Ventus 自定义指令的助记符。
    - 若缺少 `.dump`，当前实现将直接报错（可按需扩展为“从 ELF 直接解码”）。
    """
    dump_path = elf_path.with_suffix(".dump")
    if not dump_path.exists():
        raise FileNotFoundError(f"缺少对应反汇编文件: {dump_path}（当前实现需要它来识别 Ventus 自定义指令）")
    return parse_dump_instructions(dump_path)


def _slice_func_insts(all_insts: list[Inst], fr: FuncRange) -> list[Inst]:
    if fr.end is None:
        return [i for i in all_insts if i.addr >= fr.start]
    return [i for i in all_insts if fr.start <= i.addr < fr.end]


def _classify_control_flow(inst: Inst) -> tuple[str, int | None, bool, bool]:
    """
    Returns (kind, target, has_fallthrough, is_terminator).
    - kind: branch/jump/ret/end/indirect/none
    """
    mnem = inst.mnemonic
    ops = inst.operands

    if mnem == "endprg":
        return "end", None, False, True
    if mnem == "ret":
        return "ret", None, False, True
    if mnem == "join":
        # 在结构化 CFG 方案中，join 作为 merge label 处理；它本身不是“显式的 CFG 终结指令”。
        return "join", None, True, False

    if mnem == "j":
        return "jump", _parse_target_addr(ops), False, True

    if mnem == "jal":
        rd, target = _parse_jal_operands(ops)
        if rd == "zero":
            return "jump", target, False, True
        # rd==ra 的常见情形视作 call（不在本脚本内建跨函数 CFG）
        return "call", target, True, False

    if mnem == "jalr":
        rd, rs1, imm = _parse_jalr_operands(ops)
        if rd == "zero" and rs1 == "ra" and imm == 0:
            return "ret", None, False, True
        return "indirect", None, False, True

    if _is_cond_branch_mnemonic(mnem) or _is_vbranch_mnemonic(mnem):
        return "branch", _parse_target_addr(ops), True, True

    # 某些工具链可能输出 bnez/beqz 等伪指令；保守按“有目标地址 -> 条件分支”处理。
    if mnem.startswith("b") and _parse_target_addr(ops) is not None:
        return "branch", _parse_target_addr(ops), True, True

    return "none", None, True, False


def _compute_leaders(func_insts: list[Inst], func_start: int, func_end: int | None) -> set[int]:
    if not func_insts:
        return set()
    leaders: set[int] = {func_insts[0].addr, func_start}
    addr_set = {i.addr for i in func_insts}

    for idx, inst in enumerate(func_insts):
        kind, target, has_fallthrough, is_term = _classify_control_flow(inst)
        if inst.mnemonic == "join":
            leaders.add(inst.addr)
        if target is not None:
            if (func_end is None and target >= func_start) or (func_end is not None and func_start <= target < func_end):
                if target in addr_set:
                    leaders.add(target)
        if is_term and idx + 1 < len(func_insts):
            leaders.add(func_insts[idx + 1].addr)
        # 非 terminator 的 fallthrough 不需要额外加 leader；否则会退化为“每条指令一个块”。

    # 过滤掉不在本函数指令集合中的 leader（例如落在 gap 上）
    return {a for a in leaders if a in addr_set}


def _build_basic_blocks(func_insts: list[Inst], leaders: set[int]) -> dict[int, BasicBlock]:
    blocks: dict[int, BasicBlock] = {}
    if not func_insts:
        return blocks
    leader_set = set(leaders)

    current_start = func_insts[0].addr
    current: list[Inst] = []

    def _flush() -> None:
        nonlocal current_start, current
        if not current:
            return
        blocks[current_start] = BasicBlock(start=current_start, insts=current, succs=[])
        current = []

    for idx, inst in enumerate(func_insts):
        if inst.addr in leader_set and current and inst.addr != current_start:
            _flush()
            current_start = inst.addr
        current.append(inst)

        _kind, _target, _has_fallthrough, is_term = _classify_control_flow(inst)
        if is_term:
            _flush()
            if idx + 1 < len(func_insts):
                current_start = func_insts[idx + 1].addr

    _flush()
    return blocks


def _build_cfg_edges(blocks: dict[int, BasicBlock]) -> None:
    if not blocks:
        return
    # Map instruction addr -> block start
    inst_to_block: dict[int, int] = {}
    all_insts: list[Inst] = []
    for b in sorted(blocks.values(), key=lambda bb: bb.start):
        for inst in b.insts:
            inst_to_block[inst.addr] = b.start
            all_insts.append(inst)
    all_insts.sort(key=lambda i: i.addr)
    next_inst: dict[int, int] = {}
    for i, inst in enumerate(all_insts[:-1]):
        next_inst[inst.addr] = all_insts[i + 1].addr

    for b in blocks.values():
        last = b.insts[-1]
        kind, target, _has_fallthrough, is_term = _classify_control_flow(last)
        succs: list[Edge] = []

        def _add_edge(dst_addr: int | None, ekind: str) -> None:
            if dst_addr is None:
                return
            dst_block = inst_to_block.get(dst_addr)
            if dst_block is None:
                return
            succs.append(Edge(src=b.start, dst=dst_block, kind=ekind))

        if kind == "branch":
            _add_edge(target, "branch")
            ft = next_inst.get(last.addr)
            _add_edge(ft, "fallthrough")
        elif kind == "jump":
            _add_edge(target, "jump")
        elif kind in {"ret", "end", "indirect"}:
            pass
        else:
            # 普通块末尾默认 fallthrough
            ft = next_inst.get(last.addr)
            _add_edge(ft, "fallthrough")

        b.succs = succs


def _find_inst_by_addr(func_insts: list[Inst]) -> dict[int, Inst]:
    return {i.addr: i for i in func_insts}


def _parse_auipc(inst: Inst) -> tuple[str, int] | None:
    # auipc rd, imm
    if inst.mnemonic != "auipc":
        return None
    ops = [o.strip() for o in inst.operands.split(",") if o.strip() != ""]
    if len(ops) < 2:
        return None
    rd = _norm_reg(ops[0])
    try:
        imm = int(ops[1], 0)
    except ValueError:
        return None
    return rd, imm


def _parse_setrpc(inst: Inst) -> tuple[str, str, int] | None:
    if inst.mnemonic != "setrpc":
        return None
    ops = [o.strip() for o in inst.operands.split(",") if o.strip() != ""]
    if len(ops) < 3:
        return None
    rd = _norm_reg(ops[0])
    rs1 = _norm_reg(ops[1])
    try:
        off = int(ops[2], 0)
    except ValueError:
        return None
    return rd, rs1, off


def _resolve_setrpc_join_pc(func_insts: list[Inst], idx: int) -> int | None:
    """仅实现原型阶段假设的典型模式：auipc <r>, imm + setrpc ..., <r>, off（近邻）"""
    setrpc = _parse_setrpc(func_insts[idx])
    if setrpc is None:
        return None
    _rd, rs1, off = setrpc

    # 向前找最近一次对 rs1 的 auipc 定义
    for j in range(idx - 1, max(-1, idx - 12), -1):
        au = _parse_auipc(func_insts[j])
        if au is None:
            continue
        rd, imm = au
        if rd != rs1:
            continue
        base = func_insts[j].addr + (imm << 12)
        return base + off
    return None


def _collect_succs(blocks: dict[int, BasicBlock]) -> dict[int, set[int]]:
    succs: dict[int, set[int]] = {b.start: set() for b in blocks.values()}
    for b in blocks.values():
        for e in b.succs:
            succs[b.start].add(e.dst)
    return succs


def _collect_preds(succs: dict[int, set[int]]) -> dict[int, set[int]]:
    preds: dict[int, set[int]] = {n: set() for n in succs.keys()}
    for n, ss in succs.items():
        for s in ss:
            preds.setdefault(s, set()).add(n)
    return preds


def _compute_postdominators(succs: dict[int, set[int]]) -> dict[int, set[int]]:
    EXIT = -1
    nodes = set(succs.keys()) | {EXIT}
    succs2: dict[int, set[int]] = {n: set(ss) for n, ss in succs.items()}
    succs2[EXIT] = set()

    postdom: dict[int, set[int]] = {n: set(nodes) for n in nodes}
    postdom[EXIT] = {EXIT}

    changed = True
    while changed:
        changed = False
        for n in sorted(nodes):
            if n == EXIT:
                continue
            ss = succs2.get(n, set())
            if not ss:
                ss = {EXIT}
            # intersection of postdom of successors
            it = None
            for s in ss:
                if it is None:
                    it = set(postdom[s])
                else:
                    it &= postdom[s]
            if it is None:
                it = {EXIT}
            new = {n} | it
            if new != postdom[n]:
                postdom[n] = new
                changed = True
    return postdom


def _compute_dominators(entry: int, preds: dict[int, set[int]], nodes: set[int]) -> dict[int, set[int]]:
    dom: dict[int, set[int]] = {n: set(nodes) for n in nodes}
    dom[entry] = {entry}

    changed = True
    while changed:
        changed = False
        for n in sorted(nodes):
            if n == entry:
                continue
            ps = preds.get(n, set())
            if not ps:
                new = {n}
            else:
                it = None
                for p in ps:
                    if it is None:
                        it = set(dom[p])
                    else:
                        it &= dom[p]
                if it is None:
                    it = set()
                new = {n} | it
            if new != dom[n]:
                dom[n] = new
                changed = True
    return dom


def _emit_dot(blocks: dict[int, BasicBlock], out_path: Path, title: str) -> None:
    def _n(a: int) -> str:
        return f"n{a:x}"

    lines: list[str] = []
    lines.append("digraph CFG {")
    lines.append('  graph [fontname="monospace"];')
    lines.append('  node [shape=box fontname="monospace"];')
    lines.append('  edge [fontname="monospace"];')
    lines.append(f'  label="{title}";')
    lines.append("  labelloc=t;")

    for b in sorted(blocks.values(), key=lambda bb: bb.start):
        inst_lines = []
        for inst in b.insts[:8]:
            inst_lines.append(f"0x{inst.addr:x}: {inst.mnemonic} {inst.operands}".rstrip())
        if len(b.insts) > 8:
            inst_lines.append("...")
        label = "\\l".join(inst_lines) + "\\l"
        lines.append(f'  {_n(b.start)} [label="{label}"];')

    for b in blocks.values():
        for e in b.succs:
            lines.append(f'  {_n(e.src)} -> {_n(e.dst)} [label="{e.kind}"];')

    lines.append("}")
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def verify_one(elf_path: Path, out_dir: Path, emit_dot: bool, emit_json: bool, verbose: bool) -> dict:
    all_insts = disassemble_text(elf_path)
    funcs = read_elf_func_symbols(elf_path)
    func_ranges = compute_func_ranges(funcs)

    report: dict = {
        "elf": str(elf_path),
        "functions": [],
        "stats": {},
    }

    totals = {
        "funcs": 0,
        "blocks": 0,
        "edges": 0,
        "vbranch": 0,
        "vbranch_ok": 0,
        "vbranch_fail": 0,
        "unsupported_jalr": 0,
    }

    for fr in func_ranges:
        func_insts = _slice_func_insts(all_insts, fr)
        if not func_insts:
            continue
        func_end = fr.end
        leaders = _compute_leaders(func_insts, fr.start, func_end)
        blocks = _build_basic_blocks(func_insts, leaders)
        _build_cfg_edges(blocks)

        succs = _collect_succs(blocks)
        preds = _collect_preds(succs)
        postdom = _compute_postdominators(succs)
        dom = _compute_dominators(min(blocks.keys()), preds, set(succs.keys()))

        inst_to_block: dict[int, int] = {}
        for b in blocks.values():
            for inst in b.insts:
                inst_to_block[inst.addr] = b.start

        inst_by_addr = _find_inst_by_addr(func_insts)

        # 线性扫描解析 setrpc -> CSR_RPC(join_pc) -> vbranch
        current_rpc: int | None = None
        vchecks: list[VBranchCheck] = []
        unsupported_jalr: list[dict] = []

        for idx, inst in enumerate(func_insts):
            if inst.mnemonic == "setrpc":
                join_pc = _resolve_setrpc_join_pc(func_insts, idx)
                current_rpc = join_pc
                continue

            if _is_vbranch_mnemonic(inst.mnemonic):
                kind, target, _has_ft, _is_term = _classify_control_flow(inst)
                assert kind == "branch"
                fallthrough = None
                if idx + 1 < len(func_insts):
                    fallthrough = func_insts[idx + 1].addr

                join_pc = current_rpc
                join_is_join_inst = False
                loop_like = False
                error: str | None = None
                if join_pc is None:
                    error = "无法在 vbranch 处解析 CSR_RPC(join PC)"
                else:
                    join_inst = inst_by_addr.get(join_pc)
                    join_is_join_inst = join_inst is not None and join_inst.mnemonic == "join"
                    if not join_is_join_inst:
                        error = f"join PC=0x{join_pc:x} 处不存在 join 指令"

                vblock = inst_to_block.get(inst.addr, fr.start)
                postdom_ok = False
                no_side_exit_ok = False
                single_entry_ok = False
                if error is None and join_pc is not None:
                    join_block = inst_to_block.get(join_pc)
                    if join_block is None:
                        error = f"join PC=0x{join_pc:x} 未成为基本块入口"
                    else:
                        # loop-like：存在回边（某个后继块经若干纯跳转后可达“支配 vbranch 块”的 header）
                        vdom = dom.get(vblock, set())
                        for s in succs.get(vblock, set()):
                            cur = s
                            for _ in range(8):
                                if cur in vdom:
                                    loop_like = True
                                    break
                                bcur = blocks.get(cur)
                                if bcur is None:
                                    break
                                if len(bcur.succs) == 1 and bcur.succs[0].kind == "jump":
                                    cur = bcur.succs[0].dst
                                    continue
                                break
                            if loop_like:
                                break

                        postdom_ok = join_block in postdom.get(vblock, set())

                        # region: 从 vbranch 块的后继出发，走到 join_block 之前的节点集合
                        region: set[int] = set()
                        work = list(succs.get(vblock, set()))
                        while work:
                            n = work.pop()
                            if n == join_block:
                                continue
                            if n in region:
                                continue
                            region.add(n)
                            for s in succs.get(n, set()):
                                if s == join_block:
                                    continue
                                work.append(s)

                        no_side_exit_ok = True
                        for n in region:
                            for s in succs.get(n, set()):
                                if s == join_block or s in region:
                                    continue
                                no_side_exit_ok = False
                                break
                            if not no_side_exit_ok:
                                break

                        if loop_like:
                            # 对“回边 vbranch”的区域，天然会存在来自 loop preheader 的额外入口；
                            # 原型阶段我们仍认为其可结构化（更接近 while/for 形态），因此跳过单入口约束。
                            single_entry_ok = True
                        else:
                            single_entry_ok = True
                            for n in region:
                                for p in preds.get(n, set()):
                                    if p == vblock or p in region:
                                        continue
                                    single_entry_ok = False
                                    break
                                if not single_entry_ok:
                                    break

                        if not postdom_ok:
                            error = (error + "; " if error else "") + "join 块未后支配 vbranch 块"
                        elif not no_side_exit_ok:
                            error = (error + "; " if error else "") + "分支区域存在侧出口(side exit)"
                        elif (not loop_like) and (not single_entry_ok):
                            error = (error + "; " if error else "") + "分支区域存在多入口(multi-entry)"

                ok = error is None
                vchecks.append(
                    VBranchCheck(
                        func=fr.name,
                        vbranch_addr=inst.addr,
                        vbranch_block=vblock,
                        mnemonic=inst.mnemonic,
                        target=target,
                        fallthrough=fallthrough,
                        join_pc=join_pc,
                        join_is_join_inst=join_is_join_inst,
                        loop_like=loop_like,
                        postdom_ok=postdom_ok,
                        no_side_exit_ok=no_side_exit_ok,
                        single_entry_ok=single_entry_ok,
                        error=None if ok else error,
                    )
                )
                continue

            if inst.mnemonic == "jalr":
                rd, rs1, imm = _parse_jalr_operands(inst.operands)
                if not (rd == "zero" and rs1 == "ra" and imm == 0):
                    unsupported_jalr.append(
                        {
                            "addr": f"0x{inst.addr:x}",
                            "inst": inst.line.strip(),
                        }
                    )

        totals["funcs"] += 1
        totals["blocks"] += len(blocks)
        totals["edges"] += sum(len(b.succs) for b in blocks.values())
        totals["vbranch"] += len(vchecks)
        totals["vbranch_ok"] += sum(1 for v in vchecks if v.error is None)
        totals["vbranch_fail"] += sum(1 for v in vchecks if v.error is not None)
        totals["unsupported_jalr"] += len(unsupported_jalr)

        if emit_dot:
            out_dir.mkdir(parents=True, exist_ok=True)
            safe_name = re.sub(r"[^0-9A-Za-z_]+", "_", fr.name)
            dot_path = out_dir / f"{elf_path.stem}__{safe_name}.dot"
            _emit_dot(blocks, dot_path, title=f"{elf_path.name}:{fr.name}")

        func_entry = {
            "name": fr.name,
            "start": f"0x{fr.start:x}",
            "end": f"0x{fr.end:x}" if fr.end is not None else None,
            "insts": len(func_insts),
            "blocks": len(blocks),
            "edges": sum(len(b.succs) for b in blocks.values()),
            "vbranch": [dataclasses.asdict(v) for v in vchecks],
            "unsupported_jalr": unsupported_jalr,
        }
        report["functions"].append(func_entry)

        if verbose:
            print(
                f"- {elf_path.name}:{fr.name} blocks={len(blocks)} edges={func_entry['edges']} "
                f"vbranch_ok={sum(1 for v in vchecks if v.error is None)}/{len(vchecks)} "
                f"unsupported_jalr={len(unsupported_jalr)}"
            )

    report["stats"] = totals

    if emit_json:
        out_dir.mkdir(parents=True, exist_ok=True)
        out_path = out_dir / f"{elf_path.stem}.cfg_verify.json"
        out_path.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    return report


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="从 Ventus ELF（RISC-V ELF）重建 CFG，并验证 setrpc/vbranch/join 的结构化约束")
    sub = ap.add_subparsers(dest="cmd", required=True)

    ap_v = sub.add_parser("verify", help="构建 CFG 并做静态正确性验证（支持 glob）")
    ap_v.add_argument("inputs", nargs="+", help="输入 ELF 路径或 glob（如 ventus-env/rodinia/opencl/*/object0.riscv）")
    ap_v.add_argument("--out-dir", default="lab/03_sbt_feasibility/try/cfg_rebuild/out", help="输出目录")
    ap_v.add_argument("--emit-dot", action="store_true", help="输出每个函数的 CFG .dot（Graphviz）")
    ap_v.add_argument("--emit-json", action="store_true", help="输出 JSON 报告")
    ap_v.add_argument("--verbose", action="store_true", help="输出每个函数的简要统计")

    args = ap.parse_args(argv)

    if args.cmd == "verify":
        out_dir = Path(args.out_dir)
        inputs = _iter_inputs(args.inputs)
        if not inputs:
            raise SystemExit("未匹配到任何输入文件")
        all_reports = []
        for p in inputs:
            if not p.exists():
                raise SystemExit(f"输入文件不存在: {p}")
            all_reports.append(verify_one(p, out_dir, args.emit_dot, args.emit_json, args.verbose))

        # 汇总打印：只输出失败项（默认），verbose 时会显示每函数统计
        total_v = sum(r["stats"]["vbranch"] for r in all_reports)
        total_ok = sum(r["stats"]["vbranch_ok"] for r in all_reports)
        total_fail = sum(r["stats"]["vbranch_fail"] for r in all_reports)
        total_jalr = sum(r["stats"]["unsupported_jalr"] for r in all_reports)
        print(f"[CFG verify] objects={len(all_reports)} vbranch_ok={total_ok}/{total_v} unsupported_jalr={total_jalr}")

        failures = []
        for r in all_reports:
            for f in r["functions"]:
                for vb in f["vbranch"]:
                    if vb["error"] is not None:
                        failures.append((r["elf"], f["name"], vb))
        if failures:
            print("[失败明细]")
            for elf, fn, vb in failures[:80]:
                print(
                    f"- {Path(elf).name}:{fn} vbranch@0x{vb['vbranch_addr']:x} join={vb['join_pc']} err={vb['error']}"
                )
            if len(failures) > 80:
                print(f"... 还有 {len(failures) - 80} 条失败未展开（可用 --emit-json 查看全量）")
            return 2
        return 0

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
