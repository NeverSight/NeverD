#!/usr/bin/env python3
"""Reconstruct and execute ordered PE32 value/reference/catch-all clauses."""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess

if __package__:
    from .check_windows_registration_eh import run_image
    from .check_windows_registration_realigned import require_test_result
    from .check_windows_registration_realigned_rewrite import (
        BASES, ROUTES, file_digest, validate_installation,
    )
    from .check_windows_registration_cxx_rewrite import code_owner
    from .check_windows_registration_rewrite import PE32
    from .windows_registration_libraries import load_libraries
else:
    from check_windows_registration_eh import run_image
    from check_windows_registration_realigned import require_test_result
    from check_windows_registration_realigned_rewrite import (
        BASES, ROUTES, file_digest, validate_installation,
    )
    from check_windows_registration_cxx_rewrite import code_owner
    from check_windows_registration_rewrite import PE32
    from windows_registration_libraries import load_libraries

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_multiple_catch_driver.cpp"
EMITTER = ROOT / "unittests/lift/eh/WindowsRegistrationMultipleCatchTests.cpp"
FORMS = {"ordered-llvm-fixed": "Fixed", "ordered": "Realigned"}
CASES = tuple(name + suffix for name in FORMS for suffix in ("", "-control"))
OBSERVATION = re.compile(r"MULTICATCH " + r"([0-9A-F]{8}) " * 10 + r"([0-9A-F]{8})\r?\n")


def validate_decompilation(text: str, language: str) -> None:
    if "highir.structured_regions=1, fallback_regions=0" not in text or \
            text.count("= __neverd_x86_callback_esp(0x") != 3:
        raise ValueError("ordered catch decompilation lost a runtime body")
    if language == "c" and text.count("Native x86 callback @") != 3:
        raise ValueError("ordered C output lost a callback entry")
    if language == "cpp" and (text.count("catch (") != 3 or ".Value" in text):
        raise ValueError("ordered C++ output lost a clause or invented a catch field")
    if len(set(re.findall(r"goto L_([0-9A-F]+);", text))) != 3:
        raise ValueError("ordered catch output lost its distinct continuations")


