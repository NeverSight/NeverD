#!/usr/bin/env python3
"""Verify package-inaccessible class methods through source and SDK generic chains."""
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
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-cross-package-ancestors-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "cross_package_ancestors/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        original_hashes = hashes(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        declarations = {"Lfixture/base/Root;", "Lfixture/base/Parent;"}
        classes = all_classes - declarations if args.dependency_parent else all_classes
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        dependency = work / "owned-parent.jar"

        def write_dependency(root: Path, target: Path) -> None:
            with zipfile.ZipFile(target, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                for path in sorted(root.rglob("*.class")):
                    relative = path.relative_to(root).as_posix()
                    if "L" + relative[:-6] + ";" in declarations:
                        archive.writestr(relative, path.read_bytes())

        write_dependency(originals, dependency)
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_parent:
            classpath += ["--classpath", dependency]

        def dex(root: Path, output: Path) -> Path:
            paths = [p for p in sorted(root.rglob("*.class"))
                     if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", output, *paths], "d8-" + output.name)
            result = output / "classes.dex"
            if not result.is_file() or list(output.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned package inheritance did not produce one DEX")
            return result

        harness_name = "CrossPackageInheritanceHarness"
        verify.compile_local([FIXTURES / ("harness/" + harness_name + ".java")], harness,
                             classpath=originals)
        harness_hashes = hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_name], "baseline")
        if baseline.splitlines() != ["values=32,identities=32,failures=8,bridges=1",
                "package-dispatch=retained,hidden-final-and-static-members=independent,generic-scopes=retained"]:
            raise RuntimeError("Owned cross-package baseline missed an expected dispatch case")
        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed the original package inheritance inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["projected_method_count"] != 1 or coverage["unrecovered_method_count"] != 0
                or coverage.get("generic_signature_projections") or coverage["status"] != "partial"):
            raise RuntimeError("Package visibility changed an original descriptor or generic contract")
        for row in coverage["methods"]:
            if row["status"] == "source-projected" and (row["projection_kind"] != "generic-superclass-bridge"
                    or row["bridge_target"] not in methods or row["bridge_dispatch"] not in all_methods
                    or row["bridge_dispatch"] != "Lfixture/base/Root;->echo(Ljava/lang/Object;)Ljava/lang/Object;"):
                raise RuntimeError("Package visibility invented an unrelated forwarding entry")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_parent else None)
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
                    raise RuntimeError("Original cross-package class metadata changed: " + owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original cross-package method metadata changed: " + identity)
        runtime_path = (rebuilt, harness, dependency) if args.dependency_parent else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime_path)),
                             harness_name], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Hidden ancestor dispatch, state or exception ordering changed")

        for label in ("visible-final", "visible-static", "formal-scope"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "formal-scope":
                receipt = change_signature(negative / "fixture/client/Child.class", b"(TS;)TS;", b"(TU;)TS;")
            elif label == "visible-final":
                receipt = change_method_flags(negative / "fixture/base/Root.class",
                    b"(Ljava/lang/Object;)Ljava/lang/Object;", 0x10, 0x11, name=b"hidden")
            else:
                receipt = change_method_flags(negative / "fixture/base/Root.class",
                    b"(I)I", 0x8, 0xc, name=b"mode")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            negative_classpath = list(classpath)
            if args.dependency_parent:
                changed_parent = work / (label + "-parent.jar")
                write_dependency(negative, changed_parent)
                negative_classpath[-1] = changed_parent
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, negative_classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Package proof skipped a visible competitor or different full formal")
        negative_java = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "Root.java":
                declaration = "final T hidden(T value)"
                if content.count(declaration) != 1:
                    raise RuntimeError("Missing original package-final source declaration")
                content = content.replace(declaration, "public " + declaration, 1)
            destination = negative_java / source.relative_to(fixture)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if (compiler.returncode == 0 or "name clash" not in compiler.stderr
                or "hidden(" not in compiler.stderr or "same erasure" not in compiler.stderr):
            raise RuntimeError("Independent javac did not reject the now-visible ancestor method")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original classes, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "dependency_parent": args.dependency_parent,
                    "class_count": len(classes), "method_count": len(methods), "bridge_count": 1,
                    "value_cases": 32, "identity_cases": 32, "failure_cases": 8,
                    "hidden_package_final_and_static_dispatch": "unchanged",
                    "complete_metadata_and_inventory": "unchanged except reported helpers",
                    "visible_final_static_and_scope_counterexamples": "rejected with original Code",
                    "independent_javac_visibility_counterexample": "rejected",
                    "original_input_harness_and_sdk_bytes": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
