#!/usr/bin/env python3
"""Replay the same hashed realigned callback probes with the native CRT."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import xml.etree.ElementTree as ET

if __package__:
    from .check_windows_registration_eh import image_digest, run_image
    from .check_windows_registration_realigned import SOURCE, require_test_result
    from .check_windows_registration_rewrite import PE32
    from .windows_registration_libraries import validate_manifest
else:
    from check_windows_registration_eh import image_digest, run_image
    from check_windows_registration_realigned import SOURCE, require_test_result
    from check_windows_registration_rewrite import PE32
    from windows_registration_libraries import validate_manifest

IMAGES = {"probe.exe": 0, "probe-rebased.exe": 0,
          "wrong-result.exe": 1, "wrong-result-rebased.exe": 1}


def validate_capture(root: Path, capture: dict) -> None:
    validate_manifest(capture.get("runtime_libraries", {}))
    if require_test_result(root / "emit.xml") != 2:
        raise ValueError("realigned emission/root identity evidence is missing")
    if capture.get("schema") != 1 or \
            capture.get("evidence") != "generated-realigned-callback-analysis" or \
            capture.get("source_sha256") != hashlib.sha256(SOURCE.read_bytes()).hexdigest() or \
            capture.get("object_sha256") != hashlib.sha256((root / "frame.obj").read_bytes()).hexdigest():
        raise ValueError("realigned callback capture has a different source or object")
    records = capture.get("images", [])
    if len(records) != len(IMAGES) or {r.get("image") for r in records} != set(IMAGES):
        raise ValueError("realigned callback runtime/control matrix is incomplete")
    for record in records:
        name = record["image"]
        if record.get("expected_exit") != IMAGES[name] or \
                record.get("sha256") != image_digest(root / name):
            raise ValueError("realigned callback image or expected result changed")
        if IMAGES[name] == 0:
            if record.get("analysis_tests") != 1 or \
                    require_test_result(root / (Path(name).stem + ".xml")) != 1:
                raise ValueError("realigned callback analysis evidence is missing")
    for name in ("probe", "wrong-result"):
        original = PE32((root / (name + ".exe")).read_bytes())
        if original.base != 0x400000 or \
                original.rebase(0x18000000) != (root / (name + "-rebased.exe")).read_bytes():
            raise ValueError("realigned callback rebase is not the same image")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--wine")
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    report = {"schema": 1, "passed": False, "images": []}
    try:
        wine = shutil.which(args.wine) if args.wine else None
        if os.name != "nt" and not wine:
            raise ValueError("native Windows or an explicit Wine executable is required")
        root = args.evidence_root.resolve()
        capture = json.loads((root / "realigned-callback.json").read_text())
        validate_capture(root, capture)
        for name, expected in IMAGES.items():
            path = root / name
            runtime = run_image(path, [wine] if wine else [], os.environ.copy(), args.timeout)
            report["images"].append({"image": name, "sha256": image_digest(path),
                                     "expected_exit": expected, "runtime": runtime})
            if runtime.get("exit_code") != expected:
                raise ValueError("native callback runtime/control mismatch")
        report["passed"] = True
    except (OSError, ValueError, KeyError, TypeError, struct.error, ET.ParseError) as error:
        report["error"] = str(error)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "report": str(args.output),
                      "error": report.get("error")}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
