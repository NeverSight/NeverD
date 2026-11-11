#!/usr/bin/env python3
"""Verify scoped source defaults through original checked parameter and return bridges."""
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
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags


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
    parser.add_argument("--dependency-root", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-scoped-default-parameters-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "scoped_default_parameters/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/ScopedDefaultParametersHarness.java"], harness,
                             classpath=originals)
        owner = "Lfixture/Provider;"
        runtime_method = owner + "->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Root;"} if args.dependency_root else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        dependency = work / "owned-root.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("fixture/Root.class", (originals / "fixture/Root.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_root:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "ScopedDefaultParametersHarness", "original"], "baseline")
        expected = ["values=43,identities=364,states=76,failures=33",
                    "scoped-parent-arguments=retained,original-default-owner-and-bridge=retained,consumer-put-declarations=absent"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned scoped source default baseline missed a dispatch, identity or failure case: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if not result.is_file() or list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned scoped source default fixture did not produce one DEX")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the original scoped source default runtime inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 1 or coverage["status"] != "partial"
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")
                or rows[runtime_method].get("projection_kind") != "generic-interface-bridge"):
            raise RuntimeError("Scoped source default changed original runtime inventory, scopes or projection receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_root else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(methods) | helpers:
            raise RuntimeError("Recovered scoped source default invented a bridge or runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                wanted = before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Original scoped source default class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"][method] != metadata:
                    raise RuntimeError("Original scoped source default method scope, access or throws changed: " + method)
        rebuilt_path = (rebuilt, harness, dependency) if args.dependency_root else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, rebuilt_path)),
                             "ScopedDefaultParametersHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Direct, inherited, reflected or SDK scoped source default execution changed")

        original_signature = b"(TK;Lfixture/Entry<TK;TV;>;)Lfixture/Entry<TK;TV;>;"
        drift_signature = b"(TK;Lfixture/Entry<TK;TV;>;)Lfixture/Entry<TV;TK;>;"
        for control in ("result-drift", "missing-original-bridge"):
            negative = work / (control + "-originals")
            shutil.copytree(originals, negative)
            owner_class = negative / "fixture/Provider.class"
            if control == "result-drift":
                receipt = change_signature(owner_class, original_signature, drift_signature)
            else:
                receipt = change_method_flags(
                    owner_class, b"(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
                    0x1041, 0x0001, name=b"put")
            (work / (control + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (control + "-dex"))
            rejected = work / (control + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (control + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Scoped source default accepted a changed full result or missing original bridge")
        negative_java = work / "result-drift-java/fixture/Provider.java"
        negative_java.parent.mkdir(parents=True)
        content = (fixture / "fixture/Provider.java").read_text()
        declaration = "default Entry<K, V> put(K key, Entry<K, V> value)"
        if content.count(declaration) != 1:
            raise RuntimeError("Missing exact independent full-result counterexample")
        negative_java.write_text(content.replace(
            declaration, "default Entry<V, K> put(K key, Entry<K, V> value)", 1))
        result = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-cp", str(originals), "-d",
                                 str(work / "result-drift-compiler"), str(negative_java)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "result-drift-compiler.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "return type" not in result.stderr:
            raise RuntimeError("Independent javac did not reject the full scoped result drift")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 1, "value_cases": 43, "identity_cases": 364,
                    "state_cases": 76, "failure_cases": 33,
                    "profile": "dependency-root" if args.dependency_root else "source-root",
                    "complete_scoped_parameters_result_and_original_default_bridge": "retained",
                    "mutable_aliases_virtual_dispatch_and_checked_parameter_order": "matched",
                    "complete_original_metadata_and_runtime_inventory": "unchanged except reported helpers",
                    "same_code_result_drift_counterexample": "rejected by NeverD and independent javac",
                    "same_code_missing_original_bridge_counterexample": "rejected by NeverD",
                    "input_harness_and_sdk_bytes": "unchanged", "coverage": coverage["status"]}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
