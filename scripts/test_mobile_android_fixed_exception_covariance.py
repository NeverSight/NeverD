#!/usr/bin/env python3
"""Verify generic class covariance with complete original fixed Exceptions and ancestor bridges."""
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
    parser.add_argument("--dependency-root", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-fixed-exception-covariance-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "fixed_exception_covariance/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/FixedExceptionCovarianceHarness.java"], harness,
                             classpath=originals)
        owner = "Lfixture/Core;"
        runtime_method = owner + "->copy()Lfixture/Parent;"
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Far;"} if args.dependency_root else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        dependency = work / "owned-root.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("fixture/Far.class", (originals / "fixture/Far.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_root:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "FixedExceptionCovarianceHarness", "original"], "baseline")
        expected = ["values=54,identities=1728,states=180,failures=126",
                    "fixed-exception-order=retained,complete-generic-parent-result=retained,original-bridge-dispatch=retained"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned scoped unchecked implementation baseline missed a dispatch, identity or failure case: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if not result.is_file() or list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned scoped unchecked implementation fixture did not produce one DEX")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the original scoped unchecked implementation runtime inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 3 or coverage["status"] != "partial"
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != {runtime_method, owner + "->copy()Ljava/lang/Object;", "Lfixture/Parent;->copy()Ljava/lang/Object;"}):
            raise RuntimeError("Unchecked covariance changed original inventory, complete parameters or original bridge receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_root else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(methods) | helpers:
            raise RuntimeError("Recovered unchecked covariance changed the original bridge or runtime inventory")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                wanted = before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Original scoped unchecked implementation class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"][method] != metadata:
                    raise RuntimeError("Original scoped unchecked implementation method scope, access or throws changed: " + method)
        rebuilt_path = (rebuilt, harness, dependency) if args.dependency_root else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, rebuilt_path)),
                             "FixedExceptionCovarianceHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Direct, inherited, reflected or SDK scoped unchecked implementation execution changed")

        parent_drift = work / "parent-result-drift-originals"
        shutil.copytree(originals, parent_drift)
        receipt = change_signature(parent_drift / "fixture/Parent.class",
            b"()Lfixture/Parent<TT;>;",
            b"()Lfixture/Parent<Ljava/lang/Number;>;")
        (work / "parent-result-drift-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        parent_dex = dex(parent_drift, work / "parent-result-drift-dex")
        rejected = work / "parent-result-drift-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(parent_dex),
                                 *map(str, classpath), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "parent-result-drift-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists():
            raise RuntimeError("Fixed covariance admitted changed full generic superclass results")
        invalid_parent = work / "parent-result-drift-java/fixture/Parent.java"
        invalid_parent.parent.mkdir(parents=True)
        content = (fixture / "fixture/Parent.java").read_text()
        declaration = "public Parent<T> copy("
        if content.count(declaration) != 1:
            raise RuntimeError("Missing exact owned parent result counterexample")
        invalid_parent.write_text(content.replace(declaration, "public Parent<Number> copy(", 1))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-cp", str(originals), "-d",
                                   str(work / "parent-result-drift-compiler"), str(invalid_parent),
                                   str(fixture / "fixture/Core.java")], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "parent-result-drift-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or "return type" not in compiler.stderr:
            raise RuntimeError("Independent javac accepted a changed complete generic parent result")
        checked = work / "checked-hierarchy-originals"
        shutil.copytree(originals, checked)
        receipt = change_signature(checked / "fixture/First.class",
                                   b"java/lang/Exception", b"java/lang/Object")
        (work / "checked-hierarchy-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        checked_dex = dex(checked, work / "checked-hierarchy-dex")
        rejected = work / "checked-hierarchy-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(checked_dex),
                                 *map(str, classpath), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "checked-hierarchy-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists():
            raise RuntimeError("Fixed covariance accepted a non-Throwable exception hierarchy")
        invalid = work / "checked-hierarchy-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "First.java":
                if content.count("extends Exception") != 1:
                    raise RuntimeError("Missing exact owned unchecked ancestor counterexample")
                content = content.replace("extends Exception", "extends Object", 1)
            destination = invalid / source.relative_to(fixture)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "checked-hierarchy-compiler"), *compiler_inputs], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "checked-hierarchy-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or not any(word in compiler.stderr for word in ("throw", "override")):
            raise RuntimeError("Independent javac accepted a non-Throwable exception declaration")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 3, "value_cases": 54, "identity_cases": 1728,
                    "state_cases": 180, "failure_cases": 126,
                    "profile": "dependency-root" if args.dependency_root else "source-root",
                    "original_fixed_exception_order_and_complete_parent_result": "retained",
                    "original_covariant_bridge_dispatch": "matched",
                    "mutable_aliases_virtual_dispatch_and_late_cast_order": "matched",
                    "complete_original_metadata_and_runtime_inventory": "unchanged except reported helpers and compiler bridge regeneration",
                    "same_code_generic_parent_result_counterexample": "rejected by NeverD and independent javac",
                    "same_code_non_throwable_ancestor_counterexample": "rejected by NeverD and independent javac",
                    "input_harness_and_sdk_bytes": "unchanged", "coverage": coverage["status"]}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
