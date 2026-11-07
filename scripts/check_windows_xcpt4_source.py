#!/usr/bin/env python3
"""Check #858's original PE64 execution and source-analysis acceptance.

Wine observes the original executable. It does not certify a reconstructed
image, nor does its observed memory become static frame-proof evidence.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct

if __package__:
    from .check_windows_registration_eh import run_image
else:
    from check_windows_registration_eh import run_image


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / (
    "unittests/corpus/corpus/windows-eh/msvc/x86_64/fh4/gs/o0/"
    "windows-seh-tests/xcpt4-msvc-x86_64-fh4-gs-o0.exe")


def check_runtime(result: dict) -> dict:
    output = result.get("stdout", "")
    # Wine's debugger can return zero after the process faults. Keep crash
    # diagnostics authoritative even if every case printed a passing result.
    diagnostics = output + "\n" + result.get("stderr", "")
    if re.search(r"\bunhandled (?:exception|page fault)\b", diagnostics,
                 re.IGNORECASE):
        raise ValueError("original xcpt4 reported an unhandled Wine exception")
    cases = re.findall(r"^\s*test(\d+)\.\.\.(.*)$", output, re.MULTILINE)
    # xcpt4 can print a failure and still return zero. Check every result.
    if (result.get("exit_code") != 0
            or [int(number) for number, _ in cases] != list(range(1, 91))
            or not all(re.search(r"(?:succeeded|passed)\s*$", text)
                       or (number == "29" and text.strip() == "skipped")
                       for number, text in cases)
            or re.search(r"failed|error", output, re.IGNORECASE)
            or output.count("Start of exception test") != 1
            or output.count("End of exception test") != 1):
        raise ValueError("original xcpt4 has a missing, failed or unexpectedly skipped case")
    # DO_ALIGNMENT_TEST is enabled only for ARM in this original source.
    skipped = [int(number) for number, text in cases if text.strip() == "skipped"]
    return {"passed": len(cases) - len(skipped), "skipped": skipped}


def check_source(source: str) -> None:
    for block in source.split("/* neverd.analysis-only:")[1:]:
        if "code=[0x140001500," not in block:
            continue
        required = ("seh.scope[", "gs.cookie_offset=", "__try", "__finally",
                    "AbnormalTermination()")
        if (all(token in block for token in required)
                and "_local_unwind" not in block
                and "unresolved runtime stack" not in block
                and re.search(r"sub_140001500\([^;]*?\)\s*\{", block)
                and re.search(r"highir.structured_regions=[1-9]\d*", block)):
            return
    raise ValueError("xcpt4 main lacks its explicit source EH/frame projection")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--input", type=Path, default=FIXTURE)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--wine")
    parser.add_argument("--wine-prefix", type=Path)
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"schema": 1, "evidence": "original-runtime-and-source-analysis",
              "passed": False}
    try:
        data = args.input.read_bytes()
        if len(data) < 64 or data[:2] != b"MZ":
            raise ValueError("expected the xcpt4 PE64 fixture")
        pe = struct.unpack_from("<I", data, 60)[0]
        if (pe > len(data) - 26 or data[pe:pe + 4] != b"PE\0\0"
                or struct.unpack_from("<H", data, pe + 4)[0] != 0x8664
                or struct.unpack_from("<H", data, pe + 24)[0] != 0x20b):
            raise ValueError("expected an x64 PE64 image")
        report["input_sha256"] = hashlib.sha256(data).hexdigest()
        environment = os.environ.copy()
        launcher = []
        if os.name != "nt":
            wine = (args.wine or shutil.which("wine")
                    or shutil.which("wine64") or shutil.which("wine64-stable"))
            if not wine:
                raise ValueError("Wine is unavailable")
            launcher = [wine]
            prefix = (args.wine_prefix or args.output / "wine-prefix").resolve()
            prefix.mkdir(parents=True, exist_ok=True)
            environment.update(WINEPREFIX=str(prefix), WINEARCH="win64",
                               WINEDEBUG="-all")
        report["original"] = run_image(args.input, launcher, environment, args.timeout)
        report["original_cases"] = check_runtime(report["original"])
        source = (args.output / "xcpt4-source.c").resolve()
        source.unlink(missing_ok=True)
        report["source"] = run_image(
            args.input, [str(args.neverd.resolve()), "decompile", "--no-debug",
                         "--no-opt", "--language=source", "-o", str(source)],
            os.environ.copy(), args.timeout)
        if report["source"].get("exit_code") != 0:
            raise ValueError("xcpt4 source decompilation failed")
        check_source(source.read_text(encoding="utf-8"))
        if hashlib.sha256(args.input.read_bytes()).hexdigest() != report["input_sha256"]:
            raise ValueError("original fixture changed during observation")
        report["passed"] = True
    except (OSError, ValueError) as error:
        report["error"] = str(error)
    destination = args.output / "xcpt4-source.json"
    destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{'PASS' if report['passed'] else 'FAIL'} {destination}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
