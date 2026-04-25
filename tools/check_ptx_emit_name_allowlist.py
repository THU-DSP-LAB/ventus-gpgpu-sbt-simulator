#!/usr/bin/env python3
"""
背景
- `reduce-lowering-name-dependence` 与后续 `modularize-ptx-emit-lowering` change 要求 emitter correctness path 不再以 `DecodedInst.name` 作为 lowering authority。

需求/作用
- 静态检查 emitter lowering 文件集中的 `.name` 读取位置，确保它们只用于显式批准的 comment / diagnostic 场景。

用法
- 直接运行：`python3 tools/check_ptx_emit_name_allowlist.py`

实现原理/处理步骤
- 逐行扫描 `sbt/ptx_emit*.cpp` 与 `sbt/ptx_emit_internal.hpp` 中的 `.name` 使用。
- 若发现比较、前后缀匹配、搜索等 authority-like 用法，立即报错。
- 对剩余 `.name` 读取做 allowlist 校验，只允许 comment / EmitError detail / scalar-exec diagnostic 相关位置保留。
"""

from __future__ import annotations

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
EMITTER_FILES = (
    ROOT / "sbt" / "ptx_emit.cpp",
    ROOT / "sbt" / "ptx_emit_internal.hpp",
    ROOT / "sbt" / "ptx_emit_control.cpp",
    ROOT / "sbt" / "ptx_emit_scalar.cpp",
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

    if errors:
        for err in errors:
            print(err, file=sys.stderr)
        return 1

    print("ok ptx emit name allowlist")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
