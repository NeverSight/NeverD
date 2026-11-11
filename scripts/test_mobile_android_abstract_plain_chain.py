#!/usr/bin/env python3
"""Verify plain interface callbacks deferred through an abstract generic parent."""
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
    from scripts.test_mobile_android_final_getter_return_slots import rename_owned_entry
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_empty_original_bridge_throws import exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_final_getter_return_slots import rename_owned_entry
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_empty_original_bridge_throws import exception_inventory


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
    parser.add_argument("--redeclare", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-abstract-plain-chain-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "abstract_plain_chain/java"
        if args.redeclare:
            authored = work / "redeclared-java"
            shutil.copytree(fixture, authored)
            target = authored / "fixture/Middle.java"
            target.write_text(target.read_text().replace("public Middle() {}",
                "public Middle() {} public abstract void removed(int first, int second);"))
            fixture = authored
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        original_hashes = hashes(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        original_exceptions = exception_inventory(originals)
        declarations = {"Lfixture/Updates;"} if args.dependency_interface else set()
        classes = all_classes - declarations
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        dependency = work / "owned-updates.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("fixture/Updates.class", (originals / "fixture/Updates.class").read_bytes())
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
                raise RuntimeError("Owned deferred callback fixture did not produce one DEX")
            return result

        verify.compile_local([FIXTURES / "harness/AbstractPlainChainHarness.java"],
                             harness, classpath=originals)
        harness_hashes = hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "AbstractPlainChainHarness", *(["redeclare"] if args.redeclare else [])], "baseline")
        expected = ["values=20,identities=462,states=147,failures=42",
                    "deferred-declarations=unchanged,child-dispatch-and-parent-forwarding=matched,generic-scopes=retained"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned deferred callback baseline missed a dispatch or effect case")
        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed the original deferred callback inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage.get("projected_method_count", 0) != 0 or coverage["unrecovered_method_count"] != 0
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Callback recovery invented methods or changed original contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_interface else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helper_rows = coverage.get("generated_source_helpers", [])
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in helper_rows}
        expected_helpers = {"Lfixture/" + owner + ";->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in ("Parent", "Middle", "Child")}
        if (helpers != expected_helpers or len(helper_rows) != len(helpers)
                or any(row["kind"] != "throw-helper" or not row["static"] for row in helper_rows)):
            raise RuntimeError("Deferred callback source helper inventory was not reported exactly")
        if actual_classes != classes or set(actual_methods) != methods | helpers:
            raise RuntimeError("Recompile changed original ownership or invented deferred methods")
        actual_records = class_identity.compiler_classes(rebuilt)
        actual_exceptions = exception_inventory(rebuilt)
        if any(actual_exceptions[m] != original_exceptions[m] for m in methods):
            raise RuntimeError("Original deferred Exceptions attributes changed")
        for owner in classes:
            wanted, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != wanted.get(key):
                    raise RuntimeError("Original deferred class metadata changed: " + owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original deferred method metadata changed: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "AbstractPlainChainHarness", *(["redeclare"] if args.redeclare else [])], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Deferred callbacks changed dispatch, state, identities or ordered failures")

        for label in ("missing-child-provider", "incompatible-parent-scope"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            target = negative / "fixture/Child.class"
            if label == "missing-child-provider":
                receipt = rename_owned_entry(target, b"removed", b"missingRemoved")
            else:
                receipt = change_signature(target,
                    b"<S:Ljava/lang/Object;V:Ljava/lang/Object;>Lfixture/Middle<TS;TV;>;",
                    b"<S:Ljava/lang/Object;V:Ljava/lang/Object;>Lfixture/Middle<TV;TS;>;")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Deferred callback validation accepted a missing or incompatible provider")
            negative_java = work / (label + "-java")
            compiler_inputs = []
            for source in inputs:
                content = source.read_text()
                if source.name == "Child.java":
                    before, after = (("void removed(", "void missingRemoved(")
                                     if label == "missing-child-provider" else
                                     ("extends Middle<S, V>", "extends Middle<V, S>"))
                    if content.count(before) != 1:
                        raise RuntimeError("Missing exact owned callback compiler counterexample")
                    content = content.replace(before, after, 1)
                destination = negative_java / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                compiler_inputs.append(str(destination))
            compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                       str(work / (label + "-compiler")), *compiler_inputs], text=True,
                                      capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-compiler.log")).write_text(compiler.stdout + compiler.stderr)
            if compiler.returncode == 0 or "does not override" not in compiler.stderr:
                raise RuntimeError("Independent javac accepted a missing or incompatible callback provider")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original classes, immutable harness or declaration bytes changed")
        evidence = {"status": "success", "dependency_interface": args.dependency_interface,
                    "class_count": len(classes), "method_count": len(methods), "projected_methods": 0,
                    "value_cases": 20, "identity_cases": 462, "state_cases": 147, "failure_cases": 42, "redeclare": args.redeclare,
                    "deferred_parent_declarations": "not invented",
                    "child_parent_and_interface_dispatch": "matched",
                    "original_parent_forwarding_and_exception_order": "matched",
                    "complete_original_metadata_and_inventory": "unchanged except reported helpers",
                    "same_code_missing_and_incompatible_provider_counterexamples": "rejected",
                    "independent_javac_counterexamples": "rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged",
                    "coverage": coverage["status"]}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
