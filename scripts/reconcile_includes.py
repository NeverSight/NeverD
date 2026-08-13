#!/usr/bin/env python3
"""Repo-wide #include reconciliation after the structural refactor.

Several components moved and/or renamed their public headers under include/
(notably evm/ and sbf/, which gained an EVM*/SBF* prefix and per-role
subdirectories). Every consumer that still includes an old path is broken.
This tool rebuilds the include graph from the current filesystem and rewrites
each broken `#include "neverd/..."` to the header's new location.

Resolution strategy for a broken `neverd/<comp>/<...>/<Base>`:
  1. If exactly one header under include/ has basename <Base>, use it.
  2. Else, within the same top component dir, try the component-prefixed
     basename (evm -> EVM<Base>, sbf -> SBF<Base>, ...). If unique, use it.
  3. Else report as UNRESOLVED for manual handling (never guess).

Default is a dry run. Pass --apply to write changes.
"""
import argparse
import re
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INC = ROOT / "include"
SEARCH = ["lib", "tools", "unittests", "include", "pluginsdk"]
SRC_EXT = {".cpp", ".h", ".hpp", ".cc", ".c", ".inc"}
INC_RE = re.compile(r'^(\s*#\s*include\s*")(neverd/[^"]+)(".*)$')

# component dir -> filename prefix the refactor added
PREFIX = {"evm": "EVM", "sbf": "SBF"}


def build_index():
    by_rel = set()
    by_base = defaultdict(list)
    for p in INC.rglob("*"):
        if p.is_file():
            rel = p.relative_to(INC).as_posix()
            by_rel.add(rel)
            by_base[p.name].append(rel)
    return by_rel, by_base


def resolve(old, by_rel, by_base):
    if old in by_rel:
        return old  # not broken
    parts = old.split("/")  # neverd / comp / ... / Base
    base = parts[-1]
    comp = parts[1] if len(parts) >= 2 else None

    # (1) component-prefixed basename within the SAME component first, so a
    # renamed evm/Decoder.h -> evm/bytecode/EVMDecoder.h is never mistaken for
    # an unrelated decode/Decoder.h that kept the bare basename.
    if comp and PREFIX.get(comp):
        pcands = [c for c in by_base.get(PREFIX[comp] + base, [])
                  if c.split("/")[:2] == ["neverd", comp]]
        if len(pcands) == 1:
            return pcands[0]

    # (2) bare basename unique within the same component.
    if comp:
        same = [c for c in by_base.get(base, [])
                if c.split("/")[:2] == ["neverd", comp]]
        if len(same) == 1:
            return same[0]

    # (3) globally unique bare basename.
    cands = by_base.get(base, [])
    if len(cands) == 1:
        return cands[0]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    by_rel, by_base = build_index()
    files = []
    for d in SEARCH:
        for p in (ROOT / d).rglob("*"):
            if p.is_file() and p.suffix in SRC_EXT:
                files.append(p)

    rewrites = defaultdict(list)  # (old->new) -> [files]
    unresolved = defaultdict(list)
    changed_files = 0
    for f in files:
        try:
            lines = f.read_text().splitlines(keepends=True)
        except UnicodeDecodeError:
            continue
        dirty = False
        for i, ln in enumerate(lines):
            m = INC_RE.match(ln.rstrip("\n"))
            if not m:
                continue
            old = m.group(2)
            if old in by_rel:
                continue
            new = resolve(old, by_rel, by_base)
            if new is None:
                unresolved[old].append(f.relative_to(ROOT).as_posix())
            elif new != old:
                rewrites[(old, new)].append(f.relative_to(ROOT).as_posix())
                lines[i] = f"{m.group(1)}{new}{m.group(3)}\n"
                dirty = True
        if dirty and args.apply:
            f.write_text("".join(lines))
            changed_files += 1

    print(f"scanned {len(files)} source files; index has {len(by_rel)} headers\n")
    print(f"== REWRITES ({len(rewrites)} distinct path changes) ==")
    for (old, new), fs in sorted(rewrites.items()):
        print(f"  {old}\n    -> {new}   ({len(fs)} files)")
    print(f"\n== UNRESOLVED ({len(unresolved)}) ==")
    for old, fs in sorted(unresolved.items()):
        print(f"  {old}   (referenced by {len(fs)} files, e.g. {fs[0]})")
    if args.apply:
        print(f"\nAPPLIED: modified {changed_files} files")
    else:
        print("\n(dry run; pass --apply to write)")


if __name__ == "__main__":
    main()
