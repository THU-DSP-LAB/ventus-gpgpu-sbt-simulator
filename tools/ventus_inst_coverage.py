#!/usr/bin/env python3
import argparse
import json
from pathlib import Path


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
        out.add(mnem.replace(".", "_"))
    return out


def load_decoded_names(paths: list[Path]) -> set[str]:
    out: set[str] = set()
    for p in paths:
        obj = json.loads(p.read_text(encoding="utf-8", errors="ignore"))
        for inst in obj.get("decoded", []):
            name = inst.get("name")
            if isinstance(name, str):
                out.add(name)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="VentusInst_basic.txt")
    ap.add_argument("--decoded-json", nargs="+", required=True)
    ap.add_argument("--min-covered", type=int, default=0)
    ap.add_argument("--min-ratio", type=float, default=0.0)
    args = ap.parse_args()

    target = load_target_mnemonics(Path(args.target))
    decoded = load_decoded_names([Path(p) for p in args.decoded_json])
    covered = target.intersection(decoded)

    total = len(target)
    cov = len(covered)
    ratio = (cov / total) if total else 0.0

    print(f"target_unique={total} decoded_unique={len(decoded)} covered_unique={cov} ratio={ratio:.3f}")

    ok = True
    if args.min_covered and cov < args.min_covered:
        ok = False
    if args.min_ratio and ratio < args.min_ratio:
        ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())

