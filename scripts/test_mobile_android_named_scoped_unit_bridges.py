#!/usr/bin/env python3
"""Compare named scoped void/Unit callbacks with immutable JVM callers."""
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
    from scripts.test_mobile_android_void_unit_bridges import encode_void_bridge
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_void_unit_bridges import encode_void_bridge
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parents", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-named-scoped-unit-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "named_scoped_unit_bridges/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        encoding = encode_void_bridge(originals / "fixture/Core.class",
                b"<E:Ljava/lang/Object;>Lfixture/Parent<TE;>;Lfixture/BiTask<TE;Ljava/lang/Throwable;Lkotlin/Unit;>;",
                original_signature=b"<E:Ljava/lang/Object;>Lfixture/Parent<TE;>;Lfixture/BiTask;", method_name=b"apply")
        (work / "owned-input-encoding.json").write_text(json.dumps(encoding, indent=2) + "\n")
        verify.compile_local([FIXTURES / "harness/NamedScopedUnitHarness.java"], harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        parents = {"Lfixture/Parent;", "Lfixture/BiTask;"} if args.dependency_parents else set()
        classes = all_classes - parents
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, original_exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-parents.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Parent", "BiTask"):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_parents:
            classpath += ["--classpath", dependency]
        baseline = []
        for route in (None, 0, 1, 2, 3):
            label = "normal" if route is None else "init-" + str(route)
            output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                    "NamedScopedUnitHarness", "apply", *([] if route is None else [str(route)])], "baseline-" + label)
            expected = "values=14,identities=363,states=258,failures=77" if route is None else "values=0,identities=5,states=8,failures=2"
            if output.splitlines() != [expected, "reflection=original"]:
                raise RuntimeError("Owned scoped Unit oracle matrix changed: " + output)
            baseline.append((route, label, output))

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned Unit fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original scoped Unit runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        target = "Lfixture/Core;->apply(Ljava/lang/Object;Ljava/lang/Throwable;)V"
        bridge = "Lfixture/Core;->apply(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 2 or projected != {target, bridge}
                or coverage["status"] != "partial" or coverage.get("generic_signature_projections")
                or coverage.get("class_source_bindings")):
            raise RuntimeError("Named Unit projection changed inventory or generic signatures")
        if (rows[target].get("projection_kind") != "void-unit-target"
                or rows[bridge].get("projection_kind") != "void-unit-interface-bridge"
                or rows[bridge].get("bridge_target") != target
                or rows[bridge].get("result_field") != "Lkotlin/Unit;->INSTANCE:Lkotlin/Unit;"
                or not rows[target].get("metadata_differences") or not rows[bridge].get("metadata_differences")):
            raise RuntimeError("Original Unit call or source method mapping was not reported")
        source_name = rows[target]["bridge_source_name"]
        if source_name != "__neverdVoidBridgeTarget_":
            raise RuntimeError("Unit target rename collided with an inherited method")
        renamed, wrapper = rows[target]["source_identity"], rows[bridge]["source_identity"]
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_parents else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {entry["class"] + "->" + entry["name"] + entry["prototype"]
                   for entry in coverage.get("generated_source_helpers", [])}
        expected_methods = (methods - {target}) | {renamed, wrapper} | helpers
        if rebuilt_classes != classes or set(rebuilt_methods) != expected_methods:
            raise RuntimeError("Rebuilt Unit source changed entries outside reported method mapping")
        actual_records, rebuilt_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for identity in methods:
            source_identity = renamed if identity == target else identity
            if rebuilt_exceptions.get(source_identity) != original_exceptions[identity]:
                raise RuntimeError("Original Unit Exceptions metadata changed")
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original Unit class or field metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                source_method = renamed if method == target else method
                expected_metadata = dict(metadata)
                if method == target:
                    expected_metadata["name"] = source_name
                if after["methods"][source_method] != expected_metadata:
                    raise RuntimeError("Original method metadata changed outside reported rename: " + method)
        wrapper_metadata = actual_records["Lfixture/Core;"]["methods"][wrapper]
        if (wrapper_metadata["access"] != 1 or wrapper_metadata.get("signature") is not None
                or rebuilt_exceptions.get(wrapper) not in (None, [])):
            raise RuntimeError("Added Unit wrapper changed its reported erased parameter shape")
        runtime = (rebuilt, harness, dependency) if args.dependency_parents else (rebuilt, harness)
        for route, label, original in baseline:
            actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                    "NamedScopedUnitHarness", source_name, *([] if route is None else [str(route)])], "rebuilt-" + label)
            expected = original.replace("reflection=original", "reflection=reported-source-mapping")
            if actual != expected:
                raise RuntimeError("Original Unit call order, identity or initialization changed")
        for label in ("missing-synthetic", "method-formal"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "missing-synthetic":
                receipt = change_method_flags(negative / "fixture/Core.class",
                        b"(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", 0x1041, 0x41, name=b"apply")
            else:
                receipt = change_signature(negative / "fixture/Core.class", b"(TE;Ljava/lang/Throwable;)V",
                        b"<M:Ljava/lang/Object;>(TM;Ljava/lang/Throwable;)V")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Incomplete named Unit proof admitted: " + label)
        invalid = work / "wrong-scope-java/fixture/Wrong.java"
        invalid.parent.mkdir(parents=True)
        invalid.write_text("package fixture; public final class Wrong<E> implements BiTask<E, Throwable, kotlin.Unit> {\n"
                           "  public <M> kotlin.Unit apply(M value, Throwable error) { return null; }\n}\n")
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-cp", str(originals), "-d",
                                   str(work / "wrong-scope-compiler"), str(invalid)], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-scope-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Independent javac accepted a method formal in place of the class contract")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 2, "value_cases": 14, "identity_cases": 383,
                    "state_cases": 290, "failure_cases": 85,
                    "profile": "dependency-parents" if args.dependency_parents else "source-parents",
                    "original_erased_bridge_and_generic_scope": "retained",
                    "void_target_rename_and_added_Unit_wrapper": "reported; caller references use mapping",
                    "inherited_method_collision": "avoided",
                    "typed_void_calls": "no Unit initialization",
                    "post_call_singleton_initialization_and_erroneous_class_state": "matched in fresh JVMs",
                    "all_other_original_metadata": "unchanged",
                    "same_Code_bridge_flags_and_method_scope_counterexamples": "rejected",
                    "independent_javac_method_formal_conflict": "rejected",
                    "input_harness_and_sdk_bytes": "unchanged", "coverage": coverage["status"]}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
