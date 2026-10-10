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
        BASES, ROUTES, PE32, file_digest, observe as observe_catches, require_test_result,
        run_image, validate_installation)
    from .windows_registration_libraries import load_libraries
    from .windows_registration_runtime import NAME, load_runtime
else:
    from check_windows_registration_multiple_catch import (
        BASES, ROUTES, PE32, file_digest, observe as observe_catches, require_test_result,
        run_image, validate_installation)
    from windows_registration_libraries import load_libraries
    from windows_registration_runtime import NAME, load_runtime

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_nested_try_driver.cpp"
EMITTER = ROOT / "unittests/lift/eh/WindowsRegistrationNestedTryTests.cpp"
PROOF = ROOT / "unittests/lift/eh/RegistrationNestedTryTestUtils.cpp"
RETHROW_PROOF = ROOT / "unittests/lift/eh/RegistrationRethrowTestUtils.cpp"
DIRECT_PROOF = ROOT / "unittests/lift/eh/RegistrationDirectThrowTestUtils.cpp"
CATCH_PROOF = ROOT / "unittests/lift/eh/WindowsRegistrationCatchContextTests.cpp"
RECEIPT_PROOF = ROOT / "unittests/lift/eh/RegistrationSourceReceiptTestUtils.cpp"
FORMS = {prefix + "-" + mode + "-llvm-fixed": "-" + mode.upper()
         for prefix in ("nested", "secondary", "rethrow", "inline-rethrow",
                        "direct-nested", "direct-secondary", "direct-rethrow", "catch-try")
         for mode in ("o0", "o1")}
CASES = tuple(name + suffix for name in FORMS for suffix in ("", "-control"))


def search_context(case):
    family, separator, mode = case.removesuffix("-control").removesuffix("-llvm-fixed").rpartition("-")
    if not separator or mode not in ("o0", "o1") or family not in (
            "nested", "secondary", "rethrow", "inline-rethrow", "direct-nested",
            "direct-secondary", "direct-rethrow", "catch-try"):
        raise ValueError("unknown nested throw profile")
    rethrow = family in ("rethrow", "inline-rethrow", "direct-rethrow")
    return {"secondary_search": rethrow or family in ("secondary", "direct-secondary"),
            "rethrow_search": rethrow,
            "inline_rethrow": family in ("inline-rethrow", "direct-rethrow"),
            "direct_throw": family.startswith("direct-"),
            "catch_try": family == "catch-try"}


def validate_search_context(receipt, case):
    context = search_context(case)
    if any(receipt.get(key) is not value for key, value in context.items()):
        raise ValueError("nested source proof has the wrong catch search context")
    return context


def observe(path, case, route, receipt, launcher, env, timeout):
    context = validate_search_context(receipt, case)
    rethrow, secondary = context["rethrow_search"], context["secondary_search"]
    expected = ((39, 28, 39, 39, 18, 39, 1, 12) if rethrow else
                (17, 17, 39, 7, 7, 39, 1, 12) if secondary else
                (17, 28, 39, 7, 18, 39, 1, 12))
    return observe_catches(path, case, route, receipt, launcher, env, timeout,
                           expected_values=expected)


