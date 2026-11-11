#!/usr/bin/env python3
"""Verify an original child bridge to an inherited final covariant getter."""
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
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory

BRIDGE_DESCRIPTOR = b"()Lfixture/Broad;"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-inherited-final-covariance-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "inherited_final_covariance/java/fixture"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/InheritedFinalCovarianceHarness.java"], harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        declarations = {"Lfixture/" + n + ";" for n in ("Api", "Broad", "Narrow", "Provider")}
        classes = all_classes - (declarations if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        original_exceptions = exception_inventory(originals)
        dependency = work / "owned-parent.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Api", "Broad", "Narrow", "Provider"):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "InheritedFinalCovarianceHarness", "original"], "baseline")
        counts = (96, 672, 288, 144)
        if baseline.splitlines() != ["values=96,identities=672,states=288,failures=144",
                "original-inherited-final-provider-and-covariant-bridge=retained,complete-metadata=unchanged"]:
            raise RuntimeError("Owned inherited covariance dispatch matrix changed: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned covariant bridge fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original covariant bridge runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        target = "Lfixture/Provider;->get()Lfixture/Narrow;"
        bridge = "Lfixture/Core;->get()Lfixture/Broad;"
        bridges = {bridge}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 1 or projected != bridges
                or coverage["status"] != "partial" or coverage.get("generic_signature_projections")
                or coverage.get("class_source_bindings")):
            raise RuntimeError("Covariant bridge projection changed inventory or unrelated generic signatures")
        if (rows[bridge].get("projection_kind") != "generic-interface-bridge"
                or rows[bridge].get("bridge_target") != target
                or rows[bridge].get("bridge_dispatch") != "Lfixture/Api;->get()Lfixture/Broad;"):
            raise RuntimeError("Original inherited final provider was not reported")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {entry["class"] + "->" + entry["name"] + entry["prototype"]
                   for entry in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != methods | helpers:
            raise RuntimeError("Rebuilt covariant bridge projection changed an original runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        rebuilt_exceptions = exception_inventory(rebuilt)
        for method in methods:
            if rebuilt_exceptions.get(method) != original_exceptions[method]:
                raise RuntimeError("Original covariant bridge Exceptions metadata changed")
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original covariant class or field metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if after["methods"][method] != wanted:
                    raise RuntimeError("Original method metadata changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             "InheritedFinalCovarianceHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original covariant results, aliases or provider effects changed")
        for label in ("missing-synthetic", "wrong-interface-formal"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "missing-synthetic":
                receipt = change_method_flags(negative / "fixture/Core.class", BRIDGE_DESCRIPTOR, 0x1041, 0x41, name=b"get")
            else:
                receipt = change_signature(negative / "fixture/Core.class", b"<S:Ljava/lang/Object;V:Ljava/lang/Object;>Lfixture/Provider<TS;TV;>;Lfixture/Api<TS;>;",
                                           b"<S:Ljava/lang/Object;V:Ljava/lang/Object;>Lfixture/Provider<TS;TV;>;Lfixture/Api<TV;>;")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Incomplete original covariant getter access proof admitted: " + label)
        invalid = work / "wrong-parent-java/fixture/Core.java"
        invalid.parent.mkdir(parents=True)
        content = (fixture / "Core.java").read_text()
        if content.count("implements Api<S>") != 1:
            raise RuntimeError("Missing owned inherited covariance counterexample")
        invalid.write_text(content.replace("implements Api<S>", "implements Api<V>", 1))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-cp", str(originals), "-d",
                                   str(work / "wrong-parent-compiler"), str(invalid)], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-parent-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Independent javac accepted an incompatible covariant getter override")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 1, "value_cases": counts[0], "identity_cases": counts[1],
                    "state_cases": counts[2], "failure_cases": counts[3],
                    "profile": "dependency-provider" if args.dependency_provider else "source-provider",
                    "inherited_final_provider_and_original_bridge": "retained with unchanged complete metadata",
                    "nullable_mutable_results_and_ordered_effects": "matched",
                    "independent_subclass_and_reflection_dispatch": "matched",
                    "original_inventory_and_exceptions": "unchanged except reported private helpers",
                    "same_Code_flags_and_formal_binding_counterexamples": "rejected",
                    "independent_javac_incompatible_covariance": "rejected",
                    "input_harness_dependency_and_sdk_bytes": "unchanged", "coverage": coverage["status"]}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
