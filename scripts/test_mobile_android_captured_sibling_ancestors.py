#!/usr/bin/env python3
"""Verify captured sibling inheritance with complete original member contracts."""
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


def hashes(directory):
    return {p.relative_to(directory).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(directory.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-root", action="store_true")
    parser.add_argument("--inherited-default", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-captured-sibling-ancestors-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture_name = "captured_sibling_defaults" if args.inherited_default else "captured_sibling_ancestors"
        verify.compile_local(sorted((FIXTURES / fixture_name / "java").rglob("*.java")), originals)
        classes, methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        harness_sources = [FIXTURES / "harness/CapturedSiblingAncestorsHarness.java"]
        harness_name = "CapturedSiblingAncestorsHarness"
        if args.inherited_default:
            harness_sources.append(FIXTURES / "harness/CapturedSiblingDefaultsHarness.java")
            harness_name = "CapturedSiblingDefaultsHarness"
        verify.compile_local(harness_sources, harness, classpath=originals, release=11)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_name], "baseline")
        expected_output = ["values=70,identities=133,states=147,failures=35",
                           "inherited-slots=original,outer=mutable,method-formal=independent"]
        if args.inherited_default:
            expected_output += ["default-values=42,identities=63,states=56,failures=28",
                                "default=unique-original,callback=ordered,capture=shared"]
        if baseline.splitlines() != expected_output:
            raise RuntimeError("Independent captured sibling oracle missed an authored expected outcome: " + baseline)
        (work / "baseline.txt").write_text(baseline)
        dependencies = {"Lfixture/Cursor;", "Lfixture/Extended;"} if args.dependency_root else set()
        expected_classes = classes - dependencies
        expected_methods = {identity for identity in methods if identity.split("->", 1)[0] not in dependencies}
        sdk = work / "sdk"
        sdk.mkdir()
        sdk_jar = work / "contracts.jar"
        if dependencies:
            for name in ("Cursor", "Extended"):
                target = sdk / "fixture" / (name + ".class")
                target.parent.mkdir(exist_ok=True)
                shutil.copyfile(originals / "fixture" / (name + ".class"), target)
            verify.run([Path(verify.java).parent / "jar", "--create", "--file", sdk_jar, "-C", sdk, "."], "sdk-jar")
        sdk_hashes = hashes(sdk)
        jar_hash = hashlib.sha256(sdk_jar.read_bytes()).hexdigest() if dependencies else None
        classpath = [args.android_jar.resolve()] + ([sdk_jar] if dependencies else [])
        dex_dir = work / "dex"
        dex_dir.mkdir()
        input_files = [p for p in sorted(originals.rglob("*.class"))
                       if "L" + p.relative_to(originals).as_posix()[:-6] + ";" not in dependencies]
        d8_classpath = [part for p in classpath for part in ("--classpath", p)]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *d8_classpath,
                    "--output", dex_dir, *input_files], "d8")
        dex = dex_dir / "classes.dex"
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != expected_classes
                or {r["identity"] for r in inventory["methods"]} != expected_methods):
            raise RuntimeError("D8 changed the original input declaration inventory")
        (work / "inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        mobile_classpath = [part for p in classpath for part in ("--classpath", str(p))]
        verify.run([args.neverd.resolve(), "mobile", dex, *mobile_classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != expected_methods
                or coverage["class_count"] != len(expected_classes)
                or coverage["method_count"] != len(expected_methods)
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage.get("minimum_java_release") != 25):
            raise RuntimeError("Captured sibling inheritance lost original entries or projection disclosure")
        bindings = {r["class"]: r for r in coverage.get("class_source_bindings", [])}
        if set(bindings) != {"Lfixture/Owner$Base;", "Lfixture/Owner$Child;"}:
            raise RuntimeError("Missing exact captured sibling ownership disclosures")
        for owner, binding in bindings.items():
            if (binding["binding_kind"] != "inner-to-static-member"
                    or binding.get("outer_capture_field") != owner + "->this$0:Lfixture/Owner;"):
                raise RuntimeError("Capture receipt did not name the original outer field")
        changes = [change for row in coverage.get("generic_signature_projections", [])
                   for change in row["changes"]]
        if (len(changes) != 7 or
                sum(c["kind"] == "static-inner-owner-arguments" for c in changes) != 3 or
                sum(c["kind"] == "static-inner-type-variable" for c in changes) != 4 or
                any(c["source_type"] != "Ljava/lang/Object;"
                    or c["original_type"] != "Lfixture/Owner;::T"
                    for c in changes if c["kind"] == "static-inner-type-variable")):
            raise RuntimeError("Captured sibling projection exceeded the authored owner/Object scope")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=sdk if dependencies else None, release=25)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != expected_classes or set(actual_methods) != expected_methods | helpers:
            raise RuntimeError("Rebuilt inventory lost or invented an unreported inherited member")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in expected_classes:
            expected, actual = records[owner], actual_records[owner]
            expected_fields = json.loads(json.dumps(expected["fields"]))
            if owner in bindings:
                capture = bindings[owner]["outer_capture_field"]
                if (expected_fields[capture]["access"] != 0x1010 or
                        not any("synthetic capture-field" in text for text in bindings[owner]["metadata_differences"])):
                    raise RuntimeError("Missing original synthetic capture reflection disclosure")
                expected_fields[capture]["access"] &= ~0x1000
            if expected_fields != actual["fields"]:
                raise RuntimeError("Fields changed beyond the disclosed capture flag: " + owner)
            for key in ("access", "superclass", "interfaces", "enclosing_method"):
                if expected[key] != actual[key]:
                    raise RuntimeError("Original class metadata changed: " + owner + ":" + key)
            expected_signature = {
                "Lfixture/Owner$Base;": "Ljava/lang/Object;Lfixture/Cursor<Ljava/lang/Object;>;",
                "Lfixture/Owner$Child;": "Lfixture/Owner$Base;Lfixture/Extended<Ljava/lang/Object;>;",
            }.get(owner, expected["signature"])
            if actual["signature"] != expected_signature:
                raise RuntimeError("Class Signature changed beyond exact captured projection: " + owner)
            expected_inner = dict(expected["inner_class"]) if expected["inner_class"] else None
            if owner in bindings:
                expected_inner["access"] |= 8
            if actual["inner_class"] != expected_inner:
                raise RuntimeError("Lexical ownership changed beyond disclosed static flag")
            for identity, method in expected["methods"].items():
                expected_signature = method["signature"]
                if identity in {"Lfixture/Owner$Base;->next()Ljava/lang/Object;",
                                "Lfixture/Owner$Child;->accept(Ljava/lang/Object;)V",
                                "Lfixture/Owner;->base(I)Lfixture/Owner$Base;",
                                "Lfixture/Owner;->child(I)Lfixture/Owner$Child;"}:
                    expected_signature = None
                if actual["methods"][identity]["signature"] != expected_signature:
                    raise RuntimeError("Method Signature changed beyond disclosed scope: " + identity)
                for key in ("access", "code", "deprecated_attribute", "runtime_visible_annotations"):
                    if method[key] != actual["methods"][identity][key]:
                        raise RuntimeError("Original method metadata changed: " + identity + ":" + key)
        runtime_classpath = [rebuilt, harness] + ([sdk] if dependencies else [])
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime_classpath)),
                            harness_name], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Original inheritance, capture alias, effect or failure behavior changed")
        controls = [
            ("owner-bound", "fixture/Owner.class", b"<T:Ljava/lang/Object;>Ljava/lang/Object;",
             b"<T:Ljava/lang/Object;:Ljava/lang/CharSequence;>Ljava/lang/Object;"),
            ("owner-substitution", "fixture/Owner$Child.class",
             b"Lfixture/Owner<TT;>.Base;Lfixture/Extended<TT;>;",
             b"Lfixture/Owner<Ljava/lang/Object;>.Base;Lfixture/Extended<TT;>;"),
            ("method-result", "fixture/Owner$Base.class", b"<T:Ljava/lang/Object;>([TT;)[TT;",
             b"<T:Ljava/lang/Object;>([TT;)[Ljava/lang/Object;"),
        ]
        for name, file, before, after in controls:
            negative = work / (name + "-classes")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / file, before, after)
            (work / (name + "-metadata.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = work / (name + "-dex")
            negative_dex.mkdir()
            negative_files = [negative / p.relative_to(originals) for p in input_files]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *d8_classpath,
                        "--output", negative_dex, *negative_files], name + "-d8")
            rejected = work / (name + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex / "classes.dex"),
                                    *mobile_classpath, "-o", str(rejected)], text=True, capture_output=True,
                                   timeout=300, env=verify.environment)
            (work / (name + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Same-Code full-contract counterexample was silently accepted: " + name)
        bad_source = work / "javac-method-result"
        shutil.copytree(FIXTURES / fixture_name / "java", bad_source)
        owner_source = bad_source / "fixture/Owner.java"
        text = owner_source.read_text()
        before = "public <T> T[] echo(T[] values)"
        assert text.count(before) == 1
        owner_source.write_text(text.replace(before, "public <T> Object[] echo(T[] values)"))
        bad_output, empty = work / "javac-method-result-output", work / "javac-empty"
        empty.mkdir()
        result = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-implicit:none",
                                 "-sourcepath", str(empty), "-processorpath", str(empty),
                                 "-classpath", str(empty), "-d", str(bad_output),
                                 *map(str, sorted(bad_source.rglob("*.java")))],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "javac-method-result.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "return type" not in result.stderr:
            raise RuntimeError("Independent javac accepted the inherited method-formal result drift")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes or hashes(sdk) != sdk_hashes:
            raise RuntimeError("Original classes, dependency contracts or independent harness changed")
        if dependencies and hashlib.sha256(sdk_jar.read_bytes()).hexdigest() != jar_hash:
            raise RuntimeError("Dependency JAR changed")
        evidence = {"status": "success", "class_count": len(expected_classes), "method_count": len(expected_methods),
                    "dependency_profile": args.dependency_root, "inherited_default": args.inherited_default,
                    "jvm_output": baseline.splitlines(),
                    "same_code_counterexamples": [row[0] for row in controls],
                    "independent_javac_method_result": "rejected",
                    "original_class_sha256": original_hashes,
                    "runtime_inventory": "original entries and reported helpers only",
                    "captured_sibling_slots": "inherited unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
