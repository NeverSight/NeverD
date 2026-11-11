#!/usr/bin/env python3
"""Verify owned reference-backed result boxing with a fixed generic representation."""
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

SOURCE = b"<S:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Function;"
INTERFACE = b"Lfixture/Function<Lfixture/Carrier<Ljava/lang/Object;Lfixture/Element;>;>;"
INPUT = b"<S:Ljava/lang/Object;>Ljava/lang/Object;" + INTERFACE
REPRESENTATION = b"Lfixture/Payload<Ljava/lang/Object;Ljava/lang/Object;>;"


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
        prefix="neverd-reference-result-box-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "reference_result_boxing/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/ReferenceResultBoxingHarness.java"], harness, classpath=originals)
        receiver = originals / "fixture/Receiver.class"
        before = receiver.read_bytes()
        receipt = {"signature": change_signature(receiver, SOURCE, INPUT),
                   "flags": bridge_flag(receiver, "invoke", "()Ljava/lang/Object;", 1, 0x1041)}
        if method_code(before) != method_code(receiver.read_bytes()):
            raise RuntimeError("Owned callback encoding changed Code or descriptors")
        (work / "owned-input-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in originals.rglob("*.class")}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "ReferenceResultBoxingHarness", "original"], "baseline")
        if baseline.splitlines() != ["values=15,identities=44,failures=4",
                "fixed-representation=retained,fresh-boxing=matched,initialization-after-success=matched"]:
            raise RuntimeError("Owned baseline missed boxing, identity or ordered initialization: " + baseline)
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", args.android_jar.resolve())
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes or
                {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned original inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        bridge = "Lfixture/Receiver;->invoke()Ljava/lang/Object;"
        row = next(row for row in coverage["methods"] if row["identity"] == bridge)
        if ({row["identity"] for row in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["projected_method_count"] != 1 or coverage["unrecovered_method_count"] != 0 or
                coverage["status"] != "partial" or row["source_identity"] != bridge or
                row["projection_kind"] != "value-carrier-interface-bridge" or
                row["box_method"] != "Lfixture/Carrier;->box(Lfixture/Payload;)Lfixture/Carrier;" or
                row["bridge_target"] != "Lfixture/Receiver;->storage()Lfixture/Payload;" or
                row["unbox_methods"] != [] or
                row["representation_field"] != "Lfixture/Carrier;->backing:Lfixture/Payload;" or
                row["representation_signature"] != REPRESENTATION.decode()):
            raise RuntimeError("Reference result proof lost the original factory or complete backing type")
        projections = coverage.get("generic_signature_projections", [])
        if (len(projections) != 1 or projections[0]["original_signature"] != INPUT.decode() or
                len(projections[0]["changes"]) != 1 or
                projections[0]["changes"][0]["original_type"] != INTERFACE.decode() or
                projections[0]["changes"][0]["scope_origin"] != bridge):
            raise RuntimeError("Original phantom type arguments were not preserved and disclosed separately")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(methods) | helpers:
            raise RuntimeError("Recompiled original inventory changed outside reported helpers")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "ReferenceResultBoxingHarness", "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Reference boxing changed freshness, payload identity, mutations or initialization")
        negative = work / "negative"
        shutil.copytree(originals, negative)
        target = negative / "fixture/Carrier.class"
        before = target.read_bytes()
        receipt = change_signature(target, REPRESENTATION,
                                   REPRESENTATION.replace(b"Ljava/lang/Object;>" , b"Lfixture/Element;>"))
        if method_code(before) != method_code(target.read_bytes()):
            raise RuntimeError("Representation counterexample changed Code or descriptors")
        (work / "negative-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        rejected = work / "rejected"
        negative_dex = verify.dex(sorted(negative.rglob("*.class")), work / "negative-dex", args.android_jar.resolve())
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic inheritance" not in result.stderr:
            raise RuntimeError("Equal erasure hid a changed full backing representation")
        negative_java = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "Carrier.java":
                content = content.replace("private final Payload<Object, Object> backing;",
                                          "private final Payload<Object, Element> backing;")
            destination = negative_java / source.relative_to(fixture)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Independent javac accepted changed backing result arguments")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in originals.rglob("*.class")}:
            raise RuntimeError("Original owned class bytes changed during recovery")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "values": 15, "identity_cases": 44, "ordered_failure_cases": 4,
                    "fixed_generic_representation": "matched", "phantom_carrier_arguments": "reported separately",
                    "fresh_boxing_and_payload_mutation": "matched", "initialization_after_success": "matched",
                    "representation_counterexample": "rejected-by-neverd-and-javac",
                    "original_code_and_descriptors": "unchanged", "coverage": "partial", "jvm_output": "matched"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
