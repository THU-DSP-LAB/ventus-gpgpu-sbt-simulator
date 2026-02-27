#!/usr/bin/env python3
import argparse
import re
import sys
from pathlib import Path


_ALIASES: dict[str, str] = {
    # VentusInst_basic.txt naming -> Spike encoding.h DECLARE_INSN id.
    # Keep this list minimal and auditable.
    "vmandnot_mm": "vmandn_mm",
    "vmornot_mm": "vmorn_mm",
}

_EXTRAS: set[str] = {
    # Keep compile-first smoke (Rodinia/PoCL) stable even if these mnemonics are
    # not listed in VentusInst_basic.txt.
    "vsub12_vi",
    "vsetvli",
    "vlw_v",
    "vsw_v",
}


def _load_declared_insn_ids(encoding_h: Path) -> set[str]:
    text = encoding_h.read_text(encoding="utf-8", errors="ignore")
    out: set[str] = set()
    for m in re.finditer(r"DECLARE_INSN\(\s*([A-Za-z0-9_]+)\s*,", text):
        out.add(m.group(1))
    return out


def _resolve_encoding_id(mnemonic: str, declared: set[str]) -> str:
    if mnemonic in declared:
        return mnemonic
    # Some custom unary vector ops are listed without ".v" suffix in VentusInst_basic.txt.
    if f"{mnemonic}_v" in declared:
        return f"{mnemonic}_v"
    alias = _ALIASES.get(mnemonic)
    if alias and alias in declared:
        return alias
    return mnemonic


def _load_target_pattern_mnemonics(ventusinst: Path) -> set[str]:
    out: set[str] = set()
    lines = ventusinst.read_text(encoding="utf-8", errors="ignore").splitlines()
    for i, line in enumerate(lines):
        if i == 0:
            continue
        if not line.strip():
            continue
        cols = line.split("\t")
        if len(cols) < 2:
            continue
        kind = cols[0].strip()
        if kind not in {"Custom", "V"}:
            continue
        asm = cols[1].strip()
        if not asm:
            continue
        mnem = asm.split()[0].replace(".", "_")
        out.add(mnem)
    return out


def _render_want(ids: list[str]) -> str:
    header = """# Spike DECLARE_INSN IDs used as pattern inputs (single source of truth).
#
# Format:
#   - one insn id per line (underscore naming, matching spike encoding.h)
#   - blank lines and '#' comments are ignored
#
# Scope:
#   - Full pattern coverage for `VentusInst_basic.txt` Custom/V mnemonics.
#   - Plus a small set of bring-up extras required by real ELFs (Rodinia/PoCL).
#
"""
    body = "\n".join(ids) + "\n"
    return header + body


def main() -> int:
    ap = argparse.ArgumentParser(description="Update data/spike_want.txt from VentusInst_basic.txt + Spike encoding.h")
    ap.add_argument("--ventusinst", default="VentusInst_basic.txt", type=Path)
    ap.add_argument("--encoding-h", default="../spike/riscv/encoding.h", type=Path)
    ap.add_argument("--out", default="data/spike_want.txt", type=Path)
    ap.add_argument("--dry-run", action="store_true", help="Do not write, only report")
    args = ap.parse_args()

    declared = _load_declared_insn_ids(args.encoding_h)
    raw_target = _load_target_pattern_mnemonics(args.ventusinst)
    target = {_resolve_encoding_id(m, declared) for m in raw_target}
    target |= _EXTRAS

    missing = sorted(x for x in target if x not in declared)
    if missing:
        print("error: these target mnemonics are not DECLARE_INSN in encoding.h:", file=sys.stderr)
        for x in missing:
            print(f"  - {x}", file=sys.stderr)
        return 2

    ids = sorted(target)
    if args.dry_run:
        print(f"ok: {len(ids)} ids, would write: {args.out}")
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(_render_want(ids), encoding="utf-8")
    print(f"wrote: {args.out} ({len(ids)} ids)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
