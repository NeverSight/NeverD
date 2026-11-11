#!/usr/bin/env python3
"""Verify witnessed Object lower parameter projection and original erased bridge execution."""
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag


def hashes(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-object-lower-parameter-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "object_lower_parameter_bridge/java").rglob("*.java"))
        inputs += [FIXTURES / "result_only_carrier_bridges/java/fixture/Function.java",
                   FIXTURES / "value_carrier_bridges/java/fixture/Trace.java",
                   FIXTURES / "contained_consumer_parameters/java/fixture/Consumer.java",
                   FIXTURES / "contained_consumer_parameters/java/fixture/Value.java"]
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/ObjectLowerParameterBridgeHarness.java"], harness, classpath=originals)
        owner = "Lfixture/Receiver;"
        original_signature = b"<S:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Function;"
        interface = "Lfixture/Function<Lfixture/Consumer<-Ljava/lang/Object;>;Ljava/lang/Object;>;"
        input_signature = b"<S:Ljava/lang/Object;>Ljava/lang/Object;" + interface.encode()
        prototype = "(Ljava/lang/Object;)Ljava/lang/Object;"
        bridge = owner + "->invoke" + prototype
        target = owner + "->invoke(Lfixture/Consumer;)Ljava/lang/Object;"
        target_signature = b"(Lfixture/Consumer<Ljava/lang/Object;>;)Ljava/lang/Object;"
        source_signature = b"(Lfixture/Consumer<-Ljava/lang/Object;>;)Ljava/lang/Object;"
        edits = [change_signature(originals / "fixture/Receiver.class", original_signature, input_signature),
                 bridge_flag(originals / "fixture/Receiver.class", "invoke", prototype, 0x1, 0x1041)]
        (work / "owned-input-encoding.json").write_text(json.dumps(edits, indent=2) + "\n")
        records = class_identity.compiler_classes(originals)
        classes, methods = compiler_inventory(verify, originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "ObjectLowerParameterBridgeHarness", "original"], "baseline")
        if baseline.splitlines() != ["values=504,identities=508,failures=10", "reflection=original"]:
            raise RuntimeError("Owned reference forwarding baseline missed expected identity, value or failure cases: " + baseline)
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", args.android_jar.resolve())
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed original reference forwarding identities")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        projections = coverage.get("generic_signature_projections", [])
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes) or
                coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"] or
                coverage["projected_method_count"] != 1 or coverage["status"] != "partial" or
                len(projections) != 1 or projections[0].get("declaration") != target or
                projections[0].get("original_signature") != target_signature.decode() or
                not projections[0].get("metadata_differences") or len(projections[0]["changes"]) != 1):
            raise RuntimeError("Object lower parameter projection lost original inventory or conservative coverage")
        change = projections[0]["changes"][0]
        if (change["kind"] != "inherited-closed-parameter-variance" or
                change["scope_origin"] != "Lfixture/Function;->invoke" + prototype or
                change["original_type"] != target_signature.decode() or change["source_type"] != source_signature.decode() or
                rows[bridge].get("projection_kind") != "generic-interface-bridge"):
            raise RuntimeError("Object lower parameter projection lost its complete original bridge witness")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("Object lower parameter projection added or lost an undisclosed runtime slot")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                wanted = before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Object lower parameter projection changed class or field metadata: " + identity + ":" + key)
            for member, metadata in before["methods"].items():
                wanted = dict(metadata)
                if member == target:
                    wanted["signature"] = source_signature.decode()
                if after["methods"][member] != wanted:
                    raise RuntimeError("Object lower parameter projection changed complete original method metadata: " + member)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "ObjectLowerParameterBridgeHarness", "recovered"], "recovered-execution")
        if actual.splitlines() != [baseline.splitlines()[0], "reflection=reported-Object-lower-parameter"]:
            raise RuntimeError("Object lower parameter projection changed result identity, mutation, effects or exception order")
        for label, old, changed in (
                ("different-lower-bound", input_signature, input_signature.replace(b"<-Ljava/lang/Object;>", b"<-Lfixture/Value;>")),
                ("different-invariant", target_signature, target_signature.replace(b"<Ljava/lang/Object;>", b"<Lfixture/Value;>")),
                ("upper-contract", input_signature, input_signature.replace(b"<-Ljava/lang/Object;>", b"<+Ljava/lang/Object;>"))):
            negative = work / ("negative-" + label)
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Receiver.class", old, changed)
            (work / (label + "-signature.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = verify.dex(sorted(negative.rglob("*.class")), work / (label + "-dex"), args.android_jar.resolve())
            rejected = work / ("rejected-" + label)
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                     str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Object lower parameter projection accepted same-erasure scope drift with unchanged Code")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or
                hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original input, SDK or immutable harness changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 504, "identity_cases": 508, "ordered_failure_cases": 10,
                    "original_erased_bridge_and_field_metadata": "unchanged",
                    "callback_dispatch_and_generic_bridge_cast_failures": "original order matched",
                    "projection_and_complete_metadata": "matched to report",
                    "same_erasure_bound_counterexamples": "rejected",
                    "input_sdk_and_immutable_harness": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
