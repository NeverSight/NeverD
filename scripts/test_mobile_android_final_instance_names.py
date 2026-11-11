#!/usr/bin/env python3
"""Verify closed final JVM names independently of unrelated dependency names."""
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
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--kotlin-lib", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--ordinary", action="store_true")
    parser.add_argument("--dependency-base", action="store_true")
    parser.add_argument("--implicit-finality", action="store_true")
    parser.add_argument("--generic-owner", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-final-instance-names-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "final_instance_names"
        original_java, originals, harness = work / "original-java", work / "originals", work / "harness"
        kotlin = args.kotlin_lib.resolve()
        compiler_jars = sorted(kotlin.glob("*.jar"))
        if not (kotlin / "kotlin-compiler.jar").is_file():
            raise RuntimeError("Original witness requires the unmodified Kotlin compiler distribution")
        inputs = [args.android_jar.resolve(), *compiler_jars]
        input_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        stdlib, annotations = kotlin / "kotlin-stdlib.jar", kotlin / "annotations-13.0.jar"
        dependencies = []
        for jar in (stdlib, annotations):
            output = work / jar.name
            with zipfile.ZipFile(jar) as source, zipfile.ZipFile(output, "x") as dest:
                for entry in source.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):
                            raise RuntimeError("Unsafe compiler dependency entry")
                        dest.writestr(entry.filename, source.read(entry))
            dependencies.append(output)
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), original_java, release=8)
        originals.mkdir()
        kotlin_sources = sorted((fixture / "kotlin").rglob("*.kt"))
        if args.ordinary or args.implicit_finality or args.generic_owner:
            authored = work / "authored-source"; authored.mkdir()
            selected = []
            for source in kotlin_sources:
                text = source.read_text()
                if source.name == "Terminal.kt":
                    if text.count("    @JvmSynthetic\n") != 1:
                        raise RuntimeError("Missing original final synthetic source profile")
                    if args.ordinary:
                        text = text.replace("    @JvmSynthetic\n", "")
                    if args.generic_owner:
                        if text.count("class Terminal : Base()") != 1 or text.count("fun read(input: Any?): Any?") != 1:
                            raise RuntimeError("Original generic owner source anchors changed")
                        text = text.replace("class Terminal : Base()", "class Terminal<T> : Base()")
                        text = text.replace("fun read(input: Any?): Any?", "fun read(input: Any?): T?")
                        text = text.replace("return Trace.consume(input)", "return Trace.consume(input) as T?")
                    if args.implicit_finality:
                        if text.count("    fun read(") != 1:
                            raise RuntimeError("Original implicit finality source anchor changed")
                        text = text.replace("    fun read(", "    open fun read(")
                path = authored / source.name; path.write_text(text); selected.append(path)
            kotlin_sources = selected
        verify.run([verify.java, "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (stdlib, original_java))),
                    "-jvm-target", "1.8", "-d", originals, *kotlin_sources], "kotlinc-original")
        for path in sorted(original_java.rglob("*.class")):
            target = originals / path.relative_to(original_java)
            target.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(path, target)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("FinalInstanceNamesCaller", "FinalInstanceNamesHarness")], harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 4 or len(all_methods) != 11:
            raise RuntimeError("Original final instance inventory changed: " + repr((len(all_classes), len(all_methods))))
        excluded = {"Lfixture/Alien;"} | ({"Lfixture/Base;"} if args.dependency_base else set())
        classes = all_classes - excluded
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        dependency = work / "owned-dependency.jar"
        with zipfile.ZipFile(dependency, "x") as archive:
            for owner in sorted(excluded):
                path = owner[1:-1] + ".class"; archive.writestr(path, (originals / path).read_bytes())
        dependencies.append(dependency)
        dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        classpath = ["--classpath", args.android_jar.resolve()]
        for path in dependencies:
            classpath += ["--classpath", path]
        original_name = "Lfixture/Terminal;->read-value(Ljava/lang/Object;)Ljava/lang/Object;"
        original_flags = (0x1 if args.ordinary else 0x1001) | (0 if args.implicit_finality else 0x10)
        original_method = records["Lfixture/Terminal;"]["methods"][original_name]
        if original_method["access"] != original_flags or not records["Lfixture/Terminal;"]["access"] & 0x10:
            raise RuntimeError("Compiler changed the original final-class public method flags")
        if args.generic_owner and (records["Lfixture/Terminal;"]["signature"] != "<T:Ljava/lang/Object;>Lfixture/Base;"
                or original_method["signature"] != ("(Ljava/lang/Object;)TT;" if args.ordinary else None)):
            raise RuntimeError("Compiler changed the complete generic owner/result contracts")

        def execute(runtime, spelling, synthetic, label):
            outputs = []
            for route in range(4):
                for receiver in range(2):
                    for value in range(3):
                        for mode in range(4):
                            output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                "FinalInstanceNamesHarness", str(route), str(receiver), str(value), str(mode),
                                spelling, str(synthetic).lower()], f"{label}-{route}-{receiver}-{value}-{mode}")
                            if output != "attempts=2,results-identities-effects-failures=matched,dependency-name=retained\n":
                                raise RuntimeError("Frozen final instance observer missed its cases: " + output)
                            outputs.append(output)
            return outputs

        baseline = execute((originals, harness, *dependencies), "read-value", not args.ordinary, "baseline")

        def make_dex(root, directory):
            directory.mkdir()
            paths = [p for p in sorted(root.rglob("*.class"))
                     if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", directory, *paths], "d8-" + directory.name)
            dex = directory / "classes.dex"
            if list(directory.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned final instance changed its DEX inventory")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original final instance declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        row = rows[original_name]
        spelling = row.get("source_name")
        renamed = "Lfixture/Terminal;->" + str(spelling) + "(Ljava/lang/Object;)Ljava/lang/Object;"
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count") != 1 or coverage["status"] != "partial"
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")
                or row.get("projection_kind") != "virtual-method-identifier" or row.get("source_identity") != renamed
                or row.get("source_name_declaration") != original_name or row.get("source_name_family") != [original_name]
                or spelling != "__neverdVirtualMethod1" or len(row.get("metadata_differences", [])) != 3):
            raise RuntimeError("Closed final instance lost its scoped name or reported differences")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=8,
                             classpath=os.pathsep.join(map(str, dependencies)))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != (methods - {original_name}) | {renamed} | helpers:
            raise RuntimeError("Final instance inventory changed beyond disclosed names/helpers")
        actual_records, actual_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        drops = []
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original final instance class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                wanted = copy.deepcopy(metadata); selected = renamed if identity == original_name else identity
                if identity == original_name:
                    wanted["name"] = spelling
                    wanted["access"] &= ~0x1000
                for key in ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations"):
                    if wanted.get(key) and any(wanted[key]):
                        drops.append({"identity": identity, "attribute": key, "original": wanted[key]})
                        wanted[key] = []
                if after["methods"][selected] != wanted or actual_exceptions[selected] != exceptions[identity]:
                    raise RuntimeError("Final instance method metadata changed beyond disclosed name, flag and D8 CLASS loss: " + identity)
        expected_drop_sites = {("Lfixture/Terminal;->" + name + "(Ljava/lang/Object;)Ljava/lang/Object;", key)
                               for name in ("read-value", "entry") for key in
                               ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations")}
        if not args.ordinary:
            expected_drop_sites.remove((original_name, "runtime_invisible_parameter_annotations"))
        if {(d["identity"], d["attribute"]) for d in drops} != expected_drop_sites:
            raise RuntimeError("Original compiler CLASS annotation loss changed its exact sites")
        (work / "d8-class-annotation-loss.json").write_text(json.dumps(drops, indent=2) + "\n")
        if execute((rebuilt, harness, *dependencies), spelling, False, "rebuilt") != baseline:
            raise RuntimeError("Closed final instance results, identity, order or failures changed")
        negative = work / "private-originals"; shutil.copytree(originals, negative)
        flags = original_flags
        receipt = change_method_flags(negative / "fixture/Terminal.class", b"(Ljava/lang/Object;)Ljava/lang/Object;",
                                      flags, (flags & ~1) | 2, name=b"read-value")
        (work / "private-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = make_dex(negative, work / "private-dex")
        rejected = work / "private-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "-o", str(rejected)],
            text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "private-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists():
            raise RuntimeError("Changed private final instance obtained an unproved public name")
        mutants = {"wrong-effect": ("values[0] += seed;", "values[0] += 1;"),
                   "wrong-result": ("Object result = value;", "Object result = new Object();"),
                   "missing-callback": ("hit(2);", "")}
        for label, (old, new) in mutants.items():
            source = work / label / "Trace.java"; source.parent.mkdir()
            text = (fixture / "java/fixture/Trace.java").read_text()
            if text.count(old) != 1:
                raise RuntimeError("Missing exact authored final instance mutant site")
            source.write_text(text.replace(old, new))
            patched = work / (label + "-classes"); verify.compile_local([source], patched)
            result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (patched, originals, harness, *dependencies))),
                "FinalInstanceNamesHarness", "3", "0", "0", "0", "read-value", str(not args.ordinary).lower()],
                text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-execution.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "AssertionError" not in result.stderr:
                raise RuntimeError("Frozen final instance oracle accepted an effect/result/callback mutant")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies} != dependency_hashes
                or {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs} != input_hashes):
            raise RuntimeError("Original compiler, frozen caller or dependency bytes changed")
        evidence = {"status": "success", "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
            "class_count": len(classes), "method_count": len(methods), "compiled_behavior_mutants": len(mutants),
            "implicit_finality": args.implicit_finality, "generic_owner": args.generic_owner,
            "dependency_names_and_bytes": "unchanged", "metadata": "retained except disclosed name/flag and D8 CLASS loss",
            "coverage": "partial"}
        (work / "oracle-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
