#!/usr/bin/env python3
"""Compare original differently named reference forwarding with recovered JVM execution."""
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-reference-only-carrier-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "reference_only_carrier_bridges/java").rglob("*.java"))
        inputs += [FIXTURES / "result_only_carrier_bridges/java/fixture/Function.java",
                   FIXTURES / "value_carrier_bridges/java/fixture/Trace.java"]
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/ReferenceOnlyCarrierBridgeHarness.java"], harness, classpath=originals)
        owner = "Lfixture/Receiver;"
        original_signature = b"<S:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Function;"
        interface = "Lfixture/Function<TS;Lfixture/Carrier<TS;>;>;"
        input_signature = b"<S:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;" + interface.encode()
        prototype = "(Ljava/lang/Object;)Ljava/lang/Object;"
        bridge = owner + "->invoke" + prototype
        target = owner + "->apply(Ljava/lang/Object;)Lfixture/Carrier;"
        target_signature = b"(TS;)Lfixture/Carrier<TS;>;"
        edits = [change_signature(originals / "fixture/Receiver.class", original_signature, input_signature),
                 bridge_flag(originals / "fixture/Receiver.class", "invoke", prototype, 0x1, 0x1041)]
        (work / "owned-input-encoding.json").write_text(json.dumps(edits, indent=2) + "\n")
        records = class_identity.compiler_classes(originals)
        classes, methods = compiler_inventory(verify, originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "ReferenceOnlyCarrierBridgeHarness", "original"], "baseline")
        if baseline.splitlines() != ["values=1260,identities=1263,failures=6", "reflection=original"]:
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
                len(projections) != 1 or projections[0].get("original_signature") != input_signature.decode() or
                not projections[0].get("metadata_differences") or len(projections[0]["changes"]) != 1):
            raise RuntimeError("Reference forwarding lost original inventory or conservative coverage")
        change, row = projections[0]["changes"][0], rows[bridge]
        if (change["kind"] != "checked-value-carrier-interface" or change["scope_origin"] != bridge or
                change["original_type"] != interface or change["source_type"] != "Lfixture/Function;" or
                row.get("projection_kind") != "value-carrier-interface-bridge" or
                row.get("source_identity") != bridge or row.get("bridge_target") != target or
                row.get("bridge_dispatch") != "Lfixture/Function;->invoke" + prototype or
                row.get("unbox_methods") != [] or "box_method" in row or not row.get("metadata_differences")):
            raise RuntimeError("Reference forwarding invented a conversion or lost its complete original witness")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("Reference forwarding added or lost an undisclosed runtime slot")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                wanted = original_signature.decode() if identity == owner and key == "signature" else before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Reference forwarding changed class or field metadata: " + identity + ":" + key)
            for member, metadata in before["methods"].items():
                wanted = dict(metadata)
                if member == bridge:
                    wanted["access"] = 0x1
                if after["methods"][member] != wanted:
                    raise RuntimeError("Reference forwarding changed complete original method metadata: " + member)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "ReferenceOnlyCarrierBridgeHarness", "recovered"], "recovered-execution")
        if actual.splitlines() != [baseline.splitlines()[0], "reflection=reported-raw-interface"]:
            raise RuntimeError("Reference forwarding changed result identity, mutation, effects or exception order")
        for label, old, changed in (
                ("input-scope", target_signature, b"(TV;)Lfixture/Carrier<TS;>;"),
                ("result-scope", target_signature, b"(TS;)Lfixture/Carrier<TV;>;"),
                ("missing-input-scope", target_signature, b"(Ljava/lang/Object;)Lfixture/Carrier<TS;>;")):
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
                raise RuntimeError("Reference forwarding accepted same-erasure scope drift with unchanged Code")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or
                hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original input, SDK or immutable harness changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 1260, "identity_cases": 1263, "ordered_failure_cases": 6,
                    "reference_result_and_mutation": "original reference retained, including null",
                    "full_target_and_field_scope": "unchanged", "additional_conversions": "none",
                    "projection_and_complete_metadata": "matched to report",
                    "same_erasure_scope_counterexamples": "rejected",
                    "input_sdk_and_immutable_harness": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
