#!/usr/bin/env python3
"""Verify raw interface arguments witnessed by exact original captured scopes."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature


def hashes(root: Path) -> dict:
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(root.rglob("*.class"))}


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
        prefix="neverd-raw-captured-interface-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "raw_captured_interface/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/RawCapturedInterfaceHarness.java"], harness,
                             classpath=originals)
        owner = "Lfixture/Owner$Receiver;"
        runtime_method = owner + "->compare(Ljava/lang/Object;Ljava/lang/Object;)I"
        source_signature = b"<R:Ljava/lang/Object;>Ljava/lang/Object;Ljava/util/Comparator<TT;>;"
        raw_signature = b"<R:Ljava/lang/Object;>Ljava/lang/Object;Ljava/util/Comparator;"
        receipt = change_signature(originals / "fixture/Owner$Receiver.class", source_signature, raw_signature)
        (work / "owned-input-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        classes, methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "RawCapturedInterfaceHarness", "original"], "baseline")
        expected = ["values=36,identities=194,states=46,failures=10",
                    "captured-formal=reported,sdk-default-owner=retained,raw-edge=original"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned comparator baseline missed a dispatch, identity or failure case: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", destination, *sorted(root.rglob("*.class"))], "d8-" + destination.name)
            result = destination / "classes.dex"
            if not result.is_file() or list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned raw-interface fixture did not produce one DEX")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the original raw-interface runtime inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        projections = coverage.get("generic_signature_projections", [])
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial"
                or coverage.get("minimum_java_release") != 25):
            raise RuntimeError("Captured raw interface lost original runtime entries or projection disclosure")
        changes = [change for row in projections for change in row["changes"]]
        arguments = [change for change in changes if change["kind"] == "raw-interface-captured-formal"]
        if (len(arguments) != 1 or arguments[0].get("original_type") != "Ljava/util/Comparator;"
                or arguments[0].get("source_type") != "Ljava/util/Comparator<TT;>;"
                or arguments[0].get("scope_origin") != runtime_method):
            raise RuntimeError("Captured argument lost its exact original resolved lexical witness")
        erased = [change for change in changes if change["kind"] == "static-inner-type-variable"]
        qualifiers = [change for change in changes if change["kind"] == "static-inner-owner-arguments"]
        if (len(erased) != 4 or len(qualifiers) != 1 or len(changes) != 6
                or any(change["source_type"] != "Ljava/lang/Object;"
                       or change["original_type"] != "Lfixture/Owner;::T" for change in erased)):
            raise RuntimeError("Captured argument changed beyond its reported Object-bound lexical projection")
        bindings = coverage.get("class_source_bindings", [])
        if len(bindings) != 1 or bindings[0].get("binding_kind") != "inner-to-static-member":
            raise RuntimeError("Missing exact captured owner projection")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(methods) | helpers:
            raise RuntimeError("Recovered raw interface invented a bridge, default owner or runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            expected_fields = json.loads(json.dumps(before["fields"]))
            expected_inner = dict(before["inner_class"]) if before["inner_class"] else None
            wanted_signature = before.get("signature")
            if identity == owner:
                capture = owner + "->this$0:Lfixture/Owner;"
                if (bindings[0].get("outer_capture_field") != capture
                        or expected_fields[capture]["access"] != 0x1010
                        or not any("synthetic capture-field" in message
                                   for message in bindings[0].get("metadata_differences", []))):
                    raise RuntimeError("Missing synthetic capture-field metadata disclosure")
                expected_fields[capture]["access"] &= ~0x1000
                expected_fields[owner + "->seen:Ljava/lang/Object;"]["signature"] = None
                expected_inner["access"] |= 8
                wanted_signature = "<R:Ljava/lang/Object;>Ljava/lang/Object;Ljava/util/Comparator<Ljava/lang/Object;>;"
            if after["fields"] != expected_fields or after["inner_class"] != expected_inner:
                raise RuntimeError("Captured fields or lexical owner changed beyond exact disclosed differences")
            for key in ("access", "signature", "superclass", "interfaces", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                wanted = wanted_signature if key == "signature" else before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Captured class metadata differs from its reported projection: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if method == runtime_method:
                    wanted["signature"] = None
                elif method == "Lfixture/Owner;->create()Lfixture/Owner$Receiver;":
                    wanted["signature"] = "()Lfixture/Owner$Receiver<Ljava/lang/String;>;"
                if after["methods"][method] != wanted:
                    raise RuntimeError("Captured method metadata differs from its exact disclosed projection: " + method)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "RawCapturedInterfaceHarness", "rebuilt"], "rebuilt-execution")
        if actual.splitlines() != [expected[0], expected[1].replace("raw-edge=original", "raw-edge=reported-owned-argument")]:
            raise RuntimeError("Direct, interface, reflected or unchanged SDK default execution changed")

        for label, signature, java_before, java_after in (
                ("method-owned-shadow", b"<U:Ljava/lang/Object;>(TU;TU;)I", "int compare(T left, T right)", "<U> int compare(U left, U right)"),
                ("inconsistent-parameters", b"(TT;Ljava/lang/Object;)I", "int compare(T left, T right)", "int compare(T left, Object right)"),
                ("reversed-parameters", b"(Ljava/lang/Object;TT;)I", "int compare(T left, T right)", "int compare(Object left, T right)")):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Owner$Receiver.class", b"(TT;TT;)I", signature)
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                     str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Raw interface proof accepted an incompatible same-Code scope")
            negative_java = work / (label + "-java")
            compiler_inputs = []
            for source in inputs:
                content = source.read_text()
                if source.name == "Owner.java":
                    if content.count(java_before) != 1:
                        raise RuntimeError("Missing exact independent scope counterexample")
                    content = content.replace(java_before, java_after, 1)
                destination = negative_java / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                compiler_inputs.append(str(destination))
            result = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                     str(work / (label + "-compiler")), *compiler_inputs], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-compiler.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or not any(word in result.stderr for word in ("clash", "override")):
                raise RuntimeError("Independent javac accepted the incompatible original scope")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 36, "identity_cases": 194, "state_cases": 46, "failure_cases": 10,
                    "original_scopes": "exact disclosed Object projection", "runtime_inventory": "original entries and reported helpers only",
                    "raw_interface_argument": "exact original captured formal, reported projection",
                    "original_sdk_defaults_and_method_owned_scopes": "unchanged",
                    "same_code_scope_and_parameter_counterexamples": "rejected by recovery and independent javac",
                    "input_harness_and_sdk_bytes": "unchanged", "coverage": coverage["status"]}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
