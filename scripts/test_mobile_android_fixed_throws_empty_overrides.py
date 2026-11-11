#!/usr/bin/env python3
"""Verify empty implementation throws beneath fixed plain interface exceptions."""
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
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-fixed-throws-empty-overrides-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "fixed_throws_empty_overrides/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/FixedThrowsEmptyOverridesHarness.java"],
                             harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        all_classes, all_methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        classes = all_classes - ({"Lfixture/Handler;"} if args.dependency_interface else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}

        def write_interface(root: Path, destination: Path) -> None:
            with zipfile.ZipFile(destination, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("fixture/Handler.class", (root / "fixture/Handler.class").read_bytes())

        dependency = work / "owned-handler.jar"
        write_interface(originals, dependency)
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_interface:
            classpath += ["--classpath", dependency]

        def dex(root: Path, output: Path, dependency_arguments=None) -> Path:
            paths = [p for p in sorted(root.rglob("*.class"))
                     if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            output.mkdir()
            arguments = classpath if dependency_arguments is None else dependency_arguments
            verify.run([*verify.d8, "--debug", "--min-api", "24", *arguments,
                        "--output", output, *paths], "d8-" + output.name)
            result = output / "classes.dex"
            if not result.is_file() or list(output.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned fixed throws fixture did not produce one DEX")
            return result

        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "FixedThrowsEmptyOverridesHarness"], "baseline")
        expected = ["values=20,identities=280,states=50,failures=30",
                    "fixed-interface-throws=retained,empty-parent-throws=retained,bridge-casts-and-ordered-effects=matched"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned fixed throws baseline missed a dispatch or ordered failure case")
        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed the original fixed throws inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        bridge = "Lfixture/Host$Child;->test(Ljava/lang/Object;Ljava/lang/Object;)Z"
        typed = "Lfixture/Host$Child;->test(Ljava/lang/Object;Ljava/lang/String;)Z"
        dispatch = "Lfixture/Host$Parent;->test(Ljava/lang/Object;Ljava/lang/Object;)Z"
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["projected_method_count"] != 1
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != {bridge}
                or rows[bridge].get("projection_kind") != "generic-superclass-bridge"
                or rows[bridge].get("bridge_target") != typed
                or rows[bridge].get("bridge_dispatch") != dispatch):
            raise RuntimeError("Empty throws override lost an original contract or added an unrelated projection")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_interface else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != methods | helpers:
            raise RuntimeError("Recompile changed inventory beyond reported source helpers")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            wanted, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != wanted.get(key):
                    raise RuntimeError("Original fixed throws class metadata changed: " + owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original fixed throws method metadata changed: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "FixedThrowsEmptyOverridesHarness"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original empty throws, fixed exceptions, bridge casts or ordered effects changed")

        for label in ("incompatible-parent-scope", "non-throwable-interface"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "incompatible-parent-scope":
                receipt = change_signature(negative / "fixture/Host$Child.class",
                    b"Lfixture/Host$Parent<Ljava/lang/String;>;",
                    b"Lfixture/Host$Parent<Ljava/lang/Integer;>;")
            else:
                receipt = change_signature(negative / "fixture/Handler.class",
                                           b"java/lang/Throwable", b"java/lang/String")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_classpath = list(classpath)
            if args.dependency_interface:
                negative_dependency = work / (label + "-handler.jar")
                write_interface(negative, negative_dependency)
                negative_classpath[-1] = negative_dependency
            negative_dex = dex(negative, work / (label + "-dex"), negative_classpath)
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, negative_classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or not any(
                    word in result.stderr for word in ("generic", "throws", "Throwable")):
                raise RuntimeError("Empty throws handling bypassed a different scope or malformed exception type")
        for label in ("expanded-child-throws", "incompatible-parent-scope"):
            negative_java = work / (label + "-java")
            compiler_inputs = []
            for source in inputs:
                content = source.read_text()
                if source.name == "Host.java":
                    before, after = (("public boolean test(Object receiver, String value) {",
                                      "public boolean test(Object receiver, String value) throws Exception {")
                                     if label == "expanded-child-throws" else
                                     ("extends Parent<String>", "extends Parent<Integer>"))
                    if content.count(before) != 1:
                        raise RuntimeError("Missing exact owned fixed throws compiler counterexample")
                    content = content.replace(before, after, 1)
                destination = negative_java / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                compiler_inputs.append(str(destination))
            compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                       str(work / (label + "-compiler")), *compiler_inputs], text=True,
                                      capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-compiler.log")).write_text(compiler.stdout + compiler.stderr)
            if compiler.returncode == 0 or not any(word in compiler.stderr for word in ("throw", "override")):
                raise RuntimeError("Independent javac accepted expanded checked throws or incompatible scope")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original classes, immutable harness or declaration bytes changed")
        evidence = {"status": "success", "dependency_interface": args.dependency_interface,
                    "class_count": len(classes), "method_count": len(methods), "bridge_count": 1,
                    "value_cases": 20, "identity_cases": 280, "state_cases": 50, "failure_cases": 30,
                    "fixed_interface_and_empty_implementation_exceptions": "unchanged",
                    "parent_child_interface_and_reflective_dispatch": "matched",
                    "original_bridge_cast_and_runtime_failure_order": "matched",
                    "complete_original_metadata_and_inventory": "unchanged except reported helpers",
                    "same_code_scope_and_non_throwable_counterexamples": "rejected",
                    "independent_javac_scope_and_checked_throws_counterexamples": "rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