def validate_decompilation(text: str, language: str, inline: bool = False,
                           direct: bool = False, catch_try: bool = False) -> None:
    regions, callbacks = (3, 4) if catch_try else (2, 3)
    if f"highir.structured_regions={regions}, fallback_regions=0" not in text or \
            text.count("= __neverd_x86_callback_esp(0x") != callbacks:
        raise ValueError("nested decompilation lost a language region or callback")
    if language == "c" and text.count("Native x86 callback @") != callbacks:
        raise ValueError("nested C output lost a callback entry")
    if language == "cpp" and (text.count("catch (") != callbacks or
                               len(re.findall(r"\btry \{", text)) != regions or ".Value" in text):
        raise ValueError("nested C++ output lost a try or invented an object field")
    if inline and language == "cpp" and len(re.findall(r"\bthrow;", text)) != 1:
        raise ValueError("direct rethrow lost its current exception")
    if direct and language == "cpp" and any("throw (" + spelling + ")" not in text
                                                for spelling in ("int", "unsigned int", "float")):
        raise ValueError("direct typed throw output lost its arguments")
    if len(set(re.findall(r"goto L_([0-9A-F]+);", text))) < callbacks:
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
    env = os.environ.copy() | {"WINEDEBUG": "-all", "WINEDLLOVERRIDES": "vcruntime140=n"}
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
        runtime, report["catch_search_runtime"] = load_runtime(args.runtime_libs.resolve())
        report["source_sha256"], report["emitter_sha256"] = file_digest(SOURCE), file_digest(EMITTER)
        report["proof_sha256"] = file_digest(PROOF)
        report["rethrow_proof_sha256"] = file_digest(RETHROW_PROOF)
        report["direct_proof_sha256"] = file_digest(DIRECT_PROOF)
        report["catch_proof_sha256"] = file_digest(CATCH_PROOF)
        report["receipt_proof_sha256"] = file_digest(RECEIPT_PROOF)
        test, patch = args.test_binary.resolve(), args.patch_binary.resolve()
        for kind, optimization in FORMS.items():
            for control in (False, True):
                name = kind + ("-control" if control else "")
                case = out / name
                case.mkdir(exist_ok=True)
                shutil.copyfile(runtime, case / NAME)
                context = search_context(kind)
                inline, rethrow, secondary, direct = (
                    context["inline_rethrow"], context["rethrow_search"],
                    context["secondary_search"], context["direct_throw"])
                catch_try = context["catch_try"]
                first = (39 if rethrow else 17) + int(control)
                run([compiler, "--target=i686-pc-windows-msvc", "-fms-extensions", "-fexceptions",
                     "-fcxx-exceptions", "-fno-omit-frame-pointer", optimization,
                     *(["-DDIRECT_TYPED_THROW"] if direct else []),
                     *(["-DCATCH_TRY"] if catch_try else []),
                     *(["-DINLINE_RETHROW_SEARCH"] if inline else []),
                     *(["-DRETHROW_SEARCH"] if rethrow else
                       ["-DSECONDARY_SEARCH"] if secondary else []),
                     "-DEXPECTED_FIRST=" + str(first), "-c", SOURCE,
                     "-o", case / "driver.obj"])
                original, product = case / "original.exe", case / "product.exe"
                run([linker, "/entry:mainCRTStartup", "/nodefaultlib", "/machine:x86", "/subsystem:console",
                     "/fixed:no", "/dynamicbase:no", "/out:" + str(original),
                     case / "driver.obj", *libraries])
                initial = run_image(original, [wine] if wine else [], env, args.timeout)
                report["commands"].append({"original_runtime": initial})
                if initial.get("exit_code") != int(control):
                    raise ValueError("original nested fixture failed")
                test_case = (("DirectThrowAndRethrow" if rethrow else
                              "DirectSecondaryThrow" if secondary else "DirectThrows") if direct else
                             "InlineRethrow" if inline else "Rethrow" if rethrow else
                             "SecondarySearch" if secondary else "NestedSearch")
                test_filter = ("WindowsRegistrationCatchContext.InputPE32RestoresTheOuterReferenceCatch"
                               if catch_try else
                               "WindowsRegistrationNestedTry.InputPE32Reconstructs" + test_case)
                run([test, "--gtest_filter=" + test_filter,
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
                    validate_decompilation(source.read_text(), language, inline, direct, catch_try)
                    if language == "c":
                        run([compiler, "-x", "c", "-std=c11", "-fsyntax-only",
                             "-Werror=implicit-function-declaration", source])
                    else:
                        run([compiler, "-x", "c++", "-std=c++17",
                             "-fsyntax-only", source])
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
