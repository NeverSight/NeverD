#!/usr/bin/env python3
"""Verify original abstract bounded parameters with disclosed absent compiler slots."""
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
    from scripts.test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-root", action="store_true")
    parser.add_argument("--concrete-parent", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-abstract-bounded-parameters-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "abstract_bounded_parameters/java"
        if args.concrete_parent:
            authored = work / "concrete-parent-java"
            shutil.copytree(fixture, authored)
            shutil.copyfile(FIXTURES / "abstract_bounded_parameters/concrete/Parent.java", authored / "fixture/Parent.java")
            fixture = authored
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        callers = [FIXTURES / "harness/AbstractBoundedParametersHarness.java"]
        if args.concrete_parent:
            callers.append(FIXTURES / "harness/AbstractBoundedParentCaller.java")
        verify.compile_local(callers, harness, classpath=originals)
        edits = [omit_owned_interface_bridge(originals / "fixture/Middle.class",
                    b"read(Ljava/lang/String;ILjava/lang/Object;JZ)V", original_flags=0x1044)]
        (work / "owned-abstract-slots.json").write_text(json.dumps(edits, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        declarations = {"Lfixture/Parent;", "Lfixture/Root;"}
        classes = all_classes - (declarations if args.dependency_root else set())
        methods = {i for i in all_methods if i.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "original-declarations.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Parent", "Root"):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_root:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "AbstractBoundedParametersHarness", "original"], "baseline")
        if baseline.splitlines() != ["values=864,identities=2808,states=952,failures=100",
                "abstract-bounded-declarations-and-concrete-dispatch=retained,compiler-slot=reported"]:
            raise RuntimeError("Independent abstract bounded parameter oracle missed original outcomes: " + baseline)

        if args.concrete_parent:
            parent_baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                                         "AbstractBoundedParentCaller"], "parent-baseline")
            if parent_baseline.splitlines() != ["values=144,identities=300,states=148,failures=6"]:
                raise RuntimeError("Independent concrete parent oracle missed original outcomes: " + parent_baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [p for p in sorted(root.rglob("*.class")) if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned abstract bounded parameter fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original abstract declarations")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count", 0) != 3
                or coverage["status"] != "partial" or coverage.get("generic_signature_projections")
                or coverage.get("class_source_bindings")):
            raise RuntimeError("Abstract declarations changed inventory or complete scopes")
        projected = {i for i, row in rows.items() if row.get("projection_kind") == "class-parameter-runtime-bridges"}
        expected = {"Lfixture/Middle;->read(Ljava/lang/String;ILfixture/Container;JZ)V"}
        if projected != expected or any(not rows[i].get("generated_bridges") or not rows[i].get("metadata_differences") for i in expected):
            raise RuntimeError("Absent abstract class compiler slots were not disclosed")
        compiler_bridges = [r for r in coverage.get("generated_source_helpers", []) if r["kind"] == "compiler-parameter-class-bridge"]
        if len(compiler_bridges) != 1:
            raise RuntimeError("Unexpected abstract class compiler bridge count")
        for row in compiler_bridges:
            if (row["class"] != "Lfixture/Middle;" or row["static"]
                    or row["access_flags"] != ["protected", "bridge", "synthetic"]):
                raise RuntimeError("Compiler bridge visibility or ownership changed")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_root else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != methods | helpers:
            raise RuntimeError("Abstract bounded parameters changed runtime inventory beyond reported helpers")
        after = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after[owner].get(key) != records[owner].get(key):
                    raise RuntimeError("Original class/field scope changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                if after[owner]["methods"][identity] != metadata:
                    raise RuntimeError("Original declaration metadata changed: " + identity)
        actual_exceptions = exception_inventory(rebuilt)
        if any(actual_exceptions[i] != exceptions[i] for i in methods):
            raise RuntimeError("Original exception declarations changed")
        runtime = (rebuilt, harness, dependency) if args.dependency_root else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), "AbstractBoundedParametersHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Abstract class values, virtual dispatch or failures changed")
        if args.concrete_parent:
            parent_actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                       "AbstractBoundedParentCaller"], "parent-rebuilt")
            if parent_actual != parent_baseline:
                raise RuntimeError("Inherited concrete parent body or exception identity changed")
        for label in ("class-formal-drift", "method-formal-drift", "missing-synthetic"):
            negative = work / label
            shutil.copytree(originals, negative)
            middle = negative / "fixture/Middle.class"
            original_signature = b"(Ljava/lang/String;ITF;JZ)V"
            if label == "class-formal-drift":
                receipt = change_signature(middle, original_signature,
                    b"(Ljava/lang/String;ILfixture/Container<TE;>;JZ)V")
            elif label == "method-formal-drift":
                receipt = change_signature(middle, original_signature,
                    b"<F:Lfixture/Container<TE;>;>(Ljava/lang/String;ITF;JZ)V")
            else:
                receipt = change_method_flags(negative / "fixture/Core.class",
                    b"(Ljava/lang/String;ILjava/lang/Object;JZ)V", 0x1041, 0x41, name=b"read")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Abstract compiler projection hid incompatible original declarations: " + label)
        authored = work / "invalid-java"
        shutil.copytree(fixture, authored)
        source = authored / "fixture/Middle.java"
        content = source.read_text()
        assert content.count("int index, F box") == 1
        source.write_text(content.replace("int index, F box", "int index, Container<E> box"))
        rejected_classes = work / "invalid-classes"
        rejected_classes.mkdir()
        result = subprocess.run([str(verify.javac), "-d", str(rejected_classes), *map(str, sorted(authored.rglob("*.java")))],
                                capture_output=True, text=True, timeout=300, env=verify.environment)
        (work / "invalid-javac.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0:
            raise RuntimeError("Independent javac accepted a changed class-formal contract")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original fixture, harness, declarations or SDK changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "values": 864, "identities": 2808, "states": 952, "failures": 100,
                    "projected_methods": 3, "generated_class_bridges": 1, "dependency_root": args.dependency_root,
                    "source_coverage": coverage["status"], "concrete_parent": args.concrete_parent,
                    "parent_body_checks": {"values": 144, "identities": 300, "states": 148, "failures": 6} if args.concrete_parent else None,
                    "original_complete_metadata_and_exceptions": "unchanged",
                    "abstract_runtime_slots": "absent in immutable input; compiler additions disclosed",
                    "concrete_original_bridges": "two original bodies required and retained",
                    "same_code_counterexamples": "rejected", "original_input_harness_dependency_and_sdk": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
