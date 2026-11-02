#!/usr/bin/env python3
"""Join unpruned source configuration, pointer and function declarations.

Input comes from neverd-sigmaker --runtime-pointer-evidence, with every selected
pointer and its proposed default included as --evidence-symbol. Existing bodies
are refined in copied packs; this tool never duplicates them or lowers the
ordinary caller floor. Verify emitted .npat files with neverd-sigmaker --verify.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path

if __package__:
    from . import build_msvc_signatures as builder
else:
    import build_msvc_signatures as builder


def name(value: object) -> bool:
    return isinstance(value, str) and bool(value) and not any(c.isspace() for c in value)


def read(paths: list[Path]) -> list[dict]:
    rows = []
    for path in paths:
        for text in path.read_text(encoding="utf-8").splitlines():
            row = json.loads(text)
            if not isinstance(row, dict) or type(row.get("schema")) is not int or row["schema"] != 1 or \
                    not name(row.get("name")) or type(row.get("complete")) is not bool or \
                    row.get("kind") not in ("load_config", "pointer", "function"):
                raise builder.BuildError("invalid runtime pointer evidence record")
            if not row["complete"]:
                if not isinstance(row.get("reason"), str) or not row["reason"]:
                    raise builder.BuildError("incomplete evidence needs a reason")
            elif row["kind"] == "load_config":
                size, fields = row.get("bytes"), row.get("fields")
                if row["name"] != "_load_config_used" or type(size) is not int or size < 4 or not isinstance(fields, list):
                    raise builder.BuildError("invalid source configuration extent")
                offsets = set()
                for field in fields:
                    if not isinstance(field, dict) or type(field.get("offset")) is not int or \
                            field["offset"] < 0 or field["offset"] % 8 or field["offset"] + 8 > size or \
                            field["offset"] in offsets or not name(field.get("name")):
                        raise builder.BuildError("invalid source callable field")
                    offsets.add(field["offset"])
            elif row["kind"] == "pointer":
                if not name(row.get("target")):
                    raise builder.BuildError("invalid source default target")
            else:
                if not isinstance(row.get("pattern"), str):
                    raise builder.BuildError("function definition needs its complete body")
                line = builder.parse_line(row["pattern"])
                if line is None or not line.require_caller or row["name"] not in {n.split()[1] for n in line.names}:
                    raise builder.BuildError("function definition disagrees with its body")
            rows.append(row)
    return rows


def fully_bound(line: builder.PatternLine) -> bool:
    """Every wildcard belongs to a typed four-byte relocated operand.

    Native matching additionally decodes each operand and confirms the exact
    independently bound storage. Function-reference alternatives cannot enter
    this metadata gate.
    """
    shape = builder.Shape.of(line.key)
    if not line.require_caller or shape.crc_len or not shape.fully_verified() or \
            len(shape.lead + shape.tail) != shape.total * 2 or \
            line.local_sites or any(offset not in line.data_sites for offset, _ in line.refs):
        return False
    fields = {i for offset in line.data_sites for i in range(offset, offset + 4)}
    return all((shape.lead + shape.tail)[i * 2:i * 2 + 2] != ".." or i in fields
               for i in range(shape.total)) and all(0 <= i < shape.total for i in fields)


@dataclass
class Selection:
    metadata: str | None
    updates: dict[Path, str]
    fields: tuple[tuple[int, str, str], ...]


def select(candidates: list[Path], evidence: list[Path], existing: list[Path],
           variables: set[str]) -> Selection:
    proposed, audit = read(candidates), read(evidence)
    if not proposed or not audit or not variables:
        raise builder.BuildError("candidate, independent evidence and pointer names are required")
    rows = proposed + audit
    definitions = defaultdict(list)
    configurations = []
    for row in rows:
        if row["kind"] == "load_config":
            if not row["complete"]:
                raise builder.BuildError("incomplete source configuration counterclaim")
            configurations.append(row)
        else:
            definitions[row["name"]].append(row)
    proposed_configs = [r for r in proposed if r["kind"] == "load_config" and r["complete"]]
    fields = set()
    sizes = []
    for row in proposed_configs:
        selected = [(f["offset"], f["name"]) for f in row["fields"] if f["name"] in variables]
        if selected:
            fields.update(selected)
            sizes.append(row["bytes"])
    if {n for _, n in fields} != variables or not sizes:
        raise builder.BuildError("a requested pointer has no candidate configuration field")
    by_offset = defaultdict(set)
    for row in configurations:
        for field in row["fields"]:
            by_offset[field["offset"]].add(field["name"])
    triples = []
    for offset, variable in sorted(fields):
        if by_offset[offset] != {variable}:
            raise builder.BuildError("conflicting configuration field ownership")
        claims = definitions[variable]
        if not claims or any(not r["complete"] or r["kind"] != "pointer" for r in claims):
            raise builder.BuildError(f"{variable}: missing or incomplete pointer definitions")
        targets = {r["target"] for r in claims}
        if len(targets) != 1:
            raise builder.BuildError(f"{variable}: conflicting default targets")
        target = next(iter(targets))
        if not any(r["name"] == variable for r in audit) or not any(r["name"] == variable for r in proposed):
            raise builder.BuildError(f"{variable}: missing candidate or audit definition")
        triples.append((offset, variable, target))
    fields_tuple = tuple(triples)
    targets = {t for _, _, t in triples}
    sources = defaultdict(lambda: defaultdict(set))
    for target in targets:
        claims = definitions[target]
        if not claims or any(not r["complete"] or r["kind"] != "function" for r in claims):
            raise builder.BuildError(f"{target}: missing or incomplete explicitly typed function definitions")
        if not any(r["name"] == target for r in audit) or not any(r["name"] == target for r in proposed):
            raise builder.BuildError(f"{target}: missing candidate or audit function")
        for row in claims:
            line = builder.parse_line(row["pattern"])
            if not fully_bound(line) or any(n not in variables for _, n in line.refs):
                raise builder.BuildError(f"{target}: source body has an unbound operand")
            sources[target][line.key].add((line.refs, line.data_sites))
        if any(len(refs) != 1 for refs in sources[target].values()):
            raise builder.BuildError(f"{target}: identical source bodies disagree on references")
    owned = []
    found = set()
    updates = {}
    for path in existing:
        original = path.read_text(encoding="utf-8")
        output = []
        for raw in original.splitlines(keepends=True):
            line = builder.parse_line(raw)
            if line is None:
                output.append(raw)
                continue
            if line.is_load_config:
                owned.append(line.load_config_fields)
                output.append(raw)
                continue
            selected = targets.intersection(n.split()[1] for n in line.names)
            replacement = line
            refinements = set()
            for target in selected:
                if not line.require_caller or line.key not in sources[target]:
                    continue
                refs, data_sites = next(iter(sources[target][line.key]))
                if any(ref not in refs for ref in line.refs) or not line.data_sites.issubset(data_sites):
                    raise builder.BuildError(f"{target}: existing source constraints conflict")
                refinements.add((refs, data_sites))
                if len(refinements) != 1:
                    raise builder.BuildError("existing aliases have conflicting source references")
                replacement = line.with_refs(refs, data_sites=data_sites)
                found.add(target)
            output.append(replacement.text + ("\n" if raw.endswith("\n") else "")
                          if replacement.text != line.text else raw)
        text = "".join(output)
        if text != original:
            updates[path] = text
    if found != targets:
        raise builder.BuildError("selected packs lack an existing complete source-matched caller body")
    size = max(sizes)
    text = (".." * size + f" 00 0000 {size:04X} !loadcfg :0000 _load_config_used " +
            " ".join(f"&{off:04X} {var} ^{off:04X} {target}" for off, var, target in triples))
    builder.parse_line(text)
    if owned and any(value != fields_tuple for value in owned):
        raise builder.BuildError("existing runtime metadata conflicts; preserve it for review")
    return Selection(None if owned else text, updates, fields_tuple)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for role in ("candidate", "evidence", "existing"):
        parser.add_argument("--" + role, type=Path, action="append", required=True)
    parser.add_argument("--pointer", action="append", required=True)
    parser.add_argument("--output", type=Path, required=True, help="new metadata .npat")
    parser.add_argument("--updated-dir", type=Path, required=True, help="new directory for refined copies")
    args = parser.parse_args(argv)
    try:
        if args.output.suffix != ".npat":
            raise builder.BuildError("runtime metadata requires .npat")
        result = select(args.candidate, args.evidence, args.existing, set(args.pointer))
        if len({p.name for p in result.updates}) != len(result.updates):
            raise builder.BuildError("refined input basenames collide")
        args.updated_dir.mkdir(exist_ok=False, parents=True)
        with args.output.open("x", encoding="utf-8") as stream:
            stream.write((result.metadata + "\n" if result.metadata else "") + "---\n")
        for path, text in result.updates.items():
            (args.updated_dir / path.name).write_text(text, encoding="utf-8")
        report = {"fields": result.fields, "metadata_added": result.metadata is not None,
                  "refined": [{"file": p.name,
                               "before_sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
                               "after_sha256": hashlib.sha256(t.encode()).hexdigest()}
                              for p, t in result.updates.items()]}
        print(json.dumps(report, indent=2))
    except (builder.BuildError, OSError, ValueError) as exc:
        parser.exit(1, f"error: {exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
