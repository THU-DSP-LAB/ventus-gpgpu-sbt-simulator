#!/usr/bin/env python3
import argparse
import json
from pathlib import Path


_TARGET_ALIASES: dict[str, str] = {
    # VentusInst_basic.txt mnemonic -> sbt_decode decoded name.
    # Keep minimal and auditable.
    "vfexp": "vfexp_v",
    "vmandnot_mm": "vmandn_mm",
    "vmornot_mm": "vmorn_mm",
}


def load_target_mnemonics(path: Path) -> set[str]:
    lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
    out: set[str] = set()
    for i, line in enumerate(lines):
        if i == 0:
            continue
        line = line.strip()
        if not line:
            continue
        cols = line.split("\t")
        if len(cols) < 2:
            continue
        asm = cols[1].strip()
        if not asm:
            continue
        mnem = asm.split()[0]
        normalized = mnem.replace(".", "_")
        out.add(_TARGET_ALIASES.get(normalized, normalized))
    return out


def load_exceptions(path: Path) -> set[str]:
    if not path.exists():
        return set()
    out: set[str] = set()
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        s = line.split("#", 1)[0].strip()
        if not s:
            continue
        out.add(s)
    return out


def load_decoded_names(paths: list[Path]) -> set[str]:
    out: set[str] = set()
    for p in paths:
        obj = json.loads(p.read_text(encoding="utf-8", errors="ignore"))
        saw_regext = False
        for inst in obj.get("decoded", []):
            name = inst.get("name")
            if isinstance(name, str):
                out.add(name)
            if "regext" in inst:
                saw_regext = True
        # sbt_decode bundles regext/regexti as prefixes into the following instruction.
        # Treat both prefix mnemonics as covered if any decoded instruction had a regext prefix.
        if saw_regext:
            out.add("regext")
            out.add("regexti")
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="VentusInst_basic.txt")
    ap.add_argument("--exceptions", default="data/inst_exceptions.txt", help="Mnemonic-level exception list (one per line)")
    ap.add_argument("--decoded-json", nargs="+", required=True)
    ap.add_argument("--min-covered", type=int, default=0)
    ap.add_argument("--min-ratio", type=float, default=0.0)
    ap.add_argument("--require-full", action="store_true", help="Fail unless all (target - exceptions) mnemonics are covered")
    args = ap.parse_args()

    target = load_target_mnemonics(Path(args.target))
    exceptions = load_exceptions(Path(args.exceptions)) if args.exceptions else set()
    effective_target = target - exceptions
    decoded = load_decoded_names([Path(p) for p in args.decoded_json])
    covered = effective_target.intersection(decoded)

    total = len(target)
    exc = len(exceptions.intersection(target))
    effective_total = len(effective_target)
    cov = len(covered)
    ratio = (cov / effective_total) if effective_total else 0.0
    uncovered = effective_total - cov

    print(
        f"target_unique={total} exceptions_in_target={exc} effective_target_unique={effective_total} "
        f"decoded_unique={len(decoded)} covered_unique={cov} uncovered_unique={uncovered} ratio={ratio:.3f}"
    )

    ok = True
    if args.require_full and uncovered != 0:
        ok = False
    if args.min_covered and cov < args.min_covered:
        ok = False
    if args.min_ratio and ratio < args.min_ratio:
        ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