def observe(path: Path, case: str, route: str, receipt: dict,
            launcher: list[str], env: dict[str, str], timeout: float,
            *, expected_values: tuple = (17, 28, 39, 7, 18, 39, 1, 12),
            expected_cleanup: tuple | None = None) -> dict:
    image = PE32(path.read_bytes())
    if image.base not in BASES or image.u16(image.optional + 70) & 0x40:
        raise ValueError("multiple-catch probe has no forced base")
    result = run_image(path, launcher, env, timeout)
    output = result.get("stdout", "")
    if expected_cleanup is not None:
        output, separator, cleanup = output.rpartition("CLEANUP ")
        matched = re.fullmatch(r"([0-9A-F]{8}) ([0-9A-F]{8}) ([0-9A-F]{8})\r?\n", cleanup)
        if not separator or not matched or \
                tuple(int(v, 16) for v in matched.groups()) != expected_cleanup:
            raise ValueError("catch cleanup order or count differs")
    match = OBSERVATION.fullmatch(output)
    if result.get("exit_code") != int(case.endswith("-control")) or not match:
        raise ValueError("multiple-catch runtime or negative control failed")
    values = [int(v, 16) for v in match.groups()]
    if tuple(values[:8]) != expected_values:
        raise ValueError("catch dispatch, reference effect or registration chain differs")
    owner = "source" if route == "original" else "generated"
    begin, end = receipt[owner + "_begin"], receipt[owner + "_end"]
    callers = [value - image.base for value in values[8:]]
    if any(not begin <= caller < end for caller in callers):
        raise ValueError("throw caller is outside the exact parent owner")
    code_owner(image, begin, end, ".text" if route == "original" else ".ndtext")
    return {"image": path.name, "sha256": file_digest(path), "route": route,
            "base": image.base, "expected_exit": int(case.endswith("-control")),
            "caller_rvas": callers, "runtime": result}


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
    report = {"schema": 1, "evidence": "ordered-catch-source-reconstruction",
              "passed": False, "commands": [], "objects": {}, "cases": []}

    def run(command, extra=None):
        command = list(map(str, command))
        result = subprocess.run(command, env=env | (extra or {}), cwd=out,
                                capture_output=True, text=True, errors="replace",
                                timeout=args.timeout, check=False)
        report["commands"].append({"command": command, "exit_code": result.returncode,
                                   "stdout": result.stdout, "stderr": result.stderr})
        if result.returncode:
            raise ValueError("multiple-catch build, proof or installation failed")

    try:
        compiler, linker = shutil.which(args.compiler), shutil.which(args.linker)
        wine = shutil.which(args.wine) if os.name != "nt" else None
        if not compiler or not linker or os.name != "nt" and not wine:
            raise ValueError("compiler, linker or Wine unavailable")
        libraries, report["runtime_libraries"] = load_libraries(args.runtime_libs.resolve())
        report["source_sha256"], report["emitter_sha256"] = file_digest(SOURCE), file_digest(EMITTER)
        test, patch = args.test_binary.resolve(), args.patch_binary.resolve()
        for kind, emitter in FORMS.items():
            run([test, "--gtest_filter=WindowsRegistrationMultipleCatch.Emits" + emitter + "Callbacks",
                 "--gtest_output=xml:" + str(out / (kind + "-emit.xml"))],
                {"NEVERD_REGISTRATION_MULTIPLE_OBJECT": str(out / (kind + ".obj"))})
            if require_test_result(out / (kind + "-emit.xml")) != 1:
                raise ValueError("multiple-catch emitter check missing")
            report["objects"][kind] = file_digest(out / (kind + ".obj"))
            for control in (False, True):
                name = kind + ("-control" if control else "")
                case = out / name
                case.mkdir(exist_ok=True)
                run([compiler, "--target=i686-pc-windows-msvc", "-fms-extensions", "-fexceptions",
                     "-fcxx-exceptions", "-fno-omit-frame-pointer", "-O1",
                     "-DEXPECTED_FIRST=" + ("18" if control else "17"), "-c", SOURCE,
                     "-o", case / "driver.obj"])
                original, product = case / "original.exe", case / "product.exe"
                run([linker, "/entry:mainCRTStartup", "/nodefaultlib", "/machine:x86", "/subsystem:console",
                     "/fixed:no", "/dynamicbase:no", "/out:" + str(original),
                     case / "driver.obj", out / (kind + ".obj"), *libraries])
                # Check the fixture before attributing a failure to reconstruction.
                initial = run_image(original, [wine] if wine else [], env, args.timeout)
                report["commands"].append({"original_runtime": initial})
                if initial.get("exit_code") != int(control) or not OBSERVATION.fullmatch(initial.get("stdout", "")):
                    raise ValueError("original multiple-catch fixture failed")
                run([test, "--gtest_filter=WindowsRegistrationMultipleCatch.InputPE32ReconstructsOrderedCatches",
                     "--gtest_output=xml:" + str(case / "rewrite.xml")],
                    {"NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32": str(original),
                     "NEVERD_REGISTRATION_REALIGNED_OUTPUT_PE32": str(product),
                     "NEVERD_REGISTRATION_REALIGNED_RECEIPT": str(case / "contract.json"),
                     "NEVERD_REGISTRATION_OUTPUT_IR": str(case / "source.ll")})
                if require_test_result(case / "rewrite.xml") != 1:
                    raise ValueError("ordered source reconstruction check missing")
                receipt = json.loads((case / "contract.json").read_text())
                validate_installation(PE32(original.read_bytes()), PE32(product.read_bytes()), receipt, name)
                for mode in ("section", "inplace"):
                    run([patch, "patch", original, "--from-ir=" + str(case / "source.ll"),
                         "--mode=" + mode, "--no-opt", "-o", case / ("cli-" + mode + ".exe")])
                    if (case / ("cli-" + mode + ".exe")).read_bytes() != product.read_bytes():
                        raise ValueError("CLI installation differs from checked transaction")
                record = {"case": name, "contract_sha256": file_digest(case / "contract.json"),
                          "ir_sha256": file_digest(case / "source.ll"), "images": []}
                report["cases"].append(record)
                record["decompilation"] = {}
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
                print("PASS ordered catches " + name + ": 8 executions, 12 dispatches each", flush=True)
        report["passed"] = True
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    path = out / "multiple-catch-rewrite.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "report": str(path), "error": report.get("error")}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
