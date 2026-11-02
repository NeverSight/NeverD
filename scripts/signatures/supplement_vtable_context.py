#!/usr/bin/env python3
"""Select missing primary table metadata from unpruned source declarations.

Use neverd-sigmaker --vtable-evidence for both candidate and evidence inputs.
Every declaration for a requested type participates, including incomplete ones.
Existing files must be the packs selected for the intended image profile.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re

if __package__:
    from . import build_msvc_signatures as builder
else:
    import build_msvc_signatures as builder


# Read the matcher-owned floor instead of creating a second policy value.
_LIMITS = Path(__file__).resolve().parents[2] / "include/neverd/sigs/SignatureLimits.def"
MIN_STATED_BYTES = int(re.search(r"NEVERD_SIGS_LIMIT\(MinStatedBytes,\s*(\d+)\)",
                                _LIMITS.read_text(encoding="utf-8"))[1])


def strong_body(line: builder.PatternLine) -> bool:
    shape = builder.Shape.of(line.key)
    leading = shape.lead[:2 * shape.total]
    tail = shape.tail[:2 * max(0, shape.total - len(shape.lead) // 2 - shape.crc_len)]
    fixed = sum(text[i:i + 2] != ".." for text in (leading, tail)
                for i in range(0, len(text), 2))
    return line.require_caller and shape.fully_verified() and fixed >= MIN_STATED_BYTES


@dataclass(frozen=True)
class Table:
    name: str
    size: int
    slots: tuple[tuple[int, tuple[str, ...]], ...]

    @property
    def text(self) -> str:
        fields = " ".join(f"^{offset:04X} {name}"
                          for offset, names in self.slots for name in names)
        return f"{'..' * self.size} 00 0000 {self.size:04X} !vtable :0000 {self.name} {fields}"


def read(path: Path) -> list[tuple[str, Table | None]]:
    result = []
    with path.open(encoding="utf-8") as source:
        for raw in source:
            row = json.loads(raw)
            if not isinstance(row, dict):
                raise builder.BuildError("table evidence must be a JSON object")
            name = row.get("name", "")
            if type(row.get("schema")) is not int or row["schema"] != 1 or not isinstance(name, str) or not re.fullmatch(r"\?\?_7\S+@@6B@", name) or \
                    type(row.get("complete")) is not bool:
                raise builder.BuildError(f"{path.name}: invalid table evidence record")
            if not row["complete"]:
                if not row.get("reason"):
                    raise builder.BuildError("incomplete declaration needs a reason")
                result.append((name, None))
                continue
            size, fields = row.get("bytes"), row.get("slots")
            if type(size) is not int or size <= 0 or size % 8 or not isinstance(fields, list):
                raise builder.BuildError("invalid table extent")
            slots = []
            for field in fields:
                if not isinstance(field, dict):
                    raise builder.BuildError("table field must be a JSON object")
                offset, names, weak = field.get("offset"), field.get("names"), field.get("weak_search")
                if type(offset) is not int or not isinstance(names, list) or \
                        len(names) != (2 if weak is not None else 1) or \
                        any(not isinstance(n, str) or not n or any(c.isspace() for c in n)
                            for n in names) or len(set(names)) != len(names) or \
                        (weak is not None and (type(weak) is not int or weak not in (1, 2, 3))):
                    raise builder.BuildError("invalid table target or weak fallback")
                slots.append((offset, tuple(sorted(names))))
            if sorted(offset for offset, _ in slots) != list(range(0, size, 8)):
                raise builder.BuildError("table fields are incomplete or overlap")
            table = Table(name, size, tuple(sorted(slots)))
            builder.parse_line(table.text)
            result.append((name, table))
    return result


def select(candidates: list[Path], evidence: list[Path], existing: list[Path],
           names: set[str]) -> list[Table]:
    claims = defaultdict(set)
    for path in evidence:
        for name, table in read(path):
            if name in names:
                claims[name].add(table)
    proposed = defaultdict(set)
    for path in candidates:
        for name, table in read(path):
            if name in names:
                proposed[name].add(table)
    if set(proposed) != names:
        raise builder.BuildError("a requested table has no candidate declaration")

    owned = defaultdict(set)
    bodies = set()
    for path in existing:
        for raw in path.read_text(encoding="utf-8").splitlines():
            line = builder.parse_line(raw)
            if line is None:
                continue
            if line.is_vtable:
                owned[line.names[0].split()[1]].add(line.text)
            elif strong_body(line):
                bodies.update(name.split()[1] for name in line.names)
    result = []
    for name in sorted(names):
        variants = proposed[name] | claims[name]
        if None in variants or len(variants) != 1 or not claims[name] or proposed[name] != claims[name]:
            raise builder.BuildError(f"{name}: missing, incomplete or conflicting source declarations")
        table = next(iter(variants))
        if owned[name]:
            # Existing metadata is preserved; a conflict cannot be resolved by
            # publishing another table next to it.
            canonical = {builder.parse_line(text).refs for text in owned[name]}
            line = builder.parse_line(table.text)
            if canonical != {line.refs} or any(builder.parse_line(text).key != line.key for text in owned[name]):
                raise builder.BuildError(f"{name}: existing table disagrees")
            continue
        if not any(target in bodies for _, targets in table.slots for target in targets):
            raise builder.BuildError(f"{name}: selected packs have no complete strong caller body")
        result.append(table)
    return result


def digest(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for role in ("candidate", "evidence", "existing"):
        parser.add_argument("--" + role, type=Path, action="append", required=True)
    parser.add_argument("--name", action="append", required=True)
    parser.add_argument("--output", type=Path, required=True, help="new .npat file")
    args = parser.parse_args(argv)
    try:
        if args.output.suffix != ".npat":
            raise builder.BuildError("table metadata requires the native .npat extension")
        tables = select(args.candidate, args.evidence, args.existing, set(args.name))
        with args.output.open("x", encoding="utf-8") as output:
            output.write("\n".join(table.text for table in tables) + "\n---\n")
        describe = lambda paths: [{"file": p.name, "sha256": digest(p)} for p in paths]
        report = {role: describe(getattr(args, role)) for role in ("candidate", "evidence", "existing")}
        report.update(names=sorted(set(args.name)), output=describe([args.output]), records=len(tables))
        with args.output.with_suffix(".sources.json").open("x", encoding="utf-8") as output:
            output.write(json.dumps(report, indent=2) + "\n")
        print(json.dumps({"records": len(tables), "sha256": digest(args.output)}))
        return 0
    except (builder.BuildError, OSError, ValueError, TypeError, KeyError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
