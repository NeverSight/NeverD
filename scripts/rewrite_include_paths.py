#!/usr/bin/env python3
"""Rewrite `#include` paths across the tree after public headers move.

Reorganizing include/neverd/ leaves every consumer pointing at the old path.
This applies a mapping file of `old/path.h -> new/path.h` lines to every
tracked C, C++, and CMake-adjacent source in the repository, so the move and
its fallout stay a single reviewable step.

Run from the repository root:

    python3 scripts/rewrite_include_paths.py mapping.txt
    python3 scripts/rewrite_include_paths.py mapping.txt --check

Lines starting with `#` and blank lines in the mapping file are ignored.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

SEARCH_ROOTS = ["lib", "include", "tools", "unittests", "plugins", "pluginsdk"]
SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx", ".inc", ".def"}
SKIP_DIR_PREFIXES = ("build", "third_party", ".git", "corpus")


def load_mapping(path: Path) -> dict[str, str]:
    mapping: dict[str, str] = {}
    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "->" not in line:
            raise SystemExit(f"{path}:{lineno}: expected `old -> new`, got {line!r}")
        old, new = (part.strip() for part in line.split("->", 1))
        if not old or not new:
            raise SystemExit(f"{path}:{lineno}: empty side in {line!r}")
        if old in mapping and mapping[old] != new:
            raise SystemExit(f"{path}:{lineno}: conflicting mapping for {old}")
        mapping[old] = new
    if not mapping:
        raise SystemExit(f"{path}: no mappings found")
    return mapping


def iter_sources():
    for root in SEARCH_ROOTS:
        base = REPO / root
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            rel_parts = path.relative_to(REPO).parts
            if any(part.startswith(SKIP_DIR_PREFIXES) for part in rel_parts):
                continue
            yield path


def build_pattern(mapping: dict[str, str]) -> re.Pattern[str]:
    # Longest first so `a/b/c.h` wins over a hypothetical `a/b.h` prefix.
    alternatives = "|".join(
        re.escape(old) for old in sorted(mapping, key=len, reverse=True)
    )
    return re.compile(rf'(#\s*include\s*[<"])({alternatives})([>"])')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mapping", type=Path, help="file of `old -> new` lines")
    parser.add_argument(
        "--check",
        action="store_true",
        help="report which files would change without writing them",
    )
    args = parser.parse_args()

    mapping = load_mapping(args.mapping)
    pattern = build_pattern(mapping)

    changed: list[tuple[str, int]] = []
    for path in iter_sources():
        try:
            text = path.read_text()
        except UnicodeDecodeError:
            continue
        new_text, count = pattern.subn(
            lambda m: f"{m.group(1)}{mapping[m.group(2)]}{m.group(3)}", text
        )
        if count:
            changed.append((path.relative_to(REPO).as_posix(), count))
            if not args.check:
                path.write_text(new_text)

    if not changed:
        print("no include paths matched the mapping")
        return 0

    total = sum(count for _, count in changed)
    verb = "would rewrite" if args.check else "rewrote"
    print(f"{verb} {total} include(s) across {len(changed)} file(s):")
    for rel, count in sorted(changed):
        print(f"  {rel} ({count})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
