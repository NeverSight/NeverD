#!/usr/bin/env python3
"""Add source-owned missing references to existing complete patterns.

Only unpruned --collision-evidence outputs may supply references. Every raw
same-body claim sharing an existing name participates, including versions that
omit the field. Existing byte assertions, names and references are preserved.
Changed files are written to a separate output directory for review.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import dataclass, field
import hashlib
import json
from pathlib import Path
import re

if __package__:
    from . import build_msvc_signatures as builder
else:
    import build_msvc_signatures as builder


def immutable_reference(name: str) -> bool:
    return (name.startswith("__imp_") and len(name) > len("__imp_")) or \
        re.fullmatch(r"\?\?_7.+@@6B@", name) is not None or \
        re.fullmatch(r"_TIC?V?U?[1-9][0-9]*\?A[VU].+@@", name) is not None


@dataclass
class Evidence:
    line: builder.PatternLine
    names: set[str] = field(default_factory=set)
    refs: dict[int, set[str]] | None = None
    local_data: bool = False
    external_data: bool = False
    caller_code: bool = False
    local_refs: dict[int, set[str]] | None = None
    external_refs: dict[int, set[str]] | None = None

    def add(self, source: builder.PatternLine) -> None:
        shared = set(self.line.names) & set(source.names)
        if not shared:
            return
        self.names.update(shared)
        # Typed data operands never acquire import/RTTI semantics from their
        # spelling. Keep that distinction even for malformed audit inputs.
        current = builder.ref_table(tuple(
            (offset, name) for offset, name in source.refs
            if offset not in source.data_sites))
        if self.refs is None:
            self.refs = current
        else:
            self.refs = {offset: names | current[offset]
                         for offset, names in self.refs.items() if offset in current}
        if self.local_data:
            current = builder.ref_table(tuple(
                (offset, name) for offset, name in source.refs
                if offset in source.local_sites))
            if self.local_refs is None:
                self.local_refs = current
            else:
                self.local_refs = {offset: names | current[offset]
                                   for offset, names in self.local_refs.items()
                                   if offset in current}
        if self.external_data:
            current = builder.ref_table(tuple(
                (offset, name) for offset, name in source.refs
                if offset in source.data_sites and offset not in source.local_sites))
            if self.external_refs is None:
                self.external_refs = current
            else:
                self.external_refs = {offset: names | current[offset]
                                      for offset, names in self.external_refs.items()
                                      if offset in current}

    def refined(self) -> builder.PatternLine:
        if not set(self.line.names) <= self.names or self.refs is None:
            return self.line
        existing = {offset for offset, _ in self.line.refs}
        added = {(offset, name) for offset, names in self.refs.items()
                 if offset not in existing and
                 (all(map(immutable_reference, names)) or
                  (self.caller_code and self.line.require_caller))
                 for name in names}
        # Every source must own the site, but different builds may use
        # different anchor operands. Preserve their complete alternatives;
        # runtime binding still requires one exact independent anchor.
        local = {(offset, name) for offset, names in (self.local_refs or {}).items()
                 if offset not in existing and
                 (self.line.require_caller or self.line.require_references)
                 for name in names}
        external = {(offset, name) for offset, names in (self.external_refs or {}).items()
                    if offset not in existing and
                    (self.line.require_caller or self.line.require_references)
                    for name in names}
        if not added and not local and not external:
            return self.line
        sites = frozenset(offset for offset, _ in local)
        external_sites = frozenset(offset for offset, _ in external)
        return self.line.with_refs(tuple(sorted(set(self.line.refs) | added | local | external)),
                                   data_sites=self.line.data_sites | sites | external_sites,
                                   local_sites=self.line.local_sites | sites)


def refine(existing: list[Path], evidence: list[Path],
           names: set[str] | None = None, *,
           local_data: bool = False,
           external_data: bool = False,
           caller_code: bool = False) -> dict[Path, str]:
    records = {}
    by_key = defaultdict(list)
    for path in existing:
        rows = []
        for raw in path.read_text(encoding="utf-8").splitlines(keepends=True):
            line = builder.parse_line(raw)
            state = None
            selected = line is not None and (names is None or any(
                name.partition(" ")[2] in names for name in line.names))
            if selected and (local_data or external_data or caller_code or not line.require_caller) and not line.is_vtable and not line.is_load_config and not line.is_code_layout:
                shape = builder.Shape.of(line.key)
                if shape.fully_verified():
                    state = Evidence(line, local_data=local_data,
                                     external_data=external_data, caller_code=caller_code)
                    by_key[line.key].append(state)
            rows.append((raw, state))
        records[path] = rows
    heads = {" ".join(key.split()[:4]) for key in by_key}
    for path in evidence:
        with path.open(encoding="utf-8") as source:
            for raw in source:
                parts = raw.split(maxsplit=5)
                if len(parts) < 6 or " ".join(parts[:4]) not in heads:
                    continue
                if parts[4] != "!evidence":
                    raise builder.BuildError("metadata refinement requires raw !evidence records")
                line = builder.parse_line(raw, evidence=True)
                if line is not None:
                    for state in by_key.get(line.key, []):
                        state.add(line)
    changed = {}
    for path, rows in records.items():
        output = []
        modified = False
        for raw, state in rows:
            line = state.refined() if state is not None else None
            if line is not None and line.refs != state.line.refs:
                output.append(line.text + "\n")
                modified = True
            else:
                output.append(raw)
        if modified:
            changed[path] = "".join(output)
    return changed


def digest(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--existing", type=Path, action="append", required=True)
    parser.add_argument("--evidence", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--name", action="append",
                        help="refine only this linkage name; repeat for a reviewed subset")
    parser.add_argument("--local-data", action="store_true",
                        help="also add unanimous same-object data relations to contextual records")
    parser.add_argument("--caller-code", action="store_true",
                        help="also add unanimous code references to complete caller records")
    parser.add_argument("--external-data", action="store_true",
                        help="also add unanimous external-data operands to contextual records")
    args = parser.parse_args(argv)
    try:
        if len({path.name for path in args.existing}) != len(args.existing):
            raise builder.BuildError("existing files need distinct output basenames")
        changed = refine(args.existing, args.evidence, set(args.name) if args.name else None,
                         local_data=args.local_data, external_data=args.external_data,
                         caller_code=args.caller_code)
        args.output.mkdir(parents=True, exist_ok=True)
        written = []
        for source, text in changed.items():
            target = args.output / source.name
            with target.open("x", encoding="utf-8") as output:
                output.write(text)
            written.append({"file": target.name, "sha256": digest(target)})
        describe = lambda paths: [{"file": path.name, "sha256": digest(path)} for path in paths]
        report = {"existing": describe(args.existing), "evidence": describe(args.evidence),
                  "names": args.name or [],
                  "local_data": args.local_data,
                  "external_data": args.external_data,
                  "caller_code": args.caller_code,
                  "output": written}
        with (args.output / "metadata.sources.json").open("x", encoding="utf-8") as output:
            output.write(json.dumps(report, indent=2) + "\n")
        print(json.dumps(written, indent=2))
        return 0
    except (builder.BuildError, OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
