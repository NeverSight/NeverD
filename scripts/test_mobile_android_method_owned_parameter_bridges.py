#!/usr/bin/env python3
"""Verify original checked bridges with complete method-owned recursive bounds."""
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
import zipfile

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
    parser.add_argument("--dependency-interface", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-method-owned-parameter-bridge-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "method_owned_parameter_bridges/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/MethodOwnedParameterBridgeHarness.java"], harness,
                             classpath=originals)
        owner = "Lfixture/Core;"
        runtime_method = owner + "->pick(Ljava/lang/Class;Ljava/lang/Object;)Ljava/lang/Enum;"
        target_method = owner + "->pick(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;"
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        classes = all_classes - ({"Lfixture/Contract;"} if args.dependency_interface else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        dependency = work / "owned-method-contract.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("fixture/Contract.class", (originals / "fixture/Contract.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_interface:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "MethodOwnedParameterBridgeHarness", "original"], "baseline")
        expected = ["values=40,identities=293,states=81,failures=41",
                    "recursive-method-bounds=retained,invariant-class-arguments=retained,original-checked-bridge=retained"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned comparator baseline missed a dispatch, identity or failure case: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
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
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 1 or coverage["status"] != "partial"
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")
                or rows[runtime_method].get("projection_kind") != "generic-interface-bridge"):
            raise RuntimeError("Method-owned bridge changed original runtime inventory, scopes or projection receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_interface else None)
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
                wanted = before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Raw interface class metadata differs from its single reported change: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"][method] != metadata:
                    raise RuntimeError("Original class-owned method scope, access or throws changed: " + method)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "MethodOwnedParameterBridgeHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Direct, interface, reflected or unchanged SDK default execution changed")

        original_signature = b"<N:Ljava/lang/Enum<TN;>;>(Ljava/lang/Class<TN;>;Ljava/lang/String;)TN;"
        for label, signature, java_before, java_after in (
                ("self-bound-drift", original_signature.replace(b"Enum<TN;>", b"Enum<+TN;>"),
                 "N extends Enum<N>", "N extends Enum<? extends N>"),
                ("invariant-class-drift", original_signature.replace(b"Class<TN;>", b"Class<+TN;>"),
                 "Class<N> kind", "Class<? extends N> kind")):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Core.class", original_signature, signature)
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Raw interface proof accepted an incompatible same-Code scope")
            negative_java = work / (label + "-java")
            compiler_inputs = []
            for source in inputs:
                content = source.read_text()
                if source.name == "Core.java":
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
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "dependency_interface": args.dependency_interface,
                    "class_count": len(classes), "method_count": len(methods), "projected_methods": 1,
                    "value_cases": 40, "identity_cases": 293, "state_cases": 81, "failure_cases": 41,
                    "method_owned_recursive_bounds_and_invariant_arguments": "unchanged",
                    "original_checked_parameter_bridge": "exact original body proof, regenerated by javac",
                    "complete_original_metadata_and_runtime_inventory": "unchanged except reported helpers",
                    "same_code_scope_counterexamples": "rejected by recovery and independent javac",
                    "input_harness_dependency_and_sdk_bytes": "unchanged", "coverage": coverage["status"]}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
