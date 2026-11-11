#!/usr/bin/env python3
"""Verify source virtual naming families rooted at a concrete superclass method."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_synthetic_method_names import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_synthetic_method_names import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_protected_array_callbacks import change_method_flags


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "kotlin-lib"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-base", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-concrete-virtual-names-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "concrete_virtual_names"
        kotlin = args.kotlin_lib.resolve()
        compiler_jars = sorted(kotlin.glob("*.jar"))
        if not (kotlin / "kotlin-compiler.jar").is_file():
            raise RuntimeError("Original witness requires the unmodified Kotlin compiler distribution")
        inputs = [args.android_jar.resolve(), *compiler_jars]
        input_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        stdlib = kotlin / "kotlin-stdlib.jar"
        dependencies = []
        for jar in (stdlib, kotlin / "annotations-13.0.jar"):
            output = work / jar.name
            with zipfile.ZipFile(jar) as source, zipfile.ZipFile(output, "x") as dest:
                for entry in source.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):
                            raise RuntimeError("Unsafe compiler dependency entry")
                        dest.writestr(entry.filename, source.read(entry))
            dependencies.append(output)
        java_originals, originals, harness = work / "original-java", work / "originals", work / "harness"
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), java_originals, release=8)
        originals.mkdir()
        verify.run([verify.java, "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (stdlib, java_originals))),
                    "-jvm-target", "1.8", "-d", originals, *sorted((fixture / "kotlin").rglob("*.kt"))], "kotlinc-original")
        for path in sorted(java_originals.rglob("*.class")):
            target = originals / path.relative_to(java_originals)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("ConcreteVirtualNamesCaller", "ConcreteVirtualNamesHarness")], harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 6 or len(all_methods) != 15:
            raise RuntimeError("Original concrete family inventory changed: " + repr((len(all_classes), len(all_methods))))
        excluded = {"Lfixture/InitBase;"} if args.dependency_base else set()
        classes = all_classes - excluded
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        dependency = work / "owned-dependency.jar"
        with zipfile.ZipFile(dependency, "x") as archive:
            for owner in sorted(excluded):
                name = owner[1:-1] + ".class"
                archive.writestr(name, (originals / name).read_bytes())
        if excluded:
            dependencies.append(dependency)
        dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        classpath = ["--classpath", args.android_jar.resolve()]
        for path in dependencies:
            classpath += ["--classpath", path]
        family = {"Lfixture/" + owner + ";->read-value(Ljava/lang/Object;)Ljava/lang/Object;"
                  for owner in ("Root", "Child", "Leaf")}
        if any(records[identity.split("->", 1)[0]]["methods"][identity]["access"] != 1 for identity in family):
            raise RuntimeError("Compiler changed an original public nonfinal virtual entry")

        def execute(runtime, spelling, label):
            outputs = []
            for route in range(4):
                for receiver in range(3):
                    for mode in range(6):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "ConcreteVirtualNamesHarness", str(route), str(receiver), str(mode), spelling],
                            f"{label}-{route}-{receiver}-{mode}")
                        if output != "attempts=2,virtual-family-values-identities-effects-failures=matched\n":
                            raise RuntimeError("Frozen concrete family observer missed its cases: " + output)
                        outputs.append(output)
            return outputs
        baseline = execute((originals, harness, *dependencies), "read-value", "baseline")

        def make_dex(selected, directory):
            directory.mkdir()
            paths = [p for p in sorted(originals.rglob("*.class"))
                     if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in selected]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", directory, *paths], "d8-" + directory.name)
            dex = directory / "classes.dex"
            if list(directory.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned virtual family changed its DEX inventory")
            return dex
        dex = make_dex(classes, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original virtual declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        spellings = {rows[identity].get("source_name") for identity in family}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count") != 3
                or coverage["status"] != "partial" or len(spellings) != 1 or None in spellings):
            raise RuntimeError("Concrete family changed original inventory or shared naming")
        spelling = next(iter(spellings))
        root = "Lfixture/Root;->read-value(Ljava/lang/Object;)Ljava/lang/Object;"
        for identity in family:
            if (rows[identity].get("source_name_declaration") != root
                    or set(rows[identity].get("source_name_family", [])) != family
                    or rows[identity].get("projection_kind") != "virtual-method-identifier"):
                raise RuntimeError("Concrete family lost its original root receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=os.pathsep.join(map(str, dependencies)), release=8)
        rename = {identity: identity.replace("->read-value(", "->" + spelling + "(") for identity in family}
        expected_methods = {rename.get(identity, identity) for identity in methods}
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != expected_methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Concrete family invented or omitted a runtime entry")
        after_records, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        drops = []
        for owner in classes:
            before, after = records[owner], after_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original concrete family class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                wanted = copy.deepcopy(metadata)
                if identity in family:
                    wanted["name"] = spelling
                for key in ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations"):
                    if wanted.get(key) and any(wanted[key]):
                        annotations = wanted[key] if key == "runtime_invisible_annotations" else [a for site in wanted[key] for a in site]
                        if any(a.get("type") not in {"Lkotlin/jvm/JvmName;", "Lorg/jetbrains/annotations/Nullable;"} for a in annotations):
                            raise RuntimeError("Unexpected original compiler CLASS annotation")
                        drops.append({"identity": identity, "attribute": key, "original": wanted[key]})
                        wanted[key] = []
                selected = rename.get(identity, identity)
                if after["methods"].get(selected) != wanted or after_exceptions[selected] != exceptions[identity]:
                    raise RuntimeError("Concrete family metadata changed beyond disclosed names and D8 CLASS loss: " + identity)
        expected_sites = family | {
            "Lfixture/Root;->entry(Ljava/lang/Object;)Ljava/lang/Object;",
            "Lfixture/Child;->parent(Ljava/lang/Object;)Ljava/lang/Object;"}
        if {(d["identity"], d["attribute"]) for d in drops} != {
                (identity, key) for identity in expected_sites for key in
                ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations")}:
            raise RuntimeError("Original compiler CLASS annotation loss changed its exact sites")
        (work / "d8-class-annotation-loss.json").write_text(json.dumps(drops, indent=2) + "\n")
        if execute((rebuilt, harness, *dependencies), spelling, "rebuilt") != baseline:
            raise RuntimeError("Concrete family values, dispatch, identity, order or failures changed")
        frozen_root = work / "frozen-root.jar"
        with zipfile.ZipFile(frozen_root, "x") as archive:
            archive.writestr("fixture/Root.class", (originals / "fixture/Root.class").read_bytes())
        frozen_hash = hashlib.sha256(frozen_root.read_bytes()).hexdigest()
        negative_classes = classes - {"Lfixture/Root;"}
        negative_dex = make_dex(negative_classes, work / "sdk-root-dex")
        rejected = work / "sdk-root-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                 *map(str, classpath), "--classpath", str(frozen_root), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "sdk-root-rejection.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "identifier cannot be represented in Java" not in result.stderr:
            raise RuntimeError("Unchanged SDK root obtained a source virtual name or failed for an unrelated reason")
        for label, old, new in (("wrong-effect", "values[0] += seed;", "values[0] += 1;"),
                                ("wrong-result", "Object result = value;", "Object result = new Object();"),
                                ("missing-callback", "hit(3);", "")):
            source = work / label / "Trace.java"
            source.parent.mkdir()
            text = (fixture / "java/fixture/Trace.java").read_text()
            if text.count(old) != 1:
                raise RuntimeError("Missing exact authored virtual family mutant site")
            source.write_text(text.replace(old, new))
            patched = work / (label + "-classes")
            verify.compile_local([source], patched, release=8)
            result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (patched, originals, harness, *dependencies))),
                                     "ConcreteVirtualNamesHarness", "3", "2", "0", "read-value"],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned concrete virtual family contract" not in result.stderr:
                raise RuntimeError("Frozen virtual family oracle accepted " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies} != dependency_hashes
                or {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs} != input_hashes
                or hashlib.sha256(frozen_root.read_bytes()).hexdigest() != frozen_hash):
            raise RuntimeError("Original compiler, frozen caller or dependency bytes changed")
        evidence = {"status": "success", "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "class_count": len(classes), "method_count": len(methods), "compiled_behavior_mutants": 3,
                    "concrete_root_and_shared_override_name": "matched", "dispatch_and_lexical_super_effects": "matched",
                    "unchanged_sdk_root": "rejected", "dependency_bytes": "unchanged", "coverage": "partial",
                    "metadata": "retained except disclosed names and D8 CLASS annotation loss"}
        (work / "oracle-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
