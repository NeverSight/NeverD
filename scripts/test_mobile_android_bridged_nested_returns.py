#!/usr/bin/env python3
"""Verify witnessed nested result variance through an original protected parameter bridge."""
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

BRIDGE_DESCRIPTOR = b"(Ljava/lang/Object;)Ljava/util/Iterator;"
SOURCE_SIGNATURE = b"(Lfixture/Token<TK;TV;>;)Ljava/util/Iterator<Lfixture/Pair<+TK;+TV;>;>;"
INPUT_SIGNATURE = b"(Lfixture/Token<TK;TV;>;)Ljava/util/Iterator<Lfixture/Pair<TK;TV;>;>;"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parent", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-bridged-nested-returns-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "bridged_nested_returns/java/fixture"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/BridgedNestedReturnHarness.java", FIXTURES / "harness/BridgedNestedReturnCaller.java"], harness, classpath=originals)
        encoded = change_method_flags(originals / "fixture/Core.class", BRIDGE_DESCRIPTOR,
                                      0x1044, 0x1041, name=b"values")
        (work / "original-bridge-flags.json").write_text(json.dumps(encoded, indent=2) + "\n")
        receipt = change_signature(originals / "fixture/Core.class", SOURCE_SIGNATURE, INPUT_SIGNATURE)
        (work / "original-result-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Base;", "Lfixture/Token;", "Lfixture/Pair;"} if args.dependency_parent else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        original_exceptions = exception_inventory(originals)
        dependency = work / "owned-parent.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Base", "Token", "Pair"):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_parent:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "BridgedNestedReturnHarness", "original"], "baseline")
        counts = (46, 136, 100, 60)
        if baseline.splitlines() != ["values=46,identities=136,states=100,failures=60",
                "nested-variance-and-access=reported,original-bridge-descriptor-and-effects=retained"]:
            raise RuntimeError("Owned protected consumer dispatch matrix changed: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned consumer bridge fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original consumer bridge runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        target = "Lfixture/Core;->values(Lfixture/Token;)Ljava/util/Iterator;"
        bridge = "Lfixture/Core;->values(Ljava/lang/Object;)Ljava/util/Iterator;"
        targets, bridges = {target}, {bridge}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 2 or projected != targets | bridges
                or coverage["status"] != "partial"
                or coverage.get("class_source_bindings")):
            raise RuntimeError("Consumer access projection changed inventory or unrelated generic signatures")
        projections = coverage.get("generic_signature_projections", [])
        if (len(projections) != 1 or projections[0]["declaration"] != target
                or projections[0]["original_signature"] != INPUT_SIGNATURE.decode()
                or len(projections[0]["changes"]) != 1
                or projections[0]["changes"][0]["kind"] != "inherited-nested-return-variance"
                or projections[0]["changes"][0]["source_type"] != SOURCE_SIGNATURE.decode()
                or projections[0]["changes"][0]["scope_origin"] != "Lfixture/Base;->values(Ljava/lang/Object;)Ljava/util/Iterator;"):
            raise RuntimeError("Nested return projection lost its complete inherited witness")
        if (rows[target].get("projection_kind") != "public-erased-protected-parameter-target"
                or rows[target].get("original_bridge") != bridge or rows[bridge].get("bridge_target") != target
                or not rows[target].get("metadata_differences") or not rows[bridge].get("metadata_differences")):
            raise RuntimeError("Original public erased entry or target access difference was not reported")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_parent else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {entry["class"] + "->" + entry["name"] + entry["prototype"]
                   for entry in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != methods | helpers:
            raise RuntimeError("Rebuilt consumer access projection changed an original runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        rebuilt_exceptions = exception_inventory(rebuilt)
        for method in methods:
            if rebuilt_exceptions.get(method) != original_exceptions[method]:
                raise RuntimeError("Original consumer bridge Exceptions metadata changed")
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original consumer class or field metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if method in targets:
                    wanted["access"] = 1
                    wanted["signature"] = SOURCE_SIGNATURE.decode()
                if after["methods"][method] != wanted:
                    raise RuntimeError("Original method metadata changed outside target visibility: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_parent else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             "BridgedNestedReturnHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original consumer state, checked cast, access or callback effects changed")
        for label in ("missing-synthetic", "parameter-scope", "result-scope", "lower-witness"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "missing-synthetic":
                receipt = change_method_flags(negative / "fixture/Core.class", BRIDGE_DESCRIPTOR, 0x1041, 0x41, name=b"values")
            elif label == "parameter-scope":
                receipt = change_signature(negative / "fixture/Core.class", INPUT_SIGNATURE,
                    INPUT_SIGNATURE.replace(b"Token<TK;TV;>", b"Token<Ljava/lang/Object;TV;>"))
            elif label == "result-scope":
                receipt = change_signature(negative / "fixture/Core.class", INPUT_SIGNATURE,
                    INPUT_SIGNATURE.replace(b"Pair<TK;TV;>", b"Pair<TV;TK;>"))
            else:
                receipt = change_signature(negative / "fixture/Middle.class",
                    b"<K:Ljava/lang/Object;V:Ljava/lang/Object;>Lfixture/Base<Lfixture/Pair<+TK;+TV;>;Lfixture/Token<TK;TV;>;>;",
                    b"<K:Ljava/lang/Object;V:Ljava/lang/Object;>Lfixture/Base<Lfixture/Pair<-TK;+TV;>;Lfixture/Token<TK;TV;>;>;")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Incomplete original closed consumer access proof admitted: " + label)
        invalid = work / "wrong-parent-java/fixture/Core.java"
        invalid.parent.mkdir(parents=True)
        content = (fixture / "Core.java").read_text()
        if content.count("Iterator<Pair<? extends K,? extends V>> values") != 1:
            raise RuntimeError("Missing owned nested return counterexample")
        invalid.write_text(content.replace("Iterator<Pair<? extends K,? extends V>> values",
                                           "Iterator<Pair<K,V>> values", 1))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-cp", str(originals), "-d",
                                   str(work / "wrong-parent-compiler"), str(invalid)], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-parent-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Independent javac accepted an incompatible closed consumer override")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 2, "value_cases": counts[0], "identity_cases": counts[1],
                    "state_cases": counts[2], "failure_cases": counts[3],
                    "profile": "dependency-parent" if args.dependency_parent else "source-parent",
                    "original_public_erased_entries_and_protected_parent_callbacks": "retained",
                    "typed_target_access": "protected to public; reported",
                    "nested_return_variance": "complete inherited witness; reported",
                    "nullable_mutable_inputs_checked_casts_and_ordered_effects": "matched",
                    "original_inventory_and_other_metadata": "unchanged except reported private helpers",
                    "same_Code_flags_and_parent_binding_counterexamples": "rejected",
                    "independent_javac_incompatible_consumer_override": "rejected",
                    "input_harness_and_sdk_bytes": "unchanged", "coverage": coverage["status"]}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
