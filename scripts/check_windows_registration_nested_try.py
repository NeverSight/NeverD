#!/usr/bin/env python3
"""Reconstruct compiler-generated nested PE32 C++ exception searches."""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import xml.etree.ElementTree as ET

if __package__:
    from .check_windows_registration_multiple_catch import (
        BASES, ROUTES, PE32, file_digest, observe, require_test_result,
        run_image, validate_installation)
    from .windows_registration_libraries import load_libraries
else:
    from check_windows_registration_multiple_catch import (
        BASES, ROUTES, PE32, file_digest, observe, require_test_result,
        run_image, validate_installation)
    from windows_registration_libraries import load_libraries

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_nested_try_driver.cpp"
EMITTER = ROOT / "unittests/lift/eh/WindowsRegistrationNestedTryTests.cpp"
PROOF = ROOT / "unittests/lift/eh/RegistrationNestedTryTestUtils.cpp"
FORMS = {"nested-o0-llvm-fixed": "-O0", "nested-o1-llvm-fixed": "-O1"}
CASES = tuple(name + suffix for name in FORMS for suffix in ("", "-control"))


def validate_decompilation(text: str, language: str) -> None:
    if "highir.structured_regions=2, fallback_regions=0" not in text or \
            text.count("= __neverd_x86_callback_esp(0x") != 3:
        raise ValueError("nested decompilation lost a language region or callback")
    if language == "c" and text.count("Native x86 callback @") != 3:
        raise ValueError("nested C output lost a callback entry")
    if language == "cpp" and (text.count("catch (") != 3 or
                               len(re.findall(r"\btry \{", text)) != 2 or ".Value" in text):
        raise ValueError("nested C++ output lost a try or invented an object field")
    if len(set(re.findall(r"goto L_([0-9A-F]+);", text))) < 3:
        raise ValueError("nested output lost runtime continuation targets")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", type=Path, required=True)
    parser.add_argument("--patch-binary", type=Path, required=True)
    parser.add_argument("--runtime-libs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--linker", default="lld-link")
    parser.add_argument("--wine", default="wine")
    parser.add_argument("--timeout", type=float, default=90)
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy() | {"WINEDEBUG": "-all"}
    report = {"schema": 1, "evidence": "nested-try-source-reconstruction",
              "passed": False, "commands": [], "cases": []}

    def run(command, extra=None):
        command = list(map(str, command))
        result = subprocess.run(command, env=env | (extra or {}), cwd=out,
                                capture_output=True, text=True, errors="replace",
                                timeout=args.timeout, check=False)
        report["commands"].append({"command": command, "exit_code": result.returncode,
                                   "stdout": result.stdout, "stderr": result.stderr})
        if result.returncode:
            raise ValueError("nested source build, proof or installation failed")

    try:
        compiler, linker = shutil.which(args.compiler), shutil.which(args.linker)
        wine = shutil.which(args.wine) if os.name != "nt" else None
        if not compiler or not linker or os.name != "nt" and not wine:
            raise ValueError("compiler, linker or Wine unavailable")
        libraries, report["runtime_libraries"] = load_libraries(args.runtime_libs.resolve())
        report["source_sha256"], report["emitter_sha256"] = file_digest(SOURCE), file_digest(EMITTER)
        report["proof_sha256"] = file_digest(PROOF)
        test, patch = args.test_binary.resolve(), args.patch_binary.resolve()
        for kind, optimization in FORMS.items():
            for control in (False, True):
                name = kind + ("-control" if control else "")
                case = out / name
                case.mkdir(exist_ok=True)
                run([compiler, "--target=i686-pc-windows-msvc", "-fms-extensions", "-fexceptions",
                     "-fcxx-exceptions", "-fno-omit-frame-pointer", optimization,
                     "-DEXPECTED_FIRST=" + ("18" if control else "17"), "-c", SOURCE,
                     "-o", case / "driver.obj"])
                original, product = case / "original.exe", case / "product.exe"
                run([linker, "/entry:mainCRTStartup", "/nodefaultlib", "/machine:x86", "/subsystem:console",
                     "/fixed:no", "/dynamicbase:no", "/out:" + str(original),
                     case / "driver.obj", *libraries])
                initial = run_image(original, [wine] if wine else [], env, args.timeout)
                report["commands"].append({"original_runtime": initial})
                if initial.get("exit_code") != int(control):
                    raise ValueError("original nested fixture failed")
                run([test, "--gtest_filter=WindowsRegistrationNestedTry.InputPE32ReconstructsNestedSearch",
                     "--gtest_output=xml:" + str(case / "rewrite.xml")],
                    {"NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32": str(original),
                     "NEVERD_REGISTRATION_REALIGNED_OUTPUT_PE32": str(product),
                     "NEVERD_REGISTRATION_REALIGNED_RECEIPT": str(case / "contract.json"),
                     "NEVERD_REGISTRATION_OUTPUT_IR": str(case / "source.ll")})
                if require_test_result(case / "rewrite.xml") != 1:
                    raise ValueError("nested source reconstruction check missing")
                receipt = json.loads((case / "contract.json").read_text())
                validate_installation(PE32(original.read_bytes()), PE32(product.read_bytes()), receipt, name)
                for mode in ("section", "inplace"):
                    run([patch, "patch", original, "--from-ir=" + str(case / "source.ll"),
                         "--mode=" + mode, "--no-opt", "-o", case / ("cli-" + mode + ".exe")])
                    if (case / ("cli-" + mode + ".exe")).read_bytes() != product.read_bytes():
                        raise ValueError("CLI installation differs from checked nested transaction")
                record = {"case": name, "contract_sha256": file_digest(case / "contract.json"),
                          "ir_sha256": file_digest(case / "source.ll"),
                          "object_sha256": file_digest(case / "driver.obj"),
                          "decompilation": {}, "images": []}
                report["cases"].append(record)
                for language in ("c", "cpp"):
                    source = case / ("decompiled." + language)
                    run([patch, "decompile", original,
                         "--func=" + hex(receipt["base"] + receipt["source_begin"]),
                         "--language=" + language, "-o", source])
                    validate_decompilation(source.read_text(), language)
                    if language == "c":
                        run([compiler, "-x", "c", "-std=c11", "-fsyntax-only",
                             "-Werror=implicit-function-declaration", source])
                    record["decompilation"][language] = file_digest(source)
                for route in ROUTES:
                    source = case / (route + ".exe")
                    rebased = case / (route + "-rebased.exe")
                    rebased.write_bytes(PE32(source.read_bytes()).rebase(BASES[1]))
                    for image in (source, rebased):
                        record["images"].append(observe(image, name, route, receipt,
                                                        [wine] if wine else [], env, args.timeout))
                print("PASS nested tries " + name + ": 8 executions, 12 dispatches each", flush=True)
        report["passed"] = True
    except (OSError, ValueError, KeyError, TypeError, struct.error,
            subprocess.TimeoutExpired, ET.ParseError) as error:
        report["error"] = str(error)
    path = out / "nested-try-rewrite.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "report": str(path), "error": report.get("error")}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
