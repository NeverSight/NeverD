#!/usr/bin/env python3
"""Verify original source-class bridges across three generic return erasures."""
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
    parser.add_argument("--dependency-root", action="store_true")
    parser.add_argument("--inherited-members", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-source-class-bridge-chain-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "source_class_bridge_chain/java"
        inputs = sorted(fixture.rglob("*.java"))
        if args.inherited_members:
            source_root = work / "owned-source"
            for source in inputs:
                content = source.read_text()
                if source.name == "Root.java":
                    declaration = "    public abstract T parse(String text);\n"
                    if content.count(declaration) != 1:
                        raise RuntimeError("Missing original generic root declaration")
                    content = content.replace(declaration, declaration +
                        "    public abstract T read(String text, int index);\n"
                        "    public abstract void write(String text, int index, T value);\n", 1)
                if source.name == "Parent.java":
                    content = content.rsplit("}", 1)[0] + '''    public int readCalls, writeCalls, lastIndex;
    public String lastText;
    public RuntimeException inheritedFailure;
    public P read(String text, int index) {
        readCalls++; lastText = text; lastIndex = index;
        if (inheritedFailure != null) throw inheritedFailure;
        return value;
    }
    public void write(String text, int index, P next) {
        writeCalls++; lastText = text; lastIndex = index;
        if (inheritedFailure != null) throw inheritedFailure;
        value = next;
    }
}
'''
                destination = source_root / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
            fixture = source_root
            inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        original_hashes = hashes(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        declarations = {"Lfixture/Choice;"}
        if args.dependency_root:
            declarations.add("Lfixture/Root;")
        classes = all_classes - declarations
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        dependency = work / "owned-root.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for owner in sorted(declarations):
                relative = owner[1:-1] + ".class"
                archive.writestr(relative, (originals / relative).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve(), "--classpath", dependency]

        def dex(root: Path, output: Path) -> Path:
            paths = [p for p in sorted(root.rglob("*.class"))
                     if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", output, *paths], "d8-" + output.name)
            result = output / "classes.dex"
            if not result.is_file() or list(output.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned source bridge chain did not produce one DEX")
            return result

        harness_name = "InheritedTypedProviderHarness" if args.inherited_members else "SourceClassBridgeChainHarness"
        harness_sources = [FIXTURES / "harness/SourceClassBridgeChainHarness.java"]
        if args.inherited_members:
            harness_sources.append(FIXTURES / ("harness/" + harness_name + ".java"))
        verify.compile_local(harness_sources, harness, classpath=originals)
        harness_hashes = hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_name], "baseline")
        expected = ["identities=56,states=28,failures=13,bridges=3",
                    "parent-and-child-dispatch=matched,three-original-return-entries=retained,generic-scopes=retained"]
        if args.inherited_members:
            expected += ["inherited-identities=221,inherited-states=224,inherited-failures=103,parent-bridges=2",
                         "parent-provider=retained,child-members=not-invented,bridge-casts-and-ordered-effects=matched"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned baseline missed a typed, parent or root dispatch case")
        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed the original bridge-chain inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        bridge_count = 5 if args.inherited_members else 3
        if ({row["identity"] for row in coverage["methods"]} != methods
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["projected_method_count"] != bridge_count or coverage["unrecovered_method_count"] != 0
                or coverage.get("generic_signature_projections") or coverage["status"] != "partial"):
            raise RuntimeError("Bridge-chain recovery changed an original descriptor or generic scope")
        bridges = [row for row in coverage["methods"]
                   if row.get("projection_kind") == "generic-superclass-bridge"]
        if len(bridges) != bridge_count or any(row["bridge_target"] not in methods
                or row["bridge_dispatch"] not in all_methods for row in bridges):
            raise RuntimeError("Source parent or child bridge lacks its own original witness")
        if {row["projection_kind"] for row in coverage["methods"] if row["status"] == "source-projected"} != {
                "generic-superclass-bridge"}:
            raise RuntimeError("Bridge-chain recovery added an unrelated source projection")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency)
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
                    raise RuntimeError("Original bridge-chain class metadata changed: " + owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original bridge-chain method metadata changed: " + identity)
        runtime_path = (rebuilt, harness, dependency)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime_path)),
                             harness_name], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original parent/child dispatch, mutable state or exception identity changed")

        negatives = ["child-result-scope", "parent-result-scope", "child-parent-scope"]
        if args.inherited_members:
            negatives += ["inherited-result-scope", "inherited-parameter-scope"]
        for label in negatives:
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "child-result-scope":
                receipt = change_signature(negative / "fixture/Child.class",
                                           b"(Ljava/lang/String;)TX;", b"(Ljava/lang/String;)TY;")
            elif label == "parent-result-scope":
                receipt = change_signature(negative / "fixture/Parent.class",
                    b"(Ljava/lang/String;)TP;", b"(Ljava/lang/String;)Ljava/io/Serializable;")
            elif label == "child-parent-scope":
                receipt = change_signature(negative / "fixture/Child.class",
                    b"<X:Ljava/lang/Enum<TX;>;Y:Ljava/lang/Enum<TY;>;>Lfixture/Parent<TX;>;",
                    b"<X:Ljava/lang/Enum<TX;>;Y:Ljava/lang/Enum<TY;>;>Lfixture/Parent<TY;>;")
            elif label == "inherited-result-scope":
                receipt = change_signature(negative / "fixture/Parent.class",
                    b"(Ljava/lang/String;I)TP;", b"(Ljava/lang/String;I)Ljava/io/Serializable;")
            else:
                receipt = change_signature(negative / "fixture/Parent.class",
                    b"(Ljava/lang/String;ITP;)V", b"(Ljava/lang/String;ILjava/io/Serializable;)V")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Source bridge deferral bypassed a different full generic scope")
        negative_java = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "Child.java":
                declaration = "extends Parent<X>"
                if content.count(declaration) != 1:
                    raise RuntimeError("Missing original child-to-parent source edge")
                content = content.replace(declaration, "extends Parent<Y>", 1)
            destination = negative_java / source.relative_to(fixture)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or "parse(" not in compiler.stderr or "compatible" not in compiler.stderr:
            raise RuntimeError("Independent javac did not reject a competing same-erasure formal")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original classes, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "dependency_root": args.dependency_root,
                    "inherited_members": args.inherited_members,
                    "class_count": len(classes), "method_count": len(methods), "bridge_count": bridge_count,
                    "identity_cases": 56, "state_cases": 28, "failure_cases": 13,
                    "three_return_erasures_parent_and_child_dispatch": "matched",
                    "complete_metadata_and_inventory": "unchanged except reported helpers",
                    "same_erasure_formal_counterexamples": "rejected with original Code",
                    "independent_javac_scope_counterexample": "rejected",
                    "original_input_harness_and_sdk_bytes": "unchanged", "coverage": "partial"}
        if args.inherited_members:
            evidence.update(inherited_identity_cases=221, inherited_state_cases=224,
                            inherited_failure_cases=103, original_parent_provider="retained",
                            child_inherited_members="not invented", original_bridge_casts="matched",
                            serializable_heap_pollution_and_typed_read_cast="matched")
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
