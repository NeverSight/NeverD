#!/usr/bin/env python3
"""Revalidate and execute the exact source C++ PE32 files on native Windows."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import struct
import subprocess
import xml.etree.ElementTree as ET

if __package__:
    from .check_windows_registration_cxx import SOURCE, parent_code_end
    from .check_windows_registration_cxx_rewrite import (
        BASES, image_name, observe, require_image_matrix,
        require_installation_identity, safe_handlers)
    from .check_windows_registration_rewrite import PE32
    from .windows_registration_cleanup_relift import (
        RELIFT_LABELS, proof_digests, require_test_result, validate_cleanup_artifacts)
    from .windows_registration_runtime import validate_runtime
else:
    from check_windows_registration_cxx import SOURCE, parent_code_end
    from check_windows_registration_cxx_rewrite import (
        BASES, image_name, observe, require_image_matrix,
        require_installation_identity, safe_handlers)
    from check_windows_registration_rewrite import PE32
    from windows_registration_cleanup_relift import (
        RELIFT_LABELS, proof_digests, require_test_result, validate_cleanup_artifacts)
    from windows_registration_runtime import validate_runtime


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--wine")
    parser.add_argument("--wine-prefix", type=Path)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    if os.name != "nt" and not args.wine:
        parser.error("native Windows is required unless --wine is explicit")
    root = args.evidence_root.resolve()
    report = {"schema": 1, "platform": platform.platform(),
              "evidence": "wine-cxx-replay" if args.wine else "native-cxx-replay",
              "passed": False, "cases": []}
    try:
        source = json.loads((root / "registration-cxx-rewrite.json").read_text())
        schema = source.get("schema")
        installation = {1: "manual-checked-transaction",
                        2: "manual-public-cli-checked-transactions",
                        3: "two-generation-checked-transactions"}.get(schema)
        if not installation or \
                source.get("evidence") != "source-msvc-cxx-reconstruction" or \
                source.get("installation") != installation or \
                source.get("source_sha256") != hashlib.sha256(SOURCE.read_bytes()).hexdigest() or \
                not source.get("passed") or len(source["cases"]) != 2 or \
                {case["case"] for case in source["cases"]} != {"value", "reference"}:
            raise ValueError("source C++ reconstruction evidence is incomplete")
        if schema == 3 and source.get("proofs") != proof_digests():
            raise ValueError("source C++ re-lift proof changed")
        if source.get("source_probe"):
            if source["source_probe"] != "derived-saved-esp-writeback":
                raise ValueError("source C++ probe has an unknown derivation")
            report["source_probe"] = source["source_probe"]
        report["source_schema"] = schema
        report["installation"] = installation
        environment = os.environ.copy()
        if schema == 3:
            environment["WINEDLLOVERRIDES"] = "vcruntime140=n"
        if args.wine_prefix:
            environment.update(WINEARCH="win32", WINEDEBUG="-all",
                               WINEPREFIX=str(args.wine_prefix.resolve()))
        launcher = [args.wine] if args.wine else []
        for case in source["cases"]:
            name = case["case"]
            reference = name == "reference"
            parent = root / name
            if schema == 3:
                validate_runtime(parent, source["catch_search_runtime"])
                if require_test_result(parent / "rewrite.xml") != 1:
                    raise ValueError("source C++ first-generation proof failed or skipped")
            contract = json.loads((parent / "compiled-contract.json").read_text())
            original = PE32((parent / "original.exe").read_bytes())
            entry = original.entry(b"registration_cxx_probe")
            end = parent_code_end(parent / "original.map", original, entry)
            if original.base != BASES[0] or case.get("reference") != reference or \
                    case["original_code_end_rva"] != end or \
                    contract["source_image_sha256"] != hashlib.sha256(original.data).hexdigest():
                raise ValueError("source C++ original owner or identity changed")
            patched = PE32((parent / "patched.exe").read_bytes())
            if contract["image_sha256"] != hashlib.sha256(patched.data).hexdigest():
                raise ValueError("source C++ installed image changed its compiler identity")
            _, old_handlers = safe_handlers(original)
            _, new_handlers = safe_handlers(patched)
            if new_handlers != sorted(set(old_handlers + [contract["registration_handler_rva"]])):
                raise ValueError("source C++ SafeSEH closure changed")
            relift, second = validate_cleanup_artifacts(parent, contract, case) \
                if schema == 3 else (None, None)
            records = case["observations"]
            require_image_matrix(records, schema)
            replay = {"case": name, "observations": []}
            report["cases"].append(replay)
            for record in records:
                path = parent / image_name(record["image"])
                second_generation = path.name.removesuffix(".exe").removesuffix("-rebased") in RELIFT_LABELS
                if hashlib.sha256(path.read_bytes()).hexdigest() != record["sha256"]:
                    raise ValueError("source C++ replay file is missing or changed")
                if record["generated"]:
                    require_installation_identity(
                        PE32(path.read_bytes()), second if second_generation else patched)
                observation = observe(path, record["generated"], reference, end,
                                      contract, launcher, environment, args.timeout,
                                      relift if second_generation else None)
                if observation["runtime_base"] != record["runtime_base"]:
                    raise ValueError("source C++ loader changed its forced base")
                replay["observations"].append(observation)
            print(f"PASS native source C++ {name}: {len(records)} identical-file executions", flush=True)
        report["passed"] = True
    except (OSError, ValueError, KeyError, TypeError, struct.error,
            ET.ParseError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{'PASS' if report['passed'] else 'FAIL'} source C++ replay: {args.output}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
