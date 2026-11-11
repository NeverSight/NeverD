#!/usr/bin/env python3
"""Verify original bridges to differently named scoped superclass setters."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_kotlin_scopes import erase_fixture_method_signature
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_kotlin_scopes import erase_fixture_method_signature
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-named-setter-bridges-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "named_generic_setter_bridges/java").rglob("*.java"))
        inputs.append(FIXTURES / "value_carrier_bridges/java/fixture/Trace.java")
        verify.compile_local(inputs, originals)
        harness_name = "NamedGenericSetterBridgeHarness"
        verify.compile_local([FIXTURES / ("harness/" + harness_name + ".java")], harness,
                             classpath=originals)
        child = originals / "fixture/Child.class"
        before_code = method_code(child.read_bytes())
        edits = {"signature": erase_fixture_method_signature(child, method_name=b"set$contract",
            prototype=b"(Ljava/lang/Object;)V", expected_signature=b"(TS;)V",
            original_flags=0x1, set_synthetic=False),
            "flags": bridge_flag(child, "set$contract", "(Ljava/lang/Object;)V",
                                 original_flags=0x1, input_flags=0x1041)}
        if before_code != method_code(child.read_bytes()):
            raise RuntimeError("Owned setter metadata edit changed Code or runtime descriptors")
        (work / "owned-input-encoding.json").write_text(json.dumps(edits, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_name, "original"], "baseline")
        if baseline.splitlines() != ["identities=16,failures=6", "reflection=original"]:
            raise RuntimeError("Owned generic setter baseline missed cases")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", args.android_jar.resolve())
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed original setter identities")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        bridge = "Lfixture/Child;->set$contract(Ljava/lang/Object;)V"
        row = next(row for row in coverage["methods"] if row["identity"] == bridge)
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["projected_method_count"] != 1 or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial" or coverage.get("generic_signature_projections")
                or row["projection_kind"] != "named-generic-setter-bridge"
                or row["bridge_target"] != "Lfixture/Child;->set(Ljava/lang/Object;)V"
                or row["bridge_dispatch"] != "Lfixture/Base;->set$contract(Ljava/lang/Object;)V"
                or not row.get("metadata_differences")):
            raise RuntimeError("Setter recovery lost complete generic or original body contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if (rebuilt_classes != classes or not set(methods).issubset(rebuilt_methods)
                or set(rebuilt_methods) - set(methods) != helpers):
            raise RuntimeError("Recompiled setter changed original method inventory")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             harness_name, "recovered"], "recovered-execution")
        if actual.splitlines() != [baseline.splitlines()[0], "reflection=reported-bridge-flags"]:
            raise RuntimeError("Setter recovery changed identity, virtual dispatch or exceptions")
        negative = work / "negative-originals"
        for path in originals.rglob("*.class"):
            destination = negative / path.relative_to(originals)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(path.read_bytes())
        receipt = change_signature(negative / "fixture/Child.class", b"(TS;)V", b"(TV;)V")
        (work / "negative-metadata-change.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = verify.dex(sorted(negative.rglob("*.class")), work / "negative-dex",
                                  args.android_jar.resolve())
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic inheritance" not in result.stderr:
            raise RuntimeError("Different setter formal with identical erasure and Code was accepted")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted(originals.rglob("*.class"))}:
            raise RuntimeError("Owned setter originals changed during recovery")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "identity_cases": 16, "ordered_failure_cases": 6,
                    "original_code_and_descriptors": "unchanged", "generic_reflection": "matched",
                    "bridge_flags": "reported", "metadata_counterexample": "rejected",
                    "virtual_dispatch_and_effects": "matched", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
