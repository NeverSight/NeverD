#!/usr/bin/env python3
"""Verify complete specialized interface covariance through original erased bridges."""
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
        prefix="neverd-specialized-interface-covariance-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "specialized_interface_covariance/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/SpecializedInterfaceCovarianceHarness.java"], harness,
                             classpath=originals)
        owner = "Lfixture/Core;"
        runtime_method = owner + "->get(Lfixture/Carrier;)Ljava/lang/Object;"
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Contract;"} if args.dependency_interface else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        dependency = work / "owned-interface.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("fixture/Contract.class", (originals / "fixture/Contract.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_interface:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "SpecializedInterfaceCovarianceHarness", "original"], "baseline")
        expected = ["values=60,identities=540,states=90,failures=30",
                    "specialized-result=Number,original-result=Byte,erased-bridge=Object,complete-parameters-and-late-cast-order=retained"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned covariant result baseline missed a dispatch, identity or failure case: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if not result.is_file() or list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned covariant result fixture did not produce one DEX")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the original covariant result runtime inventory")
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
            raise RuntimeError("Specialized interface covariance changed original runtime inventory, scopes or projection receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_interface else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(methods) | helpers:
            raise RuntimeError("Recovered covariant result invented a bridge or runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                wanted = before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Original covariant result class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"][method] != metadata:
                    raise RuntimeError("Original covariant result method scope, access or throws changed: " + method)
        rebuilt_path = (rebuilt, harness, dependency) if args.dependency_interface else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, rebuilt_path)),
                             "SpecializedInterfaceCovarianceHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Direct, inherited, reflected or SDK covariant result execution changed")

        original_signature = b"(Lfixture/Carrier<Ljava/lang/Number;Ljava/lang/String;>;)Ljava/lang/Byte;"
        drift_signature = b"(Lfixture/Carrier<Ljava/lang/String;Ljava/lang/Number;>;)Ljava/lang/Byte;"
        negative = work / "parameter-drift-originals"
        shutil.copytree(originals, negative)
        receipt = change_signature(negative / "fixture/Core.class", original_signature, drift_signature)
        (work / "parameter-drift-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = dex(negative, work / "parameter-drift-dex")
        rejected = work / "parameter-drift-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                 *map(str, classpath), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "parameter-drift-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists():
            raise RuntimeError("Full parameter drift was accepted through an unchanged erased bridge")
        negative_java = work / "parameter-drift-java/fixture/Core.java"
        negative_java.parent.mkdir(parents=True)
        content = (fixture / "fixture/Core.java").read_text()
        declaration = "public Byte get(Carrier<Number, String> carrier)"
        if content.count(declaration) != 1:
            raise RuntimeError("Missing exact independent complete-parameter counterexample")
        content = content.replace(declaration, "public Byte get(Carrier<String, Number> carrier)", 1)
        negative_java.write_text(content)
        result = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-cp", str(originals), "-d",
                                 str(work / "parameter-drift-compiler"), str(negative_java)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "parameter-drift-compiler.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "name clash" not in result.stderr:
            raise RuntimeError("Independent javac did not reject the same-erasure parameter drift")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 1, "value_cases": 60, "identity_cases": 540,
                    "state_cases": 90, "failure_cases": 30,
                    "profile": "dependency-interface" if args.dependency_interface else "source-interface",
                    "complete_specialized_covariance_and_original_bridge": "retained",
                    "mutable_aliases_virtual_dispatch_and_late_cast_order": "matched",
                    "complete_original_metadata_and_runtime_inventory": "unchanged except reported helpers",
                    "same_code_parameter_drift_counterexample": "rejected by NeverD and independent javac",
                    "input_harness_and_sdk_bytes": "unchanged", "coverage": coverage["status"]}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
