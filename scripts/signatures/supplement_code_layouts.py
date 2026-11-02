#!/usr/bin/env python3
"""Publish source-owned code contributions without function-pattern folding.

Read all declarations from neverd-sigmaker --code-layout-evidence. Keep complete
layouts as separate claims, including different nominal claims for the same
bytes. Native verification is mandatory before replacing an output file.
No evaluation image, linker map or desired-name list is an input.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

if __package__:
    from . import build_msvc_signatures as builder
else:
    import build_msvc_signatures as builder


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


@dataclass
class Selection:
    lines: list[str]
    inputs: list[dict]
    witnesses: list[dict]
    incomplete: int
    existing: list[dict]


def select(evidence: list[Path], existing: list[Path]) -> Selection:
    if not evidence:
        raise builder.BuildError("source evidence is required")
    prior, existing_inputs = set(), []
    for path in existing:
        files = sorted([*path.glob("*.pat"), *path.glob("*.npat")]) if path.is_dir() else [path]
        for file in files:
            existing_inputs.append({"file": file.name, "sha256": sha(file)})
            for text in file.read_text(encoding="utf-8").splitlines():
                fields = text.split(maxsplit=5)
                if len(fields) > 4 and fields[4] == "!layout":
                    line = builder.parse_line(text)
                    if line is not None:
                        prior.add(line.text)
    selected, witnesses, inputs, incomplete = set(), [], [], 0
    for path in evidence:
        inputs.append({"file": path.name, "sha256": sha(path)})
        for number, text in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            row = json.loads(text)
            if not isinstance(row, dict) or type(row.get("schema")) is not int or row["schema"] != 1 or \
                    not isinstance(row.get("member"), str) or not row["member"] or \
                    type(row.get("section")) is not int or row["section"] <= 0 or \
                    type(row.get("complete")) is not bool or not isinstance(row.get("members"), list):
                raise builder.BuildError(f"{path.name}:{number}: invalid source layout declaration")
            declared = []
            for member in row["members"]:
                if not isinstance(member, dict) or type(member.get("offset")) is not int or member["offset"] < 0 or \
                        not isinstance(member.get("names"), list) or not member["names"] or \
                        any(not isinstance(n, str) or not n or any(c.isspace() for c in n) for n in member["names"]):
                    raise builder.BuildError("invalid source function coordinates")
                declared.extend((member["offset"], n) for n in member["names"])
            if not row["complete"]:
                if not isinstance(row.get("reason"), str) or not row["reason"] or "pattern" in row:
                    raise builder.BuildError("incomplete source layout needs a reason and no candidate pattern")
                incomplete += 1
                continue
            if not isinstance(row.get("pattern"), str):
                raise builder.BuildError("complete layout needs its native pattern")
            line = builder.parse_line(row["pattern"])
            if line is None or not line.is_code_layout or \
                    sorted(declared) != sorted((int(n.split()[0][1:], 16), n.split()[1]) for n in line.names):
                raise builder.BuildError("source layout disagrees with declared function coordinates")
            if line.text in prior:
                continue
            selected.add(line.text)
            witnesses.append({"file": path.name, "line": number, "member": row["member"],
                              "section": row["section"],
                              "pattern_sha256": hashlib.sha256(line.text.encode()).hexdigest()})
    return Selection(sorted(selected), inputs, witnesses, incomplete, existing_inputs)


def publish(selection: Selection, output: Path, producer: Path) -> dict:
    if output.suffix != ".npat":
        raise builder.BuildError("code layouts require a native .npat output")
    output.parent.mkdir(parents=True, exist_ok=True)
    text = "; Source-owned nonsplittable code layouts; candidates only.\n"
    text += "\n".join(selection.lines) + ("\n" if selection.lines else "")
    with tempfile.TemporaryDirectory(prefix="neverd-code-layout-", dir=output.parent) as temporary:
        candidate = Path(temporary) / output.name
        candidate.write_text(text, encoding="utf-8")
        subprocess.run([str(producer.resolve()), "--verify", str(candidate)], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        # Verification cannot silently rewrite the proposed artifact.
        if candidate.read_text(encoding="utf-8") != text:
            raise builder.BuildError("native verification changed the candidate")
        candidate.replace(output)
    return {"schema": 1, "kind": "source_code_layout", "evidence": selection.inputs,
            "incomplete_declarations": selection.incomplete, "witnesses": selection.witnesses,
            "existing": selection.existing,
            "records": len(selection.lines), "global_byte_records": 0,
            "producer_sha256": sha(producer),
            "output": {"file": output.name, "sha256": sha(output), "bytes": output.stat().st_size}}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, action="append", required=True)
    parser.add_argument("--existing", type=Path, action="append", default=[])
    parser.add_argument("--producer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = publish(select(args.evidence, args.existing), args.output, args.producer)
        args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, builder.BuildError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"code layout selection failed: {error}\n")
    print(f"Published {result['records']} code layouts; no global byte-index records")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
