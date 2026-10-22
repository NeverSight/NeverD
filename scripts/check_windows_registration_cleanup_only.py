#!/usr/bin/env python3
"""Verify PE32 cleanup-only lowering and installation, with optional Wine checks."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import xml.etree.ElementTree as ET

if __package__:
    from .check_windows_registration_eh import run_image
    from .check_windows_registration_realigned import require_test_result
    from .check_windows_registration_rewrite import PE32
    from .windows_registration_libraries import load_libraries
else:
    from check_windows_registration_eh import run_image
    from check_windows_registration_realigned import require_test_result
    from check_windows_registration_rewrite import PE32
    from windows_registration_libraries import load_libraries

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_cleanup_only.cpp"
SHARED_SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_cleanup_shared.s"
STACK_SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_cleanup_stack_aligned.s"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", type=Path, required=True)
    parser.add_argument("--runtime-libs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--linker", default="lld-link")
    parents = parser.add_mutually_exclusive_group()
    parents.add_argument("--shared-prologue", action="store_true",
                         help="use the authored shared entry/exit parent")
    parents.add_argument("--stack-aligned", action="store_true",
                         help="use the authored MSVC aligned ESP parent")
    parser.add_argument("--wine", help="optionally execute the generated fixture in Wine")
    parser.add_argument("--wine-prefix", type=Path,
                        help="isolated Wine prefix (default: OUTPUT/wine-prefix)")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    evidence = ("cleanup-only-source-to-pe-and-wine" if args.wine else
                "static-cleanup-only-source-to-pe")
    report = {"schema": 2, "evidence": evidence,
              "passed": False, "commands": [], "cases": [],
              "source_sha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest()}
    if args.shared_prologue:
        report["shared_source_sha256"] = hashlib.sha256(
            SHARED_SOURCE.read_bytes()).hexdigest()
    if args.stack_aligned:
        report["stack_source_sha256"] = hashlib.sha256(
            STACK_SOURCE.read_bytes()).hexdigest()
    authored_parent = args.shared_prologue or args.stack_aligned

    def run(command: list[str | Path], extra: dict | None = None) -> None:
        command = list(map(str, command))
        result = subprocess.run(command, capture_output=True, text=True,
                                errors="replace", timeout=120, cwd=output,
                                env=os.environ | (extra or {}))
        report["commands"].append({"command": command,
                                   "exit_code": result.returncode,
                                   "stdout": result.stdout,
                                   "stderr": result.stderr})
        if result.returncode:
            raise ValueError("cleanup-only fixture build or proof failed")

    try:
        libraries, report["runtime_libraries"] = load_libraries(
            args.runtime_libs.resolve())
        for optimization in ("O0", "O1"):
            case = output / optimization
            case.mkdir(exist_ok=True)
            image, obj, xml, product = (case / name for name in
                                        ("original.exe", "source.obj", "proof.xml",
                                         "product.exe"))
            product.unlink(missing_ok=True)
            record = {"optimization": optimization, "passed": False}
            report["cases"].append(record)
            run([args.compiler, "--target=i686-pc-windows-msvc",
                 "-fms-extensions", "-fexceptions", "-fcxx-exceptions",
                 "-fno-omit-frame-pointer", "-" + optimization,
                 *(["-DNEVERD_SHARED_PROLOGUE"] if authored_parent else []),
                 "-c", SOURCE, "-o", obj])
            shared_objects = []
            if authored_parent:
                shared = case / ("stack-aligned.obj" if args.stack_aligned
                                 else "shared.obj")
                run([args.compiler, "--target=i686-pc-windows-msvc",
                     "-c", STACK_SOURCE if args.stack_aligned else SHARED_SOURCE,
                     "-o", shared])
                shared_objects = [shared]
            run([args.linker, "/entry:mainCRTStartup", "/nodefaultlib",
                 "/machine:x86", "/subsystem:console", "/fixed:no",
                 "/dynamicbase:no", "/out:" + str(image), obj, *shared_objects,
                 *(["/export:cleanup_only_parent"] if authored_parent else []),
                 *libraries])
            record["image_sha256"] = hashlib.sha256(image.read_bytes()).hexdigest()
            run([args.test_binary.resolve(),
                 "--gtest_filter=WindowsRegistrationCleanupOnly.*",
                 "--gtest_output=xml:" + str(xml)],
                {"NEVERD_REGISTRATION_CLEANUP_ONLY_PE32": str(image),
                 "NEVERD_REGISTRATION_CLEANUP_ONLY_OUTPUT_PE32": str(product)})
            if require_test_result(xml) != 1:
                raise ValueError("cleanup-only source proof missing or skipped")
            record["product_sha256"] = hashlib.sha256(
                product.read_bytes()).hexdigest()
            if args.wine:
                prefix = (args.wine_prefix or output / "wine-prefix").resolve()
                environment = os.environ | {
                    "WINEPREFIX": str(prefix), "WINEDEBUG": "-all",
                    "WINEDLLOVERRIDES": "mscoree,mshtml=d"}
                wrong = case / "wrong-trace.exe"
                pe = PE32(product.read_bytes())
                changed = bytearray(pe.data)
                struct.pack_into("<I", changed,
                                 pe.raw(pe.entry(b"cleanup_only_trace")), 3)
                wrong.write_bytes(changed)
                record["runtime"] = []
                for original, expected in ((image, 0), (product, 0), (wrong, 1)):
                    rebased = original.with_stem(original.stem + "-rebased")
                    rebased.write_bytes(PE32(original.read_bytes()).rebase(0x500000))
                    for executable in (original, rebased):
                        observation = run_image(executable, [args.wine],
                                                environment, 40)
                        observation.update(
                            image_sha256=hashlib.sha256(
                                executable.read_bytes()).hexdigest(),
                            expected_exit=expected)
                        record["runtime"].append(observation)
                        if observation.get("exit_code") != expected:
                            raise ValueError("cleanup-only runtime result differs")
            record["passed"] = True
        report["passed"] = True
    except (OSError, ValueError, subprocess.TimeoutExpired, ET.ParseError) as error:
        report["error"] = str(error)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("cleanup-only proof:", "PASS" if report["passed"] else "FAIL")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
