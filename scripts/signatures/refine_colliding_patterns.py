#!/usr/bin/env python3
"""Constrain previously unrepresented byte collisions using unpruned sources.

This is an offline refinement of existing records, not a source-name recognizer.
It does not read evaluation labels or invent aliases. A conflicting complete
body needs sufficient distinguishing references, or becomes an incoming-only query.
An incomplete conflicting record cannot supply either proof and is omitted.
"""

from __future__ import annotations

import argparse
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import sys

try:
    from . import build_msvc_signatures as builder
    from . import refine_pattern_metadata as metadata
    from . import supplement_patterns as supplement
except ImportError:
    import build_msvc_signatures as builder
    import refine_pattern_metadata as metadata
    import supplement_patterns as supplement


def describe(path: Path) -> dict:
    return {"file": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def library_name(path: Path) -> str:
    return re.sub(r"\.part(?:[2-9][0-9]*|1[0-9]+)$", "", path.stem)


def refine(existing: Path, evidence: list[Path], output: Path, machine: str) -> dict:
    if output.exists():
        raise builder.BuildError("output already exists; use a fresh directory")
    if machine not in {"x86", "x64", "arm", "arm64"}:
        raise builder.BuildError(f"unsupported machine {machine!r}")
    paths = builder.pattern_files(existing)
    if not paths or not evidence:
        raise builder.BuildError("existing patterns and unpruned evidence are required")
    source = {}
    for path in evidence:
        count = 0
        for number, text in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            row = builder.parse_line(text, evidence=True)
            if row is None:
                continue
            if text.split()[4] != "!evidence":
                raise builder.BuildError(f"{path}:{number}: expected unpruned !evidence")
            source[row.text] = row
            count += 1
        if not count:
            raise builder.BuildError(f"{path}: empty source evidence")

    documents = {path: path.read_text(encoding="utf-8").splitlines() for path in paths}
    known = defaultdict(list)
    loaded = defaultdict(list)
    for path, lines in documents.items():
        for text in lines:
            row = builder.parse_line(text)
            if row and not (row.require_caller or row.is_vtable or row.is_load_config or row.is_code_layout):
                loaded[library_name(path), row.key].append(row)
                if not row.require_references:
                    known[library_name(path), row.key].append(row)
    by_key = defaultdict(list)
    for row in source.values():
        by_key[row.key].append(row)
    shapes = {key: builder.Shape.of(key) for key in by_key}
    ordered = sorted((shape.lead, key) for key, shape in shapes.items())
    leads = [lead for lead, _ in ordered]
    by_lead = defaultdict(list)
    by_crc = defaultdict(list)
    for key, shape in shapes.items():
        by_lead[shape.lead].append(key)
        if shape.crc_len:
            by_crc[shape.crc_len, shape.crc, len(shape.lead)].append(key)

    coverage = {}

    def covered_keys(row):
        if row.key not in coverage:
            shape = builder.Shape.of(row.key)
            if shape.crc_len:
                nearby = by_crc[shape.crc_len, shape.crc, len(shape.lead)]
            else:
                prefix = shape.stated_prefix()
                nearby = [key for _, key in ordered[
                    bisect_left(leads, prefix):bisect_right(leads, prefix + "~")]]
                # A source can encode the rest in its tail, or end at a
                # shorter RET before our padding. Its leading field is
                # then a prefix of ours, outside the forward range.
                for end in range(2, len(prefix), 2):
                    nearby.extend(by_lead[prefix[:end]])
            coverage[row.key] = [key for key in nearby
                                 if shape.covered_by(shapes[key]) or
                                 shape.padding_collision_with(shapes[key], machine=machine)]
        return coverage[row.key]

    def raw_rivals(row):
        return [other for key in covered_keys(row) for other in by_key[key]
                if builder.another_routine(row.names, other.names)]

    def represented(library, other):
        return any(set(loaded.names).intersection(other.names)
                   and loaded.refs == other.refs
                   and loaded.data_sites == other.data_sites
                   and loaded.local_sites == other.local_sites
                   for loaded in known[library, other.key])

    def incoming_declaration(row):
        variants = [other for other in by_key[row.key]
                    if set(row.names).intersection(other.names)]
        common = builder.common_refs([row, *variants])
        words = common.with_refs(common.refs, require=False).text.split()
        words.insert(4, "!caller")
        return " ".join(words)

    def clause_cost(row):
        independent = all(row.reference_kind(offset) == 0 and
                          metadata.immutable_reference(name) for offset, name in row.refs)
        return (not independent, len(row.refs), row.refs)

    # Constrain the entire loaded byte cohort together. Dropping just one
    # alternative would turn another formerly disputed name into a preferred
    # suggestion even though no new evidence supports that survivor.
    affected = set()
    for path, lines in documents.items():
        library = library_name(path)
        for text in lines:
            row = builder.parse_line(text)
            if row is None or row.require_caller or row.is_vtable or row.is_load_config or row.is_code_layout:
                continue
            if (library, row.key) not in affected and any(
                    not represented(library, other) for other in raw_rivals(row)):
                affected.add((library, row.key))

    changed = {}
    additions = defaultdict(list)
    actions = []
    for path, lines in documents.items():
        kept = []
        library = library_name(path)
        for number, text in enumerate(lines, 1):
            row = builder.parse_line(text)
            if row is None or row.require_caller or row.is_vtable or row.is_load_config or row.is_code_layout:
                kept.append(text)
                continue
            if (library, row.key) not in affected:
                kept.append(text)
                continue
            all_rivals = raw_rivals(row)
            all_rivals.extend(other for other in loaded[library, row.key]
                              if builder.another_routine(row.names, other.names))
            if not all_rivals:
                kept.append(text)
                continue
            complete = builder.Shape.of(row.key).fully_verified()
            if complete and row.refs and all(builder.distinguished([row, other]) for other in all_rivals):
                if row.require_references and not any(
                        other.names == row.names and other.refs == row.refs
                        and other.data_sites == row.data_sites
                        and other.local_sites == row.local_sites
                        for other in by_key[row.key]):
                    # Existing mandatory fields may have been authored by a
                    # different proof. Reduce them only with their own complete,
                    # unpruned source declaration and every covered counterclaim.
                    kept.append(text)
                    continue
                action, token = "required", "!refs"
            elif complete and all(name.startswith(":0000 ") for name in row.names):
                action, token = "caller", "!caller"
            else:
                action, token = "omitted", None
            native_rows = []
            if token == "!refs":
                required = row.with_refs(row.refs, require=True)
                clauses = [variant for variant in supplement.reference_variants(required)[1:]
                           if all(builder.distinguished([variant, other]) for other in all_rivals)]
                # One sufficient clause replaces one global byte record. Do
                # not multiply long bodies for every independently sufficient
                # site. Prefer image metadata, fewer alternatives, then source
                # field order. Metadata does not depend on another byte guess.
                selected = min(clauses, key=clause_cost) if clauses else required
                if clause_cost(selected)[0]:
                    # A callee name can itself be an ambiguous byte claim.
                    # Selecting one such site can confirm a false survivor
                    # while another variant needs an unavailable callee.
                    # Only image-owned metadata permits removing other sites.
                    selected = required
                if row.require_references:
                    if selected.refs == row.refs or clause_cost(selected)[0]:
                        # An image-owned anchor supplies an independent proof.
                        # Preserve existing call-only constraints, which may
                        # encode graph decisions beyond this source audit.
                        kept.append(text)
                        continue
                    action = "reduced"
                native_rows = [selected.text]
                # A pruned clause cannot declare its omitted outgoing edges.
                # Retain a separate complete source query, intersecting fields
                # across all raw same-name versions before incoming expansion.
                if clauses and all(name.startswith(":0000 ") for name in row.names):
                    native_rows.append(incoming_declaration(row))
            elif token:
                native_rows = [incoming_declaration(row)]
            for native in native_rows:
                builder.parse_line(native)
                additions[path.with_suffix(".npat").name].append(native)
            actions.append({"file": path.name, "line": number, "pattern": row.text,
                            "action": action, "rival_count": len(all_rivals),
                            "replacement_records": len(native_rows),
                            "rival_example": all_rivals[0].text})
        changed[path.name] = kept
    for name, rows in additions.items():
        lines = [line for line in changed.get(name, []) if line.strip() != "---"]
        seen = set(lines)
        for row in rows:
            if row not in seen:
                lines.append(row)
                seen.add(row)
        changed[name] = lines + ["---"]

    # Parse and plan the complete transaction before writing any output.
    output.mkdir(parents=True)
    for name, lines in changed.items():
        (output / name).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return {"schema_version": 1, "machine": machine,
            "scope": "Previously unrepresented source claims constrain entire same-library byte cohorts together. Complete contradicted claims retain incoming queries; incomplete claims are omitted.",
            "inputs": [describe(path) for path in paths],
            "evidence": [describe(path) for path in evidence],
            "outputs": [describe(path) for path in sorted(output.iterdir())],
            "counts": dict(Counter(action["action"] for action in actions)),
            "actions": actions}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--existing", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--machine", choices=("x86", "x64", "arm", "arm64"), required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.report.exists() or args.report.resolve().is_relative_to(args.output.resolve()):
            raise builder.BuildError("report must be a new file outside the output directory")
        result = refine(args.existing, args.evidence, args.output, args.machine)
        args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    except (builder.BuildError, OSError, ValueError) as error:
        print(f"collision refinement: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result["counts"], sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
