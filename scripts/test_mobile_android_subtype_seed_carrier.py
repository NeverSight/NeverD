#!/usr/bin/env python3
"""Verify a producer subtype bound, direct supertype seed and opaque result."""
from __future__ import annotations
import argparse
from collections import Counter
from contextlib import nullcontext
from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_interface_default_access import compiler_facts
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_interface_default_access import compiler_facts

OWNER = "Lfixture/Provider;"
TARGET = OWNER + "->select(Lfixture/Producer;Ljava/lang/Object;Lfixture/Context;JI)Lfixture/Result;"
SIGNATURE = "<T::TR;R:Ljava/lang/Object;>(Lfixture/Producer<+TT;>;TR;Lfixture/Context;JI)Lfixture/Result<TR;>;"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "kotlin-lib", "kotlin-java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-subtype-seed-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "subtype_seed_carrier"
        kotlin = args.kotlin_lib.resolve()
        if not (kotlin / "kotlin-compiler.jar").is_file(): raise RuntimeError("Original Kotlin compiler unavailable")
        inputs = [args.android_jar.resolve(), *sorted(kotlin.glob("*.jar"))]
        digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        input_hashes = {str(p): digest(p) for p in inputs}
        dependencies = []
        for p in (kotlin / "kotlin-stdlib.jar", kotlin / "annotations-13.0.jar"):
            dest = work / p.name
            with zipfile.ZipFile(p) as before, zipfile.ZipFile(dest, "x") as after:
                for entry in before.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"): raise RuntimeError("Unsafe compiler dependency")
                        after.writestr(entry.filename, before.read(entry))
            dependencies.append(dest)
        original_java, originals, harness = work / "original-java", work / "originals", work / "harness"
        verify.compile_local([p for p in sorted((fixture / "java").rglob("*.java")) if p.name != "Client.java"], original_java, release=8)
        originals.mkdir()
        compiler = [choose_jdk(args.kotlin_java_home) / "bin/java", "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (kotlin / "kotlin-stdlib.jar", original_java))), "-jvm-target", "1.8"]
        verify.run([*compiler, "-d", originals, *sorted((fixture / "kotlin").rglob("*.kt"))], "kotlinc-original")
        for p in original_java.rglob("*.class"):
            dest = originals / p.relative_to(original_java); dest.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(p, dest)
        # compile_local owns a fresh output directory; merge the separately compiled caller.
        caller = work / "original-caller"
        verify.compile_local([fixture / "java/fixture/Client.java"], caller, classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        for p in caller.rglob("*.class"):
            shutil.copy2(p, originals / p.relative_to(caller))
        verify.compile_local(sorted((fixture / "harness").glob("*.java")), harness, classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals); all_methods = set(all_methods)
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        if len(all_classes) != 6 or len(all_methods) != 8 or records[OWNER]["methods"][TARGET]["signature"] != SIGNATURE:
            raise RuntimeError("Original dependent producer/seed/result inventory or Signature changed")
        borrowed = {OWNER} if args.dependency_provider else set()
        classes = all_classes - borrowed; methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        if borrowed:
            dependency = work / "owned-provider.jar"
            with zipfile.ZipFile(dependency, "x") as jar:
                jar.writestr("fixture/Provider.class", (originals / "fixture/Provider.class").read_bytes())
            dependencies.append(dependency)
        dependency_hashes = {str(p): digest(p) for p in dependencies}
        def execute(runtime, label):
            outputs = []
            cases = [(variant, 0, 0) for variant in range(16)]
            cases += [(0, fail, kind) for fail in range(1, 7) for kind in (0, 1)]
            for route in range(4):
                for variant, fail, kind in cases:
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))), "SubtypeSeedHarness",
                                         str(route), str(variant), str(fail), str(kind)], f"{label}-{route}-{variant}-{fail}-{kind}")
                    pattern = f"route={route},variant={variant},fail={fail},kind={kind},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                        raise RuntimeError("Original subtype seed observer matrix changed")
                    outputs.append(output)
            return outputs
        baseline = execute((originals, harness), "baseline")
        classpath = ["--classpath", args.android_jar.resolve()]
        for p in dependencies: classpath += ["--classpath", p]
        dex = work / "dex"; dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8-original")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]: raise RuntimeError("Original subtype seed DEX count changed")
        dex_hash = digest(source_dex)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original subtype seed entry")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-dependent-call")
        if len(re.findall(r"invoke-static/range[^\n]*Lfixture/Provider;.select:\(Lfixture/Producer;Ljava/lang/Object;Lfixture/Context;JI\)Lfixture/Result;", disassembly)) != 1:
            raise RuntimeError("Original dependent producer/seed/wide call was lost")
        annotations = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        visible = Counter(re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations))
        if "VISIBILITY_BUILD" in annotations or visible != Counter({"Lkotlin/Metadata;": 1} if not borrowed else {}):
            raise RuntimeError("D8 changed subtype seed annotation retention")
        expected = {c: deepcopy(records[c]) for c in classes}; dropped = {}
        for c in classes:
            for identity, row in [(c, expected[c]), *expected[c]["fields"].items(), *expected[c]["methods"].items()]:
                for key in ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations"):
                    if row.get(key): dropped.setdefault(identity, {})[key] = row[key]
                    row[key] = []
        (work / "d8-class-annotation-loss.json").open("x").write(json.dumps(dropped, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage.get("class_source_bindings")):
            raise RuntimeError("Subtype seed recovery changed original definitions")
        projections = coverage.get("generic_signature_projections", [])
        if borrowed:
            if projections or coverage["status"] != "recovered": raise RuntimeError("SDK dependent call invented source projections")
        elif (coverage["status"] != "partial" or len(projections) != 1 or projections[0]["declaration"] != TARGET or len(projections[0]["changes"]) != 1
              or projections[0]["changes"][0]["kind"] != "type-variable-bound-position"):
            raise RuntimeError("Original dependent bound spelling change was not reported exactly")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=os.pathsep.join(map(str, dependencies)), release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c in {OWNER, "Lfixture/Client;", "Lfixture/Trace;"}}
        if not borrowed: expected_helpers.add(OWNER + "-><init>()V")
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Subtype seed recovery invented or omitted an entry/helper")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        if not borrowed:
            constructor = after[OWNER]["methods"][OWNER + "-><init>()V"]
            if constructor["access"] != 0x2 or constructor.get("signature"):
                raise RuntimeError("Source-only facade constructor must remain private and nongeneric")
        for c in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method", "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if after[c].get(key) != expected[c].get(key): raise RuntimeError("Undisclosed subtype seed class metadata changed: " + c + ":" + key)
            for identity, metadata in expected[c]["methods"].items():
                wanted = deepcopy(metadata)
                if identity == TARGET: wanted["signature"] = SIGNATURE.replace("T::TR;", "T:TR;")
                if after[c]["methods"].get(identity) != wanted or after_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Undisclosed subtype seed method metadata or Exceptions changed: " + identity)
        if execute((rebuilt, harness), "rebuilt") != baseline:
            raise RuntimeError("Subtype seed recovery changed snapshots, actual result, wide values or failures")
        source = (fixture / "kotlin/fixture/Provider.kt").read_text()
        mutations = (
            ("wrong-seed", "Trace.seenSeed = initial", "Trace.seenSeed = captured"),
            ("lost-value-snapshot", "val value: T = captured!!.value()", "captured!!.value(); val value: T = Trace.lateValue as T"),
            ("lost-context-snapshot", "context!!.accept", "Trace.context!!.accept"),
            ("truncated-flags", "accept(value, initial, flags, bits)", "accept(value, initial, flags.toInt().toLong(), bits)"),
            ("late-result", "return result", "return Trace.output as Result<R>?"),
            ("inferred-seed-result", "return result", "return initial as Result<R>?"),
            ("extra-producer-call", "val value: T = captured!!.value()", "captured!!.value(); val value: T = captured.value()"),
            ("lost-context-call", "context!!.accept(value, initial, flags, bits)", "if (false) context!!.accept(value, initial, flags, bits)"),
        )
        for label, before, changed in mutations:
            if source.count(before) != 1: raise RuntimeError("Owned subtype seed counterexample anchor changed")
            wrong = work / label / "Provider.kt"; wrong.parent.mkdir(); wrong.open("x").write(source.replace(before, changed, 1))
            compiled = work / (label + "-classes"); compiled.mkdir()
            verify.run([*compiler, "-d", compiled, wrong], "kotlinc-" + label)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness, *dependencies))),
                                     "SubtypeSeedHarness", "1", "0", "0", "0"], text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned subtype seed contract" not in result.stderr:
                raise RuntimeError("Frozen observer missed subtype seed change: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or digest(source_dex) != dex_hash
                or any(digest(Path(p)) != value for p, value in input_hashes.items())
                or any(digest(Path(p)) != value for p, value in dependency_hashes.items())):
            raise RuntimeError("Original subtype seed inputs, SDK or frozen observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods), "dependency_provider": args.dependency_provider,
                    "fresh_jvm_cases_per_stage": len(baseline), "attempts_per_case": 2, "independent_wrong_programs": len(mutations),
                    "complete_dependent_formal_producer_seed_and_result_contracts": "matched",
                    "original_definitions_metadata_and_exceptions": "matched; bound position disclosed",
                    "opaque_identity_snapshots_wide_values_effects_and_exact_failures": "matched",
                    "compiler_CLASS_annotation_loss_sites": len(dropped), "disclosed_private_facade_constructor": not bool(borrowed),
                    "original_inputs_and_frozen_observers": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n"); print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__": raise SystemExit(main())
