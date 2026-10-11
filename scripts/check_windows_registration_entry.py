#!/usr/bin/env python3
"""Reconstruct and execute PE32 C++ parents with physical entry arguments."""
from __future__ import annotations

import argparse
import hashlib
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
        BASES, ROUTES, PE32, code_owner, file_digest, require_test_result, run_image)
    from .check_windows_registration_nested_try import validate_decompilation
    from .windows_registration_libraries import load_libraries
    from .windows_registration_runtime import NAME, load_runtime
else:
    from check_windows_registration_multiple_catch import (
        BASES, ROUTES, PE32, code_owner, file_digest, require_test_result, run_image)
    from check_windows_registration_nested_try import validate_decompilation
    from windows_registration_libraries import load_libraries
    from windows_registration_runtime import NAME, load_runtime

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_entry_driver.cpp"
PROOF = ROOT / "unittests/lift/eh/RegistrationEntryABITests.cpp"
RECEIPT = ROOT / "unittests/lift/eh/RegistrationSourceReceiptTestUtils.cpp"
ABIS = {"cdecl": (0, 0), "stdcall": (0, 16), "thiscall": (1, 12), "fastcall": (2, 8)}
FORMS = {abi + "-" + mode: (abi, "-" + mode.upper())
         for abi in ABIS for mode in ("o0", "o1")}
CASES = tuple(name + suffix for name in FORMS for suffix in ("", "-control"))
OBSERVATION = re.compile(r"ENTRY " + r"([0-9A-F]{8}) " * 13 + r"([0-9A-F]{8})\r?\n")
EXPECTED = (309, 336, 363, 389, 7, 18, 39, 49, 1, 16, 16)


def entry_context(case):
    abi, _ = FORMS[case.removesuffix("-control")]
    registers, pop = ABIS[abi]
    export = ("_callback_parent@16" if abi == "stdcall" else
              "@callback_parent@16" if abi == "fastcall" else "callback_parent")
    return registers, pop, export.encode("ascii")


def validate_installation(original, product, receipt, case):
    registers, pop, export = entry_context(case)
    if receipt.get("schema") != 1 or \
            receipt.get("evidence") != "checked-realigned-source-reconstruction" or \
            receipt.get("base") != BASES[0] or original.base != BASES[0] or \
            product.base != BASES[0] or \
            receipt.get("source_image_sha256") != hashlib.sha256(original.data).hexdigest() or \
            receipt.get("image_sha256") != hashlib.sha256(product.data).hexdigest():
        raise ValueError("entry installation lost its proved image identity")
    if receipt.get("source_frame") != "fixed-displaced" or \
            (receipt.get("entry_registers"), receipt.get("entry_pop")) != (registers, pop) or \
            type(receipt.get("incoming_reads")) is not int or receipt["incoming_reads"] <= 0 or \
            receipt.get("incoming_writes") != 0:
        raise ValueError("entry receipt lost its physical parameter or frame contract")
    entry = original.entry(export)
    if entry != receipt["source_begin"] or product.entry(export) != entry:
        raise ValueError("entry installation changed its decorated export")
    at = product.raw(entry, 5)
    target = (entry + 5 + struct.unpack_from("<i", product.data, at + 1)[0]) & 0xffffffff
    if product.data[at] != 0xe9 or target != receipt["generated_begin"]:
        raise ValueError("entry trampoline lost its generated owner")
    code_owner(original, entry, receipt["source_end"], ".text")
    code_owner(product, target, receipt["generated_end"], ".ndtext")


def observe(path, case, route, receipt, launcher, env, timeout):
    image = PE32(path.read_bytes())
    if image.base not in BASES or image.u16(image.optional + 70) & 0x40:
        raise ValueError("entry runtime probe has no forced base")
    result = run_image(path, launcher, env, timeout)
    match = OBSERVATION.fullmatch(result.get("stdout", ""))
    if result.get("exit_code") != int(case.endswith("-control")) or not match:
        raise ValueError("entry runtime or negative control failed")
    values = tuple(int(value, 16) for value in match.groups())
    if values[:11] != EXPECTED:
        raise ValueError("entry arguments, returns, ESP or FS chain differ")
    owner = "source" if route == "original" else "generated"
    begin, end = receipt[owner + "_begin"], receipt[owner + "_end"]
    callers = [value - image.base for value in values[11:]]
    if any(not begin <= caller < end for caller in callers):
        raise ValueError("entry throw caller is outside its exact owner")
    code_owner(image, begin, end, ".text" if route == "original" else ".ndtext")
    return {"image": path.name, "sha256": file_digest(path), "route": route,
            "base": image.base, "expected_exit": int(case.endswith("-control")),
            "caller_rvas": callers, "runtime": result}


