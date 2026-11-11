#!/usr/bin/env python3
"""Retain empty generic marker parents, unused formals and exact bridge behavior."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import tempfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk


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
        prefix="neverd-marker-inheritance-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        inputs = sorted((FIXTURES / "marker_inheritance/java").rglob("*.java"))
        originals, harness_dir = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        classes, methods = compiler_inventory(verify, originals)
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        harness = FIXTURES / "harness/MarkerInheritanceHarness.java"
        verify.compile_local([harness], harness_dir, classpath=originals)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               "MarkerInheritanceHarness"], "baseline")
        if baseline.splitlines() != ["identities=31,failures=7,bridges=2",
                                     "marker-reflection=retained,unused-formals=retained,raw-superclass=retained"]:
            raise RuntimeError("Owned marker baseline did not exercise its expected cases")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", args.android_jar.resolve())
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the independently authored marker input inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods)
                or coverage["projected_method_count"] != 2
                or coverage["unrecovered_method_count"] != 0
                or coverage.get("generic_signature_projections") or coverage["status"] != "partial"):
            raise RuntimeError("Marker inheritance lost methods or changed generic declarations")
        for row in coverage["methods"]:
            if row["status"] == "source-projected" and (
                    row["projection_kind"] != "generic-interface-bridge"
                    or row["bridge_target"] not in methods or row["bridge_dispatch"] not in methods):
                raise RuntimeError("Marker bridge lost its exact target or contract")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if actual_classes != classes or not set(methods).issubset(actual_methods):
            raise RuntimeError("javac did not regenerate every original class and bridge descriptor")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir))),
                             "MarkerInheritanceHarness"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Marker inheritance changed dispatch, identity, failure order or reflection")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted(originals.rglob("*.class"))}:
            raise RuntimeError("Owned compiler input bytes were changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "bridge_count": 2, "identity_cases": 31, "ordered_failure_cases": 7,
                    "generic_marker_and_unused_formal_reflection": "retained", "raw_superclass": "retained",
                    "mutations_and_virtual_override": "matched", "original_class_sha256": hashes,
                    "original_descriptors": "retained", "coverage": "partial", "jvm_output": "matched"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
