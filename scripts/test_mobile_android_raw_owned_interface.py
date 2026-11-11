#!/usr/bin/env python3
"""Verify raw SDK interface arguments witnessed by original class-owned parameters."""
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
        prefix="neverd-raw-owned-interface-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "raw_owned_interface/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/RawOwnedInterfaceHarness.java"], harness,
                             classpath=originals)
        owner = "Lfixture/Receiver;"
        runtime_method = owner + "->compare(Ljava/lang/Object;Ljava/lang/Object;)I"
        source_signature = b"<T:Ljava/lang/Object;>Ljava/lang/Object;Ljava/util/Comparator<TT;>;"
        raw_signature = b"<T:Ljava/lang/Object;>Ljava/lang/Object;Ljava/util/Comparator;"
        receipt = change_signature(originals / "fixture/Receiver.class", source_signature, raw_signature)
        (work / "owned-input-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        classes, methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "RawOwnedInterfaceHarness", "original"], "baseline")
        expected = ["values=36,identities=98,states=46,failures=10",
                    "class-owned-method-formals=retained,sdk-default-owner=retained,raw-edge=original"]
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
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count", 0)
                or coverage["status"] != "partial" or len(projections) != 1
                or projections[0].get("declaration") != owner
                or projections[0].get("original_signature") != raw_signature.decode()
                or not projections[0].get("metadata_differences") or len(projections[0]["changes"]) != 1):
            raise RuntimeError("Raw interface projection changed runtime entries or lost its metadata receipt")
        change = projections[0]["changes"][0]
        if (change.get("kind") != "raw-interface-owned-class-formal"
                or change.get("original_type") != "Ljava/util/Comparator;"
                or change.get("source_type") != "Ljava/util/Comparator<TT;>;"
                or change.get("scope_origin") != runtime_method):
            raise RuntimeError("Raw interface argument lost its complete original class-owned witness")
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
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                wanted = source_signature.decode() if identity == owner and key == "signature" else before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Raw interface class metadata differs from its single reported change: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"][method] != metadata:
                    raise RuntimeError("Original class-owned method scope, access or throws changed: " + method)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "RawOwnedInterfaceHarness", "rebuilt"], "rebuilt-execution")
        if actual.splitlines() != [expected[0], expected[1].replace("raw-edge=original", "raw-edge=reported-owned-argument")]:
            raise RuntimeError("Direct, interface, reflected or unchanged SDK default execution changed")

        for label, signature, java_before, java_after in (
                ("method-owned-shadow", b"<U:Ljava/lang/Object;>(TU;TU;)I", "int compare(T left, T right)", "<U> int compare(U left, U right)"),
                ("inconsistent-parameters", b"(TT;Ljava/lang/Object;)I", "int compare(T left, T right)", "int compare(T left, Object right)")):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Receiver.class", b"(TT;TT;)I", signature)
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
                if source.name == "Receiver.java":
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
                    "value_cases": 36, "identity_cases": 98, "state_cases": 46, "failure_cases": 10,
                    "original_method_scopes_and_runtime_inventory": "unchanged",
                    "raw_interface_argument": "unique original class-owned formal, reported projection",
                    "original_sdk_defaults_and_method_owned_scopes": "unchanged",
                    "same_code_scope_and_parameter_counterexamples": "rejected by recovery and independent javac",
                    "input_harness_and_sdk_bytes": "unchanged", "coverage": coverage["status"]}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
