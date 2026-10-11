#!/usr/bin/env python3
"""Re-lift and reconstruct authenticated PE32 products, then execute both generations."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import xml.etree.ElementTree as ET

if __package__:
    from .check_windows_registration_entry import (
        BASES, CASES, RECEIPT, ROOT, ROUTES, PE32, code_owner, entry_context,
        file_digest, observe, require_test_result)
    from .replay_windows_registration_entry import validate_capture as validate_first_capture
    from .windows_registration_runtime import NAME, validate_runtime
else:
    from check_windows_registration_entry import (
        BASES, CASES, RECEIPT, ROOT, ROUTES, PE32, code_owner, entry_context,
        file_digest, observe, require_test_result)
    from replay_windows_registration_entry import validate_capture as validate_first_capture
    from windows_registration_runtime import NAME, validate_runtime

PROOF = ROOT / "unittests/lift/eh/RegistrationReliftTests.cpp"


def jump_target(image, entry):
    offset = image.raw(entry, 5)
    if image.data[offset] != 0xe9:
        raise ValueError("re-lifted entry lost its exact trampoline")
    return (entry + 5 + struct.unpack_from("<i", image.data, offset + 1)[0]) & 0xffffffff


def validate_installation(original, product, receipt, first, case):
    registers, pop, export = entry_context(case)
    if receipt.get("schema") != 1 or \
            receipt.get("evidence") != "checked-realigned-source-reconstruction" or \
            receipt.get("source_frame") != "realigned" or \
            receipt.get("base") != BASES[0] or original.base != BASES[0] or \
            product.base != BASES[0] or \
            receipt.get("source_image_sha256") != hashlib.sha256(original.data).hexdigest() or \
            receipt.get("image_sha256") != hashlib.sha256(product.data).hexdigest() or \
            (receipt.get("entry_registers"), receipt.get("entry_pop")) != (registers, pop) or \
            type(receipt.get("incoming_reads")) is not int or receipt["incoming_reads"] <= 0 or \
            receipt.get("incoming_writes") != 0:
        raise ValueError("re-lifted installation lost its image, frame or entry ABI proof")
    if (receipt["source_begin"], receipt["source_end"]) != \
            (first["generated_begin"], first["generated_end"]) or \
            receipt["source_image_sha256"] != first["image_sha256"]:
        raise ValueError("re-lifted source is not the proved first generation")
    entry = original.entry(export)
    if product.entry(export) != entry or entry != first["source_begin"] or \
            jump_target(original, entry) != receipt["source_begin"] or \
            jump_target(product, entry) != receipt["source_begin"] or \
            jump_target(product, receipt["source_begin"]) != receipt["generated_begin"] or \
            receipt["generated_begin"] < receipt["source_end"]:
        raise ValueError("two-generation trampoline chain changed its source or destination")
    code_owner(original, receipt["source_begin"], receipt["source_end"], ".ndtext")
    code_owner(product, receipt["generated_begin"], receipt["generated_end"], ".ndtext")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-root", type=Path, required=True)
    parser.add_argument("--test-binary", type=Path, required=True)
    parser.add_argument("--patch-binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--wine", default="wine")
    parser.add_argument("--timeout", type=float, default=90)
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    source, out = args.input_root.resolve(), args.output.resolve()
    if source == out or source in out.parents or out in source.parents:
        parser.error("input and output evidence directories must be separate")
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy() | {"WINEDEBUG": "-all", "WINEDLLOVERRIDES": "vcruntime140=n"}
    report = {"schema": 1, "evidence": "relifted-entry-reconstruction", "passed": False,
              "proof_sha256": file_digest(PROOF), "receipt_sha256": file_digest(RECEIPT),
              "commands": [], "cases": []}

    def run(command, extra=None):
        command = list(map(str, command))
        result = subprocess.run(command, env=env | (extra or {}), cwd=out,
                                capture_output=True, text=True, errors="replace",
                                timeout=args.timeout, check=False)
        report["commands"].append({"command": command, "exit_code": result.returncode,
                                   "stdout": result.stdout, "stderr": result.stderr})
        if result.returncode:
            raise ValueError("re-lifted source proof or installation failed")

    try:
        first = json.loads((source / "entry-rewrite.json").read_text())
        validate_first_capture(source, first)
        wine = shutil.which(args.wine) if os.name != "nt" else None
        if os.name != "nt" and not wine:
            raise ValueError("native Windows or Wine is required")
        shutil.copytree(source, out / "first", dirs_exist_ok=True)
        report["first_capture_sha256"] = file_digest(out / "first/entry-rewrite.json")
        report["catch_search_runtime"] = first["catch_search_runtime"]
        test, patch = args.test_binary.resolve(), args.patch_binary.resolve()
        for name in CASES:
            case = out / name
            case.mkdir(exist_ok=True)
            previous = out / "first" / name
            first_receipt = json.loads((previous / "contract.json").read_text())
            shutil.copyfile(previous / "product.exe", case / "original.exe")
            shutil.copyfile(validate_runtime(previous, report["catch_search_runtime"]), case / NAME)
            registers, pop, _ = entry_context(name)
            original, product = case / "original.exe", case / "product.exe"
            run([test, "--gtest_filter=RegistrationRelift.InputPE32ReconstructsGeneratedFunction",
                 "--gtest_output=xml:" + str(case / "rewrite.xml")],
                {"NEVERD_REGISTRATION_RELIFT_PE32": str(original),
                 "NEVERD_REGISTRATION_RELIFT_ENTRY": hex(BASES[0] + first_receipt["generated_begin"]),
                 "NEVERD_REGISTRATION_ENTRY_POP": str(pop),
                 "NEVERD_REGISTRATION_ENTRY_REGISTERS": str(registers),
                 "NEVERD_REGISTRATION_RELIFT_OUTPUT": str(product),
                 "NEVERD_REGISTRATION_REALIGNED_RECEIPT": str(case / "contract.json"),
                 "NEVERD_REGISTRATION_OUTPUT_IR": str(case / "source.ll")})
            if require_test_result(case / "rewrite.xml") != 1:
                raise ValueError("re-lifted source proof was not executed")
            receipt = json.loads((case / "contract.json").read_text())
            validate_installation(PE32(original.read_bytes()), PE32(product.read_bytes()),
                                  receipt, first_receipt, name)
            for mode in ("section", "inplace"):
                output = case / ("cli-" + mode + ".exe")
                run([patch, "patch", original, "--from-ir=" + str(case / "source.ll"),
                     "--mode=" + mode, "--no-opt", "-o", output])
                if output.read_bytes() != product.read_bytes():
                    raise ValueError("public re-lifted installation differs from the proved transaction")
            record = {"case": name, "contract_sha256": file_digest(case / "contract.json"),
                      "ir_sha256": file_digest(case / "source.ll"), "images": []}
            report["cases"].append(record)
            for route in ROUTES:
                path = case / (route + ".exe")
                rebased = case / (route + "-rebased.exe")
                rebased.write_bytes(PE32(path.read_bytes()).rebase(BASES[1]))
                for image in (path, rebased):
                    record["images"].append(observe(
                        image, name, route, receipt, [wine] if wine else [], env,
                        args.timeout, source_section=".ndtext"))
            print("PASS re-lift " + name + ": 8 executions, 16 calls each", flush=True)
        report["passed"] = True
    except (OSError, ValueError, KeyError, TypeError, struct.error,
            subprocess.TimeoutExpired, ET.ParseError) as error:
        report["error"] = str(error)
    path = out / "relift-rewrite.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "report": str(path), "error": report.get("error")}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
