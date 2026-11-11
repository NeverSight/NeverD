#!/usr/bin/env python3
"""Verify class-owned Object-bound returns satisfy complete plain interface slots."""
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
        prefix="neverd-class-formal-object-returns-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "class_formal_object_returns/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/ClassFormalObjectReturnsHarness.java"],
                             harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        all_classes, all_methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        classes = all_classes - ({"Lfixture/SourceView;"} if args.dependency_interface else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        dependency = work / "owned-source-view.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("fixture/SourceView.class", (originals / "fixture/SourceView.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_interface:
            classpath += ["--classpath", dependency]

        def dex(root: Path, output: Path) -> Path:
            paths = [p for p in sorted(root.rglob("*.class"))
                     if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", output, *paths], "d8-" + output.name)
            result = output / "classes.dex"
            if not result.is_file() or list(output.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned class-formal fixture did not produce one DEX")
            return result

        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "ClassFormalObjectReturnsHarness"], "baseline")
        expected = ["values=30,identities=140,states=50,failures=20",
                    "class-owned-return-formals=retained,plain-object-runtime-entries=unchanged,closed-child-members=not-invented"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned class-formal baseline missed a dispatch or identity case")
        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed the original class-formal inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage.get("projected_method_count", 0) != 0 or coverage["unrecovered_method_count"] != 0
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")):
            raise RuntimeError("Class-formal Object return changed original scope, entries or source metadata")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_interface else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != methods | helpers:
            raise RuntimeError("Recompile invented a bridge, child method or unreported helper")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            wanted, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != wanted.get(key):
                    raise RuntimeError("Original class-formal class metadata changed: " + owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original class-formal method metadata changed: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "ClassFormalObjectReturnsHarness"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original class-owned return, callback identity or ordered effects changed")

        for label in ("method-formal-shadow", "callback-arguments"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "method-formal-shadow":
                receipt = change_signature(negative / "fixture/Base.class",
                    b"()TT;", b"<T:Ljava/lang/Object;>()TT;")
            else:
                receipt = change_signature(negative / "fixture/Base.class",
                    b"(Lfixture/Callback<Ljava/lang/String;[B>;)TT;",
                    b"(Lfixture/Callback<[BLjava/lang/String;>;)TT;")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Class-formal return proof bypassed method scope or complete fixed parameters")
            negative_java = work / (label + "-java")
            compiler_inputs = []
            for source in inputs:
                content = source.read_text()
                if source.name == "Base.java":
                    before, after = (("public T getSource()", "public <T> T getSource()")
                                     if label == "method-formal-shadow" else
                                     ("public T echo(Callback<String, byte[]> callback)",
                                      "public T echo(Callback<byte[], String> callback)"))
                    if content.count(before) != 1:
                        raise RuntimeError("Missing exact owned class-formal compiler counterexample")
                    content = content.replace(before, after, 1)
                destination = negative_java / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                compiler_inputs.append(str(destination))
            compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                       str(work / (label + "-compiler")), *compiler_inputs], text=True,
                                      capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-compiler.log")).write_text(compiler.stdout + compiler.stderr)
            if compiler.returncode == 0 or not any(word in compiler.stderr for word in ("clash", "override")):
                raise RuntimeError("Independent javac accepted a method-owned scope or incompatible callback override")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original classes, immutable harness or declaration bytes changed")
        evidence = {"status": "success", "dependency_interface": args.dependency_interface,
                    "class_count": len(classes), "method_count": len(methods), "projected_methods": 0,
                    "value_cases": 30, "identity_cases": 140, "state_cases": 50, "failure_cases": 20,
                    "class_owned_return_and_field_signatures": "unchanged",
                    "complete_fixed_callback_parameters": "unchanged",
                    "mutable_sources_and_direct_parent_interface_reflection_dispatch": "matched",
                    "invented_bridges_and_closed_child_members": 0,
                    "complete_original_metadata_and_inventory": "unchanged except reported helpers",
                    "same_code_method_scope_and_parameter_counterexamples": "rejected",
                    "independent_javac_illegal_override_counterexamples": "rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged",
                    "coverage": coverage["status"]}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
