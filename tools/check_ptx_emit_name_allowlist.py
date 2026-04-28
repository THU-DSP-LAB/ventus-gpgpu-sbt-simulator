#!/usr/bin/env python3
"""
背景
- `reduce-lowering-name-dependence` 与后续 `modularize-ptx-emit-lowering` change 要求 emitter correctness path 不再以 `DecodedInst.name` 作为 lowering authority。

需求/作用
- 静态检查 emitter lowering 文件集中的 `.name` 读取位置，确保它们只用于显式批准的 comment / diagnostic 场景。
- 检查 builtin allowlist 与 inline dispatch 共享同一 lookup source，并且每个 accepted builtin symbol 都有 dispatch path。

用法
- 直接运行：`python3 tools/check_ptx_emit_name_allowlist.py`

实现原理/处理步骤
- 逐行扫描完整 PTX emitter implementation file set 与 `sbt/ptx_emit_internal.hpp` 中的 `.name` 使用。
- 若发现比较、前后缀匹配、搜索等 authority-like 用法，立即报错。
- 对剩余 `.name` 读取做 allowlist 校验，只允许 comment / EmitError detail / scalar-exec diagnostic 相关位置保留。
- 解析共享 builtin semantic table 和 `emit_builtin_call()` switch，确认 public allowlist 与 control
  dispatch 都经由共享 lookup，并且 table 中的每个 `BuiltinKind` 都有 switch case 和 verifier summary。
"""

from __future__ import annotations

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
EMITTER_FILES = (
    ROOT / "sbt" / "ptx_emit.cpp",
    ROOT / "sbt" / "ptx_emit_internal.hpp",
    ROOT / "sbt" / "ptx_emit_core.cpp",
    ROOT / "sbt" / "ptx_emit_runtime.cpp",
    ROOT / "sbt" / "ptx_emit_memory.cpp",
    ROOT / "sbt" / "ptx_emit_call.cpp",
    ROOT / "sbt" / "ptx_emit_builtin.cpp",
    ROOT / "sbt" / "ptx_emit_control.cpp",
    ROOT / "sbt" / "ptx_emit_scalar.cpp",
    ROOT / "sbt" / "ptx_emit_scalar_fp.cpp",
    ROOT / "sbt" / "ptx_emit_vector.cpp",
    ROOT / "sbt" / "ptx_emit_custom.cpp",
    ROOT / "sbt" / "ptx_emit_mma_lowering.cpp",
)

NAME_RE = re.compile(r"\b(?:di|inst|last)\.name\b")
FORBIDDEN_RE = re.compile(r"\.name\b.*(?:==|!=|\.find\(|\.rfind\(|starts_with|ends_with)")

ALLOWED_SNIPPETS = (
    'EmitError("missing.scalar_exec_metadata"',
    'EmitError("invalid.scalar_exec"',
    'EmitError("unsupported.inst"',
    'EmitError("invalid.mma.metadata"',
    'EmitError("unsupported.custom.sfu"',
    'EmitError("invalid.custom.payload"',
    'emit_line("// "',
)

BUILTIN_FILE = ROOT / "sbt" / "ptx_emit_builtin.cpp"
BUILTIN_SEMANTICS_FILE = ROOT / "sbt" / "builtin_semantics.cpp"
CONTROL_FILE = ROOT / "sbt" / "ptx_emit_control.cpp"

BUILTIN_ENTRY_RE = re.compile(r'\{"([^"]+)",\s*BuiltinKind::([A-Za-z0-9_]+)\}')
BUILTIN_CASE_RE = re.compile(r"case\s+BuiltinKind::([A-Za-z0-9_]+)\s*:")
SUMMARY_ENTRY_RE = re.compile(r"\{BuiltinKind::([A-Za-z0-9_]+),\s*k[A-Za-z0-9_]+\}")


def main() -> int:
    errors: list[str] = []

    for path in EMITTER_FILES:
        lines = path.read_text(encoding="utf-8").splitlines()
        for lineno, line in enumerate(lines, start=1):
            if not NAME_RE.search(line):
                continue
            if FORBIDDEN_RE.search(line):
                errors.append(f"{path}:{lineno}: forbidden authority-like name usage: {line.strip()}")
                continue
            if not any(snippet in line for snippet in ALLOWED_SNIPPETS):
                errors.append(f"{path}:{lineno}: name usage missing allowlist category: {line.strip()}")

    builtin_text = BUILTIN_FILE.read_text(encoding="utf-8")
    builtin_semantics_text = BUILTIN_SEMANTICS_FILE.read_text(encoding="utf-8")
    builtin_entries = BUILTIN_ENTRY_RE.findall(builtin_semantics_text)
    if not builtin_entries:
        errors.append(f"{BUILTIN_SEMANTICS_FILE}: builtin lookup table has no parsed entries")
    builtin_names = [name for name, _kind in builtin_entries]
    duplicate_names = sorted({name for name in builtin_names if builtin_names.count(name) > 1})
    for name in duplicate_names:
        errors.append(f"{BUILTIN_SEMANTICS_FILE}: duplicate builtin lookup entry: {name}")

    table_kinds = {kind for _name, kind in builtin_entries}
    dispatch_kinds = set(BUILTIN_CASE_RE.findall(builtin_text))
    missing_dispatch = sorted(table_kinds - dispatch_kinds)
    for kind in missing_dispatch:
        errors.append(f"{BUILTIN_FILE}: builtin lookup kind lacks emit_builtin_call dispatch case: {kind}")

    summary_kinds = set(SUMMARY_ENTRY_RE.findall(builtin_semantics_text))
    missing_summary = sorted(table_kinds - summary_kinds)
    for kind in missing_summary:
        errors.append(f"{BUILTIN_SEMANTICS_FILE}: builtin lookup kind lacks verifier summary: {kind}")
    extra_summary = sorted(summary_kinds - table_kinds)
    for kind in extra_summary:
        errors.append(f"{BUILTIN_SEMANTICS_FILE}: verifier summary kind is not accepted by lookup: {kind}")

    if "return sbt::is_inlined_builtin_call_name(callee);" not in builtin_text:
        errors.append(f"{BUILTIN_FILE}: public is_inlined_builtin_call_name() must delegate to shared lookup")

    control_text = CONTROL_FILE.read_text(encoding="utf-8")
    if "lookup_builtin_call(callee)" not in control_text or "emit_builtin_call(ctx, *builtin, pc)" not in control_text:
        errors.append(f"{CONTROL_FILE}: control direct-call lowering must dispatch builtins through shared lookup")

    if errors:
        for err in errors:
            print(err, file=sys.stderr)
        return 1

    print("ok ptx emit name allowlist")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
