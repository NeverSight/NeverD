#!/usr/bin/env python3
"""Replay authenticated ordered catch reconstructions with the Windows CRT."""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import xml.etree.ElementTree as ET

if __package__:
    from .check_windows_registration_multiple_catch import (
        BASES, CASES, EMITTER, FORMS, ROUTES, SOURCE, PE32, file_digest, observe,
        require_test_result, validate_installation, validate_decompilation)
    from .windows_registration_libraries import validate_manifest
else:
    from check_windows_registration_multiple_catch import (
        BASES, CASES, EMITTER, FORMS, ROUTES, SOURCE, PE32, file_digest, observe,
        require_test_result, validate_installation, validate_decompilation)
    from windows_registration_libraries import validate_manifest


def validate_capture(root: Path, capture: dict) -> list[tuple]:
    if capture.get("schema") != 1 or capture.get("passed") is not True or \
            capture.get("evidence") != "ordered-catch-source-reconstruction" or \
            capture.get("source_sha256") != file_digest(SOURCE) or \
            capture.get("emitter_sha256") != file_digest(EMITTER):
        raise ValueError("ordered catch reconstruction capture has no current source identity")
    validate_manifest(capture.get("runtime_libraries", {}))
    objects = capture.get("objects", {})
    if set(objects) != set(FORMS):
        raise ValueError("ordered catch object matrix is incomplete")
    for kind, digest in objects.items():
        if digest != file_digest(root / (kind + ".obj")) or \
                require_test_result(root / (kind + "-emit.xml")) != 1:
            raise ValueError("ordered catch object or emission evidence changed")
    cases = capture.get("cases", [])
    if len(cases) != len(CASES) or {c.get("case") for c in cases} != set(CASES):
        raise ValueError("ordered catch source/control matrix is incomplete")
    result = []
    expected = {(route + suffix + ".exe", route, base)
                for route in ROUTES for suffix, base in (("", BASES[0]), ("-rebased", BASES[1]))}
    for case in cases:
        name = case["case"]
        parent = root / name
        if case.get("contract_sha256") != file_digest(parent / "contract.json") or \
                case.get("ir_sha256") != file_digest(parent / "source.ll") or \
                require_test_result(parent / "rewrite.xml") != 1:
            raise ValueError("ordered catch source reconstruction proof changed")
        receipt = json.loads((parent / "contract.json").read_text())
        original = PE32((parent / "original.exe").read_bytes())
        product = PE32((parent / "product.exe").read_bytes())
        validate_installation(original, product, receipt, name)
        sources = case.get("decompilation", {})
        if set(sources) != {"c", "cpp"}:
            raise ValueError("ordered catch decompilation evidence is missing")
        for language, digest in sources.items():
            path = parent / ("decompiled." + language)
            if file_digest(path) != digest:
                raise ValueError("ordered catch decompilation changed")
            validate_decompilation(path.read_text(), language)
        records = case.get("images", [])
        if len(records) != len(expected) or \
                {(r.get("image"), r.get("route"), r.get("base")) for r in records} != expected:
            raise ValueError("ordered catch installation route/base matrix is incomplete")
        for record in records:
            path = parent / record["image"]
            source = original if record["route"] == "original" else product
            image = PE32(path.read_bytes())
            wanted = source.data if record["base"] == BASES[0] else source.rebase(BASES[1])
            if path.read_bytes() != wanted or image.base != record["base"] or \
                    record.get("sha256") != file_digest(path) or \
                    record.get("expected_exit") != int(name.endswith("-control")):
                raise ValueError("ordered catch replay image, base or negative control changed")
            result.append((name, path, record["route"], receipt))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--wine")
    parser.add_argument("--timeout", type=float, default=90)
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    report = {"schema": 1, "platform": platform.platform(), "passed": False,
              "evidence": "wine-multiple-catch-rewrite-replay" if args.wine else "native-multiple-catch-rewrite-replay",
              "images": []}
    try:
        wine = shutil.which(args.wine) if args.wine else None
        if (args.wine or os.name != "nt") and not wine:
            raise ValueError("native Windows or explicit Wine is required")
        root = args.evidence_root.resolve()
        capture = json.loads((root / "multiple-catch-rewrite.json").read_text())
        records = validate_capture(root, capture)
        for name, path, route, receipt in records:
            result = observe(path, name, route, receipt, [wine] if wine else [],
                             os.environ.copy(), args.timeout)
            report["images"].append({"case": name, **result})
        report["passed"] = True
    except (OSError, ValueError, KeyError, TypeError, struct.error, subprocess.TimeoutExpired,
            ET.ParseError) as error:
        report["error"] = str(error)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "report": str(args.output),
                      "error": report.get("error")}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
