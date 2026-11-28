#!/usr/bin/env python3
"""Summarize recorded verification stages and independent execution evidence."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


SCHEMA = "neverd.verification.v1"
STATES = ("passed", "failed", "blocked", "not_run", "unknown")
LOOP_STATES = ("not_run", "started", "completed", "unknown")


def summarize(document: dict) -> dict:
    if not isinstance(document, dict) or document.get("schema") != SCHEMA:
        raise ValueError("unsupported verification report schema")
    stages = document.get("stages")
    if not isinstance(stages, list):
        raise ValueError("stages must be an ordered array")
    first = None
    ordered = []
    for stage in stages:
        if not isinstance(stage, dict) or stage.get("state") not in STATES:
            raise ValueError("invalid verification stage state")
        path = stage.get("path")
        if (not isinstance(path, list) or not path
                or not all(isinstance(part, str) and part.strip() for part in path)):
            raise ValueError("a stage path must contain nonempty names")
        row = {"path": path, "state": stage["state"]}
        for key in ("diagnostic", "missing_evidence"):
            if key in stage:
                if not isinstance(stage[key], str):
                    raise ValueError(f"{key} must be text")
                row[key] = stage[key]
        ordered.append(row)
        if first is None and row["state"] in ("failed", "blocked"):
            first = row

    return {"schema": SCHEMA, "first_blocker": first, "stages": ordered,
            **execution_evidence(document)}


def execution_evidence(document: dict) -> dict:
    if not isinstance(document, dict):
        raise ValueError("execution evidence must be an object")
    comparison = document.get("record_comparison")
    if comparison is not None:
        if not isinstance(comparison, dict):
            raise ValueError("record_comparison must contain observed counts")
        compared, matches = comparison.get("compared"), comparison.get("matches")
        total = comparison.get("total", compared)
        if (any(type(n) is not int for n in (compared, matches, total))
                or not 0 <= matches <= compared <= total):
            raise ValueError("record counts must satisfy 0 <= matches <= compared <= total")
        comparison = {key: comparison[key] for key in ("total", "compared", "matches")
                      if key in comparison}
    loop = document.get("caller_loop", "unknown")
    if loop not in LOOP_STATES:
        raise ValueError("invalid caller loop execution state")
    return {"record_comparison": comparison, "caller_loop": loop}


def render(report: dict) -> str:
    first = report["first_blocker"]
    if first is None:
        lines = ["First blocking step: none recorded"]
    else:
        lines = [f"First blocking step: {' > '.join(first['path'])} ({first['state']})",
                 "Diagnostic: " + (first.get("diagnostic") or "not recorded"),
                 "Missing evidence: " + (first.get("missing_evidence") or "not specified")]
    comparison = report["record_comparison"]
    counts = "not recorded"
    if comparison is not None:
        total = f" of {comparison['total']}" if "total" in comparison else ""
        counts = f"{comparison['compared']}{total} compared, {comparison['matches']} matches"
    lines.extend(["Record comparison: " + counts,
                  "Caller loop: " + report["caller_loop"].replace("_", " "), "Stages:"])
    for stage in report["stages"]:
        lines.append(f"  {' > '.join(stage['path'])}: {stage['state'].replace('_', ' ')}")
        if stage != first and stage["state"] in ("failed", "blocked"):
            if stage.get("diagnostic"):
                lines.append("    Diagnostic: " + stage["diagnostic"])
            if stage.get("missing_evidence"):
                lines.append("    Missing evidence: " + stage["missing_evidence"])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--json", action="store_true", help="print machine-readable output")
    args = parser.parse_args()
    try:
        result = summarize(json.loads(args.report.read_text(encoding="utf-8")))
    except (OSError, ValueError) as exc:
        parser.exit(2, f"{parser.prog}: {exc}\n")
    print(json.dumps(result, indent=2) if args.json else render(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