def main():
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
    report = {"schema": 1, "evidence": "entry-abi-source-reconstruction",
              "passed": False, "commands": [], "cases": []}

    def run(command, extra=None):
        command = list(map(str, command))
        result = subprocess.run(command, env=env | (extra or {}), cwd=out,
                                capture_output=True, text=True, errors="replace",
                                timeout=args.timeout, check=False)
        report["commands"].append({"command": command, "exit_code": result.returncode,
                                   "stdout": result.stdout, "stderr": result.stderr})
        if result.returncode:
            raise ValueError("entry build, source proof or installation failed")

    try:
        compiler, linker = shutil.which(args.compiler), shutil.which(args.linker)
        wine = shutil.which(args.wine) if os.name != "nt" else None
        if not compiler or not linker or os.name != "nt" and not wine:
            raise ValueError("compiler, linker or Wine unavailable")
        libraries, report["runtime_libraries"] = load_libraries(args.runtime_libs.resolve())
        runtime, report["catch_search_runtime"] = load_runtime(args.runtime_libs.resolve())
        report["source_sha256"] = file_digest(SOURCE)
        report["proof_sha256"] = file_digest(PROOF)
        report["receipt_sha256"] = file_digest(RECEIPT)
        test, patch = args.test_binary.resolve(), args.patch_binary.resolve()
        for kind, (abi, optimization) in FORMS.items():
            for control in (False, True):
                name = kind + ("-control" if control else "")
                case = out / name
                case.mkdir(exist_ok=True)
                shutil.copyfile(runtime, case / NAME)
                registers, pop, _ = entry_context(name)
                run([compiler, "--target=i686-pc-windows-msvc", "-fms-extensions", "-fexceptions",
                     "-fcxx-exceptions", "-fno-omit-frame-pointer", optimization,
                     "-DENTRY_ABI=__" + abi, "-DEXPECTED_BIAS=" + str(int(control)),
                     "-c", SOURCE, "-o", case / "driver.obj"])
                original, product = case / "original.exe", case / "product.exe"
                run([linker, "/entry:mainCRTStartup", "/nodefaultlib", "/machine:x86", "/subsystem:console",
                     "/fixed:no", "/dynamicbase:no", "/out:" + str(original),
                     case / "driver.obj", *libraries])
                run([test, "--gtest_filter=RegistrationEntryABI.InputPE32PreservesPhysicalArgumentsAndCleanup",
                     "--gtest_output=xml:" + str(case / "rewrite.xml")],
                    {"NEVERD_REGISTRATION_ENTRY_PE32": str(original),
                     "NEVERD_REGISTRATION_ENTRY_POP": str(pop),
                     "NEVERD_REGISTRATION_ENTRY_REGISTERS": str(registers),
                     "NEVERD_REGISTRATION_ENTRY_OUTPUT": str(product),
                     "NEVERD_REGISTRATION_REALIGNED_RECEIPT": str(case / "contract.json"),
                     "NEVERD_REGISTRATION_OUTPUT_IR": str(case / "source.ll")})
                if require_test_result(case / "rewrite.xml") != 1:
                    raise ValueError("entry source reconstruction check missing")
                receipt = json.loads((case / "contract.json").read_text())
                validate_installation(PE32(original.read_bytes()), PE32(product.read_bytes()), receipt, name)
                for mode in ("section", "inplace"):
                    run([patch, "patch", original, "--from-ir=" + str(case / "source.ll"),
                         "--mode=" + mode, "--no-opt", "-o", case / ("cli-" + mode + ".exe")])
                    if (case / ("cli-" + mode + ".exe")).read_bytes() != product.read_bytes():
                        raise ValueError("CLI entry installation differs from the checked transaction")
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
                    run([compiler, "-x", "c" if language == "c" else "c++",
                         "-std=c11" if language == "c" else "-std=c++17",
                         "-fsyntax-only", source])
                    record["decompilation"][language] = file_digest(source)
                for route in ROUTES:
                    source = case / (route + ".exe")
                    rebased = case / (route + "-rebased.exe")
                    rebased.write_bytes(PE32(source.read_bytes()).rebase(BASES[1]))
                    for image in (source, rebased):
                        record["images"].append(observe(image, name, route, receipt,
                                                        [wine] if wine else [], env, args.timeout))
                print("PASS entry " + name + ": 8 executions, 16 calls each", flush=True)
        report["passed"] = True
    except (OSError, ValueError, KeyError, TypeError, struct.error,
            subprocess.TimeoutExpired, ET.ParseError) as error:
        report["error"] = str(error)
    path = out / "entry-rewrite.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "report": str(path), "error": report.get("error")}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
