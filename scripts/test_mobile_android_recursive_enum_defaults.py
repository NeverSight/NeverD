#!/usr/bin/env python3
"""Verify complete recursive Enum method bounds and inherited default ownership."""
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


def hashes(directory: Path) -> dict[str, str]:
    return {p.relative_to(directory).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(directory.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-interface", action="store_true")
    parser.add_argument("--nested-ancestor", action="store_true")
    parser.add_argument("--dependency-ancestor", action="store_true")
    parser.add_argument("--ancestor-chain", action="store_true")
    parser.add_argument("--ancestor-throws", action="store_true")
    args = parser.parse_args()
    if args.ancestor_throws and not args.ancestor_chain:
        parser.error("--ancestor-throws requires --ancestor-chain")
    if args.ancestor_chain and not args.nested_ancestor:
        parser.error("--ancestor-chain requires --nested-ancestor")
    if args.dependency_ancestor and not args.nested_ancestor:
        parser.error("--dependency-ancestor requires --nested-ancestor")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-recursive-enum-default-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "recursive_enum_default/java"
        if args.nested_ancestor:
            owned = work / "nested-java"
            shutil.copytree(fixture, owned)
            shutil.copyfile(FIXTURES / "nested_default_ancestor/java/fixture/Owner.java",
                            owned / "fixture/Owner.java")
            reader = owned / "fixture/Reader.java"
            declaration = "public class Reader<T> implements Refined<T>"
            content = reader.read_text()
            if content.count(declaration) != 1:
                raise RuntimeError("Missing owned nested-ancestor receiver declaration")
            reader.write_text(content.replace(declaration,
                "public class Reader<T> extends Owner.State implements Refined<T>", 1))
            if args.ancestor_chain:
                chain = FIXTURES / "plain_default_class_chain/java/fixture"
                for source in sorted(chain.glob("*.java")):
                    shutil.copyfile(source, owned / "fixture" / source.name)
                ancestor = owned / "fixture/Owner.java"
                content = ancestor.read_text()
                declaration = "public static class State {"
                if content.count(declaration) != 1:
                    raise RuntimeError("Missing owned plain chain ancestor declaration")
                ancestor.write_text(content.replace(declaration,
                    "public static class State extends Root {", 1))
            if args.ancestor_throws:
                for name in ("RootBase.java", "StateView.java"):
                    source = owned / "fixture" / name
                    content = source.read_text()
                    declaration = "int rootState()"
                    if content.count(declaration) != 1:
                        raise RuntimeError("Missing owned ancestor exception declaration")
                    source.write_text(content.replace(declaration,
                        "int rootState() throws java.io.IOException", 1))
                reader = owned / "fixture/Reader.java"
                content = reader.read_text()
                suffix = "\n}\n"
                if not content.endswith(suffix):
                    raise RuntimeError("Missing owned checked ancestor override anchor")
                reader.write_text(content[:-len(suffix)] + "\n    public java.io.IOException rootFailure;\n"
                    "\n    public int rootState() throws java.io.IOException {\n"
                    "        if (rootFailure != null) throw rootFailure;\n"
                    "        return rootCalls;\n    }\n}\n")
            fixture = owned
        originals, harness_dir = work / "originals", work / "harness"
        inputs = sorted(fixture.rglob("*.java"))
        verify.compile_local(inputs, originals)
        original_hashes = hashes(originals)
        records = class_identity.compiler_classes(originals)
        classes, methods = compiler_inventory(verify, originals)
        dependency = work / "owned-default.jar"
        jar_entries = ["fixture/Api.class"]
        if args.dependency_ancestor:
            jar_entries += ["fixture/Owner.class", "fixture/Owner$State.class"]
            if args.ancestor_chain:
                jar_entries += ["fixture/Root.class", "fixture/RootBase.class",
                                "fixture/StateView.class", "fixture/Pair.class"]
        with zipfile.ZipFile(dependency, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for entry in jar_entries:
                archive.write(originals / entry, entry)
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        if args.dependency_interface:
            classes -= {"Lfixture/Api;"}
        if args.dependency_ancestor:
            classes -= {"Lfixture/Owner;", "Lfixture/Owner$State;"}
            if args.ancestor_chain:
                classes -= {"Lfixture/Root;", "Lfixture/RootBase;",
                            "Lfixture/StateView;", "Lfixture/Pair;"}
        methods = {key: row for key, row in methods.items() if key.split("->", 1)[0] in classes}
        classpath = ["--classpath", dependency] if (args.dependency_interface or args.dependency_ancestor) else []
        harness = "NestedDefaultAncestorHarness" if args.nested_ancestor else "RecursiveEnumDefaultHarness"
        if args.ancestor_chain:
            harness = "PlainClassChainDefaultHarness"
        if args.ancestor_throws:
            harness = "PlainClassChainThrowsHarness"
        harness_inputs = [FIXTURES / "harness/RecursiveEnumDefaultHarness.java"]
        if args.nested_ancestor:
            harness_inputs.append(FIXTURES / "harness/NestedDefaultAncestorHarness.java")
        if args.ancestor_chain:
            harness_inputs.append(FIXTURES / "harness/PlainClassChainDefaultHarness.java")
        if args.ancestor_throws:
            harness_inputs.append(FIXTURES / "harness/PlainClassChainThrowsHarness.java")
        verify.compile_local(harness_inputs, harness_dir, classpath=originals)
        harness_hashes = hashes(harness_dir)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               harness], "baseline")
        expected = ["values=24,identities=375,states=125,failures=10,bridges=0",
                    "recursive-enum-scopes=retained,original-default-ownership-and-effects=matched"]
        if args.nested_ancestor:
            expected.append("named-static-ancestor-scope=independent,initialization-aliases-and-virtual-effects=matched")
        if args.ancestor_chain:
            expected.append("plain-class-chain=complete,original-generic-members-and-virtual-effects=matched")
        if args.ancestor_throws:
            expected.append("fixed-ancestor-throws=retained,checked-failure-identities-and-reflection=matched")
        if baseline.splitlines() != expected:
            raise RuntimeError("Recursive Enum baseline missed expected identities, effects or failures")

        def make_dex(directory: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath",
                        args.android_jar.resolve(), *classpath, "--output", destination,
                        *[p for p in sorted(directory.rglob("*.class"))
                          if "L" + p.relative_to(directory).as_posix()[:-6] + ";" in classes]],
                       "d8-" + destination.name)
            dex = destination / "classes.dex"
            if not dex.is_file() or list(destination.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Recursive Enum fixture did not retain one original DEX")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed original recursive Enum fixture ownership")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage.get("projected_method_count", 0) != 0 or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "recovered" or coverage.get("generic_signature_projections")):
            raise RuntimeError("Recursive Enum recovery copied a method or projected generic metadata")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if classpath else None,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("Recompile changed original recursive Enum runtime entries")
        after = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                if records[owner].get(key) != after[owner].get(key):
                    raise RuntimeError("Recursive Enum class/field metadata changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                if after[owner]["methods"][identity] != metadata:
                    raise RuntimeError("Complete recursive Enum method metadata changed: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir, dependency))),
                             harness], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Recursive Enum defaults changed reference identity, state or failure order")
        cases = [
            ("member-scope", "Reader", b"(Ljava/lang/Class<*>;TT;)V", b"(Ljava/lang/Class<TT;>;TT;)V",
             "Reader.java", "touch(Class<?> type, T key)", "touch(Class<T> type, T key)"),
            ("parent-scope", "Refined", b"<S:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Api<TS;>;",
             b"<S:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Api<Ljava/lang/Enum<*>;>;",
             "Refined.java", "extends Api<S>", "extends Api<Enum<?>>"),
        ]
        if args.ancestor_chain:
            cases.append(("ancestor-arity", "RootBase",
                b"<R:Ljava/lang/Object;>(Lfixture/Pair<TR;>;TR;)TR;",
                b"<R:Ljava/lang/Object;>(Lfixture/Pair<TR;TR;>;TR;)TR;",
                "RootBase.java", "pair(Pair<R> box, R fallback)",
                "pair(Pair<R, R> box, R fallback)"))
        for name, owner, before, changed, source_name, declaration, replacement in cases:
            negative = work / (name + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / ("fixture/" + owner + ".class"), before, changed)
            (work / (name + "-same-code.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = make_dex(negative, work / (name + "-dex"))
            negative_classpath = classpath
            if args.dependency_ancestor and owner == "RootBase":
                changed_dependency = work / (name + "-dependency.jar")
                with zipfile.ZipFile(dependency) as original, zipfile.ZipFile(
                        changed_dependency, "w", compression=zipfile.ZIP_DEFLATED) as altered:
                    for entry in original.namelist():
                        data = (negative / entry).read_bytes() if entry == "fixture/RootBase.class" else original.read(entry)
                        altered.writestr(entry, data)
                negative_classpath = ["--classpath", changed_dependency]
            rejected = work / (name + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                     str(args.android_jar.resolve()), *map(str, negative_classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (name + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Equal erased descriptors hid different complete Enum scopes: " + name)
            compiler_inputs = []
            for source in inputs:
                content = source.read_text()
                if source.name == source_name:
                    if content.count(declaration) != 1:
                        raise RuntimeError("Missing independent Enum counterexample declaration")
                    content = content.replace(declaration, replacement, 1)
                destination = work / (name + "-java") / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                compiler_inputs.append(str(destination))
            compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                       str(work / (name + "-compiler")), *compiler_inputs],
                                      text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (name + "-compiler.log")).write_text(compiler.stdout + compiler.stderr)
            diagnostic = "wrong number of type arguments" if name == "ancestor-arity" else "does not override"
            if compiler.returncode == 0 or diagnostic not in compiler.stderr:
                raise RuntimeError("Independent javac accepted different Enum scope: " + name)
        if (hashes(originals) != original_hashes or hashes(harness_dir) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original Enum input, immutable JVM harness or dependency changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 24, "identity_cases": 375, "state_observations": 125,
                    "ordered_failure_cases": 10, "bridge_count": 0, "coverage": "recovered",
                    "complete_recursive_enum_bound": "unchanged", "generic_metadata": "unchanged",
                    "original_default_ownership": "retained", "jvm_output": "matched",
                    "scope_counterexamples": "same Code and descriptors; rejected by NeverD and javac",
                    "declaration_profile": "unchanged JAR" if args.dependency_interface else "input interface"}
        if args.nested_ancestor:
            evidence["static_plain_ancestor"] = {
                "scope": "independent of generic enclosing owner",
                "profile": "unchanged JAR" if args.dependency_ancestor else "input nested declaration",
                "initialization_aliases_virtual_calls_and_failure_order": "matched"}
        if args.ancestor_chain:
            evidence["plain_class_chain"] = {
                "class_edges": 4, "generic_ancestor_members": "original ownership and complete scopes retained",
                "initialization_aliases_virtual_calls_and_failure_order": "matched",
                "arity_counterexample": "same Code and descriptors; source and dependency scopes rejected"}
        if args.ancestor_throws:
            evidence["fixed_ancestor_throws"] = "same validated set; original declarations, reflection and checked failures retained"
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
