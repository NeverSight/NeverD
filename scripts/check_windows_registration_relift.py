#!/usr/bin/env python3
"""Re-lift and reconstruct authenticated PE32 products, then execute both generations."""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import xml.etree.ElementTree as ET
from types import SimpleNamespace

if __package__:
    from .check_windows_registration_entry import (
        BASES, CASES, RECEIPT, ROOT, ROUTES, PE32, entry_context,
        file_digest, observe, require_test_result)
    from .replay_windows_registration_entry import validate_capture as validate_first_capture
    from .windows_registration_runtime import NAME, validate_runtime
    from .windows_registration_image import jump_target, validate_generation
else:
    from check_windows_registration_entry import (
        BASES, CASES, RECEIPT, ROOT, ROUTES, PE32, entry_context,
        file_digest, observe, require_test_result)
    from replay_windows_registration_entry import validate_capture as validate_first_capture
    from windows_registration_runtime import NAME, validate_runtime
    from windows_registration_image import jump_target, validate_generation

PROOF = ROOT / "unittests/lift/eh/RegistrationReliftTests.cpp"


def validate_installation(original, product, receipt, first, case):
    registers, pop, export = entry_context(case)
    validate_generation(original, product, receipt, first, export)
    if (receipt.get("entry_registers"), receipt.get("entry_pop")) != (registers, pop) or \
            type(receipt.get("incoming_reads")) is not int or receipt["incoming_reads"] <= 0 or \
            receipt.get("incoming_writes") != 0:
        raise ValueError("re-lifted installation lost its entry ABI proof")


def get_profile(name="entry"):
    if name == "entry":
        return SimpleNamespace(cases=CASES, first_capture="entry-rewrite.json",
                               evidence="relifted-entry-reconstruction", calls=16,
                               entry_context=entry_context, observe=observe,
                               validate_first=validate_first_capture,
                               validate_installation=validate_installation)
    if name == "objects":
        if __package__:
            from .windows_registration_objects_relift import profile
        else:
            from windows_registration_objects_relift import profile
        return profile()
    if name != "catch-cleanup":
        raise ValueError("unknown re-lift evidence profile")
    if __package__:
        from .windows_registration_nested_relift import profile
    else:
        from windows_registration_nested_relift import profile
    return profile()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("entry", "catch-cleanup", "objects"), default="entry")
    parser.add_argument("--input-root", type=Path, required=True)
    parser.add_argument("--test-binary", type=Path, required=True)
    parser.add_argument("--patch-binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--wine", default="wine")
    parser.add_argument("--timeout", type=float, default=90)
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    profile = get_profile(args.profile)
    source, out = args.input_root.resolve(), args.output.resolve()
    if source == out or source in out.parents or out in source.parents:
        parser.error("input and output evidence directories must be separate")
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy() | {"WINEDEBUG": "-all", "WINEDLLOVERRIDES": "vcruntime140=n"}
    report = {"schema": 1, "evidence": profile.evidence, "profile": args.profile, "passed": False,
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
        first = json.loads((source / profile.first_capture).read_text())
        profile.validate_first(source, first)
        wine = shutil.which(args.wine) if os.name != "nt" else None
        if os.name != "nt" and not wine:
            raise ValueError("native Windows or Wine is required")
        shutil.copytree(source, out / "first", dirs_exist_ok=True)
        report["first_capture_sha256"] = file_digest(out / "first" / profile.first_capture)
        report["catch_search_runtime"] = first["catch_search_runtime"]
        test, patch = args.test_binary.resolve(), args.patch_binary.resolve()
        for name in profile.cases:
            case = out / name
            case.mkdir(exist_ok=True)
            previous = out / "first" / name
            first_receipt = json.loads((previous / "contract.json").read_text())
            shutil.copyfile(previous / "product.exe", case / "original.exe")
            shutil.copyfile(validate_runtime(previous, report["catch_search_runtime"]), case / NAME)
            registers, pop, _ = profile.entry_context(name)
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
            profile.validate_installation(PE32(original.read_bytes()), PE32(product.read_bytes()),
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
                    record["images"].append(profile.observe(
                        image, name, route, receipt, [wine] if wine else [], env,
                        args.timeout, source_section=".ndtext"))
            print(f"PASS re-lift {name}: 8 executions, {profile.calls} calls each", flush=True)
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
