#!/usr/bin/env python3
"""Verify final generic callback bounds through mixed reference/wide bridges."""
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-mixed-final-carrier-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "mixed_final_carrier_parameters/java").rglob("*.java"))
        inputs += [FIXTURES / ("value_carrier_bridges/java/fixture/" + name + ".java")
                   for name in ("CarrierB", "Trace")]
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/MixedFinalCarrierParameterHarness.java"], harness,
                             classpath=originals)
        owner = "Lfixture/Receiver;"
        prototype = "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"
        bridge = owner + "->invoke" + prototype
        original_signature = b"<S:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Function;"
        interface = "Lfixture/Function<Lfixture/Context;Lfixture/CarrierB;Lfixture/Callback<-Lfixture/Context;+Lfixture/Result;>;Ljava/lang/Boolean;>;"
        input_signature = b"<S:Ljava/lang/Object;>Ljava/lang/Object;" + interface.encode()
        target_signature = b"(Lfixture/Context;JLfixture/Callback<-Lfixture/Context;Lfixture/Result;>;)Ljava/lang/Boolean;"
        edits = [change_signature(originals / "fixture/Receiver.class", original_signature, input_signature),
                 bridge_flag(originals / "fixture/Receiver.class", "invoke", prototype, 0x1, 0x1041)]
        (work / "owned-input-encoding.json").write_text(json.dumps(edits, indent=2) + "\n")
        records = class_identity.compiler_classes(originals)
        classes, methods = compiler_inventory(verify, originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "MixedFinalCarrierParameterHarness", "original"], "baseline")
        if baseline.splitlines() != ["values=320,identities=963,failures=9", "reflection=original"]:
            raise RuntimeError("Owned mixed-final baseline missed identity, value or exception-order cases: " + baseline)
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", args.android_jar.resolve())
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed original mixed-final declaration identities")
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
                len(projections[0]["changes"]) != 1):
            raise RuntimeError("Mixed-final proof changed coverage or added a method-signature projection")
        change = projections[0]["changes"][0]
        row = rows[bridge]
        if (change["kind"] != "checked-value-carrier-interface" or change["scope_origin"] != bridge or
                change["original_type"] != interface or change["source_type"] != "Lfixture/Function;" or
                row.get("projection_kind") != "value-carrier-interface-bridge" or
                row.get("unbox_methods") != ["Lfixture/CarrierB;->unwrap()J"] or
                "box_method" in row or not row.get("metadata_differences")):
            raise RuntimeError("Mixed-final bridge lost the original raw-interface and conversion receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("Mixed-final source added or lost an undisclosed descriptor")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                wanted = original_signature.decode() if identity == owner and key == "signature" else before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Mixed-final class/field metadata changed: " + identity + ":" + key)
            for member, metadata in before["methods"].items():
                wanted = dict(metadata)
                if member == bridge:
                    wanted["access"] = 0x1
                if after["methods"][member] != wanted:
                    raise RuntimeError("Original complete mixed-final method metadata changed: " + member)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "MixedFinalCarrierParameterHarness", "rebuilt"], "recovered-execution")
        if actual.splitlines() != [baseline.splitlines()[0], "reflection=reported-raw-interface"]:
            raise RuntimeError("Wide values, boxed Boolean identity, checked casts or exception order changed")
        for label, source_signature, changed_signature in (
                ("wrong-final-leaf", target_signature, target_signature.replace(b"Lfixture/Result;", b"Ljava/lang/String;")),
                ("contravariant-bound", input_signature, input_signature.replace(b"+Lfixture/Result;", b"-Lfixture/Result;")),
                ("open-formal", target_signature, target_signature.replace(b"Lfixture/Result;", b"TS;"))):
            negative = work / ("negative-" + label)
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Receiver.class", source_signature, changed_signature)
            (work / (label + "-signature.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = verify.dex(sorted(negative.rglob("*.class")), work / (label + "-dex"), args.android_jar.resolve())
            rejected = work / ("rejected-" + label)
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                     str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Mixed-final proof accepted a same-erasure generic contract counterexample")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or
                hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original input, SDK or immutable harness bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 320, "identity_cases": 963, "ordered_failure_cases": 9,
                    "original_target_generic_contract_and_full_metadata": "retained",
                    "same_erasure_generic_counterexamples": "rejected",
                    "input_sdk_and_immutable_harness": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
