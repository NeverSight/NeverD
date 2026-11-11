#!/usr/bin/env python3
"""Verify broad Number conversions in original source-interface void bridges."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-primitive-void-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        inputs = sorted((FIXTURES / "source_primitive_void_bridges/java").rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/SourcePrimitiveVoidBridgeHarness.java"], harness, classpath=originals)
        receipts = {
            "setter": change_signature(originals / "fixture/Setter.class",
                b"Ljava/lang/Object;Lfixture/Value;Lfixture/Marker<Ljava/lang/String;>;",
                b"Ljava/lang/Object;Lfixture/Value<Ljava/lang/Double;>;Lfixture/Marker<Ljava/lang/String;>;"),
            "receiver": change_signature(originals / "fixture/Receiver.class",
                b"<S:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Setter;Lfixture/Mirror;",
                b"<S:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Setter;Lfixture/Mirror<Ljava/lang/Double;>;"),
            "flags": bridge_flag(originals / "fixture/Setter.class", "set", "(Ljava/lang/Object;)V", 0x0001, 0x1041),
        }
        (work / "owned-input-edits.json").write_text(json.dumps(receipts, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        def hashes(root: Path) -> dict:
            return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(root.rglob("*.class"))}
        input_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "SourcePrimitiveVoidBridgeHarness", "original"], "baseline")
        if baseline.splitlines() != ["values=438,identities=153,failures=25",
                "broad-number-unboxing=retained,wide-default-dispatch-and-effects=matched,parameter-reflection=reported"]:
            raise RuntimeError("Owned wide-bridge baseline missed conversion or dispatch cases: " + baseline)
        classpath = ["--classpath", args.android_jar.resolve()]
        def owned_dex(directory: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *sorted(directory.rglob("*.class"))], "d8-" + destination.name)
            dex = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned wide setter fixture did not produce one DEX")
            return dex
        dex = owned_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the independently authored input inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        projections = coverage.get("generic_signature_projections", [])
        expected_parameters = {"Lfixture/Value;->set(Ljava/lang/Object;)V",
                               "Lfixture/GenericReceiver;->set(Ljava/lang/Object;)V"}
        if ({r["identity"] for r in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial" or
                {r["declaration"] for r in projections} != expected_parameters or
                len(projections) != len(expected_parameters)):
            raise RuntimeError("Wide setter projection lost original identities or erased another member")
        bridge = "Lfixture/Setter;->set(Ljava/lang/Object;)V"
        row = next(r for r in coverage["methods"] if r["identity"] == bridge)
        if (row.get("projection_kind") != "primitive-void-interface-bridge" or
                row.get("bridge_target") != "Lfixture/Setter;->set(D)V" or
                row.get("bridge_contracts") != ["Lfixture/Value;->set(Ljava/lang/Object;)V"] or
                not row.get("metadata_differences")):
            raise RuntimeError("Original wide bridge lost its full generic witness")
        for projection in projections:
            if (len(projection["changes"]) != 1 or
                    projection["changes"][0]["kind"] != "checked-primitive-void-parameter-erasure" or
                    projection["changes"][0]["source_type"] != "Ljava/lang/Object;" or
                    projection["changes"][0]["scope_origin"] != bridge):
                raise RuntimeError("Source parameter change lacks its exact original bridge proof")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(methods) | helpers:
            raise RuntimeError("javac changed an original runtime slot or added an unreported helper")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "SourcePrimitiveVoidBridgeHarness", "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Source setter changed broad conversion, wide values, dispatch or failure effects")
        negative = work / "negative"
        shutil.copytree(originals, negative)
        target = negative / "fixture/Setter.class"
        before = target.read_bytes()
        receipt = change_signature(target,
                b"Ljava/lang/Object;Lfixture/Value<Ljava/lang/Double;>;Lfixture/Marker<Ljava/lang/String;>;",
                b"Ljava/lang/Object;Lfixture/Value<Ljava/lang/Float;>;Lfixture/Marker<Ljava/lang/String;>;")
        if method_code(before) != method_code(target.read_bytes()):
            raise RuntimeError("Wrong-boxed-parameter counterexample changed original Code")
        (work / "negative-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = owned_dex(negative, work / "negative-dex")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath),
                                 "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Equal Object erasure hid an incompatible boxed generic parameter")
        if hashes(originals) != input_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original fixture or concrete harness Code changed during verification")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 438, "identity_cases": 153, "ordered_failure_cases": 25,
                    "broad_number_and_custom_conversion": "matched", "original_wide_default_dispatch": "retained",
                    "original_inventory_and_concrete_harness_code": "unchanged", "coverage": "partial",
                    "generic_parameter_reflection": "explicitly-reported", "other_full_generic_members": "retained",
                    "incompatible_boxed_substitution": "rejected", "reported_helpers_only": "verified"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
