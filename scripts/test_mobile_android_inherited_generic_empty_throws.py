#!/usr/bin/env python3
"""Verify inherited generic implementations with empty fixed exception overrides."""
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
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-interface", action="store_true")
    profile = parser.add_mutually_exclusive_group()
    profile.add_argument("--parameterized-return", action="store_true")
    profile.add_argument("--parameterized-throws", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-inherited-generic-empty-throws-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "inherited_generic_empty_throws/java"
        inputs = sorted(fixture.rglob("*.java"))
        if args.parameterized_return or args.parameterized_throws:
            authored = work / "parameterized-java"
            adjusted = []
            for source in inputs:
                content = source.read_text()
                if args.parameterized_return and source.name == "Parent.java":
                    end = content.rfind("}")
                    if end < 0 or " wrap(" in content:
                        raise RuntimeError("Missing exact owned generic parent overlay")
                    content = content[:end] + (
                        "    protected P wrap(Object input) {\n"
                        "        calls++;\n        events = events * 10 + 3;\n"
                        "        seenInput = input;\n        if (failure != null) throw failure;\n"
                        "        return result;\n    }\n") + content[end:]
                destination = authored / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                adjusted.append(destination)
            extra_fixture = FIXTURES / ("parameterized_raw_returns/java" if args.parameterized_return
                                        else "parameterized_fixed_throws/java")
            for source in sorted(extra_fixture.rglob("*.java")):
                destination = authored / source.relative_to(extra_fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(source.read_text())
                adjusted.append(destination)
            fixture, inputs = authored, adjusted
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        harness_inputs = [FIXTURES / "harness/InheritedGenericEmptyThrowsHarness.java"]
        if args.parameterized_return:
            harness_inputs.append(FIXTURES / "harness/ParameterizedRawReturnsHarness.java")
        elif args.parameterized_throws:
            harness_inputs.append(FIXTURES / "harness/ParameterizedFixedThrowsHarness.java")
        verify.compile_local(harness_inputs, harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        all_classes, all_methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        classes = all_classes - ({"Lfixture/Api;"} if args.dependency_interface else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        expected_inventory = (((5, 12) if args.dependency_interface else (6, 13))
                              if args.parameterized_return
                              else ((6, 13) if args.dependency_interface else (7, 14))
                              if args.parameterized_throws
                              else ((4, 7) if args.dependency_interface else (5, 8)))
        if (len(classes), len(methods)) != expected_inventory:
            raise RuntimeError("Owned inherited empty throws inventory changed")

        def write_api(root: Path, destination: Path) -> None:
            with zipfile.ZipFile(destination, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("fixture/Api.class", (root / "fixture/Api.class").read_bytes())

        dependency = work / "owned-api.jar"
        write_api(originals, dependency)
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_interface:
            classpath += ["--classpath", dependency]

        def dex(root: Path, output: Path, declaration_arguments=None) -> Path:
            paths = [p for p in sorted(root.rglob("*.class"))
                     if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            output.mkdir()
            arguments = classpath if declaration_arguments is None else declaration_arguments
            verify.run([*verify.d8, "--debug", "--min-api", "24", *arguments,
                        "--output", output, *paths], "d8-" + output.name)
            result = output / "classes.dex"
            if not result.is_file() or list(output.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned inherited empty throws fixture did not produce one DEX")
            return result

        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "InheritedGenericEmptyThrowsHarness"], "baseline")
        expected = ["values=30,identities=128,states=51,failures=21",
                    "fixed-interface-exceptions=retained,empty-generic-provider-exceptions=retained,original-provider-owner=retained"]
        if args.parameterized_return:
            baseline += verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                                    "ParameterizedRawReturnsHarness"], "raw-baseline")
            expected += ["raw-values=8,raw-identities=32,raw-states=16,raw-failures=8",
                         "raw-parent-result=retained,parameterized-child-result=retained,protected-original-bridge=retained"]
        elif args.parameterized_throws:
            baseline += verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                                    "ParameterizedFixedThrowsHarness"], "throws-baseline")
            expected += ["throws-values=30,throws-identities=198,throws-states=64,throws-failures=34",
                         "complete-parameterized-types=retained,fixed-and-empty-target-exceptions=retained,ordered-original-bridge-exceptions=retained"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned inherited empty throws baseline missed dispatch, identity or effect cases: " + baseline)
        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed original inherited empty throws inventory")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        bridge = "Lfixture/RawChild;->wrap(Ljava/lang/Object;)Ljava/lang/Object;"
        expected_bridges = {bridge} if args.parameterized_return else set()
        if args.parameterized_throws:
            expected_bridges = {"Lfixture/" + owner + ";->convert(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"
                                for owner in ("CollectionCodec", "EmptyCollectionCodec")}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods)
                or coverage.get("projected_method_count", 0) != len(expected_bridges)
                or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != ("partial" if expected_bridges else "recovered")
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != expected_bridges):
            raise RuntimeError("Inherited empty throws proof changed original scopes or copied an inherited method")
        if args.parameterized_return and (
                rows[bridge].get("projection_kind") != "generic-superclass-bridge"
                or rows[bridge].get("bridge_target") != "Lfixture/RawChild;->wrap(Ljava/lang/Object;)Ljava/util/concurrent/atomic/AtomicReference;"
                or rows[bridge].get("bridge_dispatch") != "Lfixture/Parent;->wrap(Ljava/lang/Object;)Ljava/lang/Object;"):
            raise RuntimeError("Parameterized raw return lost its original protected bridge or inherited dispatch")
        if args.parameterized_throws:
            for owner in ("CollectionCodec", "EmptyCollectionCodec"):
                entry = rows["Lfixture/" + owner + ";->convert(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"]
                if (entry.get("projection_kind") != "generic-interface-bridge"
                        or entry.get("bridge_target") != "Lfixture/" + owner + ";->convert(Ljava/lang/Object;Ljava/util/Collection;)Ljava/util/Collection;"
                        or entry.get("bridge_dispatch") != "Lfixture/Api;->convert(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"):
                    raise RuntimeError("Parameterized fixed exceptions lost an original interface bridge or complete target")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_interface else None,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != methods | helpers:
            raise RuntimeError("Inherited empty throws recompile changed inventory beyond reported helpers")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            wanted, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != wanted.get(key):
                    raise RuntimeError("Original inherited empty throws class metadata changed: " + owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original generic provider method metadata changed: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "InheritedGenericEmptyThrowsHarness"], "rebuilt-execution")
        if args.parameterized_return:
            actual += verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                                  "ParameterizedRawReturnsHarness"], "raw-rebuilt-execution")
        elif args.parameterized_throws:
            actual += verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                                  "ParameterizedFixedThrowsHarness"], "throws-rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Inherited generic provider identity, dispatch or ordered effects changed")

        mutations = {
            "wrong-parameter": ("Parent", b"(Ljava/lang/Object;TP;)TP;", b"(Ljava/lang/Object;Ljava/lang/Object;)TP;"),
            "wrong-result": ("Parent", b"(Ljava/lang/Object;TP;)TP;", b"(Ljava/lang/Object;TP;)Ljava/lang/Object;"),
            "non-throwable": ("Api", b"java/io/IOException", b"java/lang/String"),
        }
        if args.parameterized_return:
            mutations["parameterized-required-result"] = (
                "RawChild", b"Lfixture/Parent<Ljava/util/concurrent/atomic/AtomicReference;>;",
                b"Lfixture/Parent<Ljava/util/concurrent/atomic/AtomicReference<Ljava/lang/String;>;>;")
        elif args.parameterized_throws:
            for owner, label in (("CollectionCodec", "collection-parameter"),
                                 ("EmptyCollectionCodec", "empty-collection-parameter")):
                mutations[label] = (owner,
                    b"(Ljava/lang/Object;Ljava/util/Collection<*>;)Ljava/util/Collection<*>;",
                    b"(Ljava/lang/Object;Ljava/util/Collection<Ljava/lang/String;>;)Ljava/util/Collection<*>;")
            mutations["collection-scope"] = ("CollectionCodec",
                b"Ljava/lang/Object;Lfixture/Api<Ljava/util/Collection<*>;>;",
                b"Ljava/lang/Object;Lfixture/Api<Ljava/util/Collection<Ljava/lang/String;>;>;")
            mutations["collection-expanded-throws"] = ("CollectionCodec", b"java/io/IOException", b"java/lang/Throwable")
        for label, (owner, before, after) in mutations.items():
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / ("fixture/" + owner + ".class"), before, after)
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_classpath = list(classpath)
            if args.dependency_interface and owner == "Api":
                negative_dependency = work / (label + "-api.jar")
                write_api(negative, negative_dependency)
                negative_classpath[-1] = negative_dependency
            negative_dex = dex(negative, work / (label + "-dex"), negative_classpath)
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, negative_classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if (result.returncode == 0 or rejected.exists()
                    or not any(word in result.stderr for word in ("generic", "throws", "Throwable"))):
                raise RuntimeError("Inherited empty throws accepted a different complete type or malformed exception")
        compiler_mutations = {
            "wrong-parameter": ("P convert(Object input, P fallback)", "P convert(Object input, Object fallback)"),
            "wrong-result": ("P convert(Object input, P fallback)", "Object convert(Object input, P fallback)"),
            "expanded-throws": ("P convert(Object input, P fallback)", "P convert(Object input, P fallback) throws Exception"),
        }
        if args.parameterized_return:
            compiler_mutations["parameterized-required-result"] = (
                "extends Parent<AtomicReference>", "extends Parent<AtomicReference<String>>")
        elif args.parameterized_throws:
            compiler_mutations["collection-parameter"] = ("Collection<?> fallback", "Collection<String> fallback")
            compiler_mutations["empty-collection-parameter"] = ("Collection<?> fallback", "Collection<String> fallback")
            compiler_mutations["collection-scope"] = ("implements Api<Collection<?>>", "implements Api<Collection<String>>")
            compiler_mutations["collection-expanded-throws"] = ("throws java.io.IOException, ReflectiveOperationException", "throws Throwable")
        for label, (before, after) in compiler_mutations.items():
            invalid = work / (label + "-java")
            compiler_inputs = []
            for source in inputs:
                content = source.read_text()
                target_source = "RawChild.java" if label == "parameterized-required-result" else "Parent.java"
                if label.startswith("collection-"):
                    target_source = "CollectionCodec.java"
                elif label.startswith("empty-collection-"):
                    target_source = "EmptyCollectionCodec.java"
                if source.name == target_source:
                    if content.count(before) != 1:
                        raise RuntimeError("Missing exact owned generic provider compiler counterexample")
                    content = content.replace(before, after, 1)
                destination = invalid / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                compiler_inputs.append(str(destination))
            compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                       str(work / (label + "-compiler")), *compiler_inputs], text=True,
                                      capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-compiler.log")).write_text(compiler.stdout + compiler.stderr)
            if compiler.returncode == 0 or not any(word in compiler.stderr for word in ("clash", "throw", "override", "incompatible")):
                raise RuntimeError("Independent javac accepted an incompatible generic or checked throws override")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original classes, immutable harness or declaration bytes changed")
        evidence = {"status": "success", "dependency_interface": args.dependency_interface,
                    "parameterized_return": args.parameterized_return,
                    "parameterized_throws": args.parameterized_throws,
                    "class_count": len(classes), "method_count": len(methods), "projected_methods": len(expected_bridges),
                    "value_cases": 30, "identity_cases": 128, "state_cases": 51, "failure_cases": 21,
                    "complete_original_metadata_and_inventory": "unchanged except reported helpers",
                    "original_generic_provider_and_fixed_exception_declarations": "retained",
                    "generic_and_fixed_children_dispatch_identity_and_effects": "matched",
                    "same_Code_parameter_result_and_non_Throwable_counterexamples": "rejected",
                    "independent_javac_generic_and_checked_throws_counterexamples": "rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged",
                    "coverage": "partial" if expected_bridges else "recovered"}
        if args.parameterized_return:
            evidence["parameterized_raw_return"] = {
                "value_cases": 8, "identity_cases": 32, "state_cases": 16, "failure_cases": 8,
                "original_raw_parent_parameterized_target_and_protected_bridge": "retained",
                "same_Code_and_independent_javac_parameterized_required_result": "rejected"}
        elif args.parameterized_throws:
            evidence["parameterized_fixed_throws"] = {
                "value_cases": 30, "identity_cases": 198, "state_cases": 64, "failure_cases": 34,
                "original_parameterized_targets_and_ordered_bridge_exceptions": "retained",
                "same_Code_and_independent_javac_type_and_expanded_exception_counterexamples": "rejected"}
        (work / "verified.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
