#!/usr/bin/env python3
"""Verify generic method defaults reuse the complete canonical invocation proof."""
from __future__ import annotations
import argparse
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
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_interface_default_access import compiler_facts
    from test_mobile_android_static_defaults import method_code

OWNER = "Lfixture/Api;"
TARGET = OWNER + "->fold(Ljava/lang/Object;Lfixture/Bi;)Ljava/lang/Object;"
ACCESS = OWNER + "->access$fold$jd(Lfixture/Api;Ljava/lang/Object;Lfixture/Bi;)Ljava/lang/Object;"
SIGNATURE = "<R:Ljava/lang/Object;>(TR;Lfixture/Bi<-TR;-Lfixture/Api;+TR;>;)TR;"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "kotlin-lib", "kotlin-java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-contracts", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-generic-default-fold-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "generic_default_fold"
        kotlin = args.kotlin_lib.resolve()
        inputs = [args.android_jar.resolve(), *sorted(kotlin.glob("*.jar"))]
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        input_hashes = {str(p): sha(p) for p in inputs}
        dependencies = []
        for source in (kotlin / "kotlin-stdlib.jar", kotlin / "annotations-13.0.jar"):
            target = work / source.name
            with zipfile.ZipFile(source) as before, zipfile.ZipFile(target, "x") as after:
                for entry in before.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):
                            raise RuntimeError("Unsafe compiler dependency")
                        after.writestr(entry.filename, before.read(entry))
            dependencies.append(target)
        originals, harness = work / "originals", work / "harness"
        trace, parent = work / "trace", work / "api"
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), trace, release=8)
        compiler = [choose_jdk(args.kotlin_java_home) / "bin/java", "-cp", kotlin / "*",
                    "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler", "-no-stdlib", "-no-reflect", "-jvm-target", "1.8", "-Xjvm-default=all-compatibility"]
        verify.run([*compiler, "-classpath", os.pathsep.join(map(str, (kotlin / "kotlin-stdlib.jar", trace))),
                    "-d", parent, *sorted((fixture / "kotlin").rglob("*.kt"))], "kotlinc-api")
        originals.mkdir()
        for root in (trace, parent):
            for source in root.rglob("*.class"):
                target = originals / source.relative_to(root)
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.exists():
                    raise RuntimeError("Duplicate original compiler definition")
                shutil.copy2(source, target)
                if source.read_bytes() != target.read_bytes() or method_code(source.read_bytes()) != method_code(target.read_bytes()):
                    raise RuntimeError("Original compiler Code changed")
        names = ("GenericDefaultFoldCaller", "GenericDefaultFoldHarness")
        verify.compile_local([FIXTURES / "harness" / (n + ".java") for n in names], harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 4 or len(all_methods) != 8:
            raise RuntimeError("Original inherited descriptor inventory changed")
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        if (set(records[OWNER]["methods"]) != {TARGET, ACCESS}
                or records[OWNER]["methods"][TARGET]["signature"] != SIGNATURE):
            raise RuntimeError("Original generic default method scope or Signature changed")
        declarations = {"Lfixture/Trace;", "Lfixture/Bi;"} if args.dependency_contracts else set()
        classes = all_classes - declarations
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        if declarations:
            target = work / "owned-contracts.jar"
            with zipfile.ZipFile(target, "x") as jar:
                for name in ("Bi", "Trace"):
                    jar.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
            dependencies.append(target)
        dependency_hashes = {str(p): sha(p) for p in dependencies}

        def execute(runtime, label):
            outputs = []
            for route in range(4):
                for variant in range(2):
                    for mode in range(13):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))), names[1],
                                             str(route), str(variant), str(mode)], f"{label}-{route}-{variant}-{mode}")
                        pattern = f"route={route},variant={variant},mode={mode},attempt=[01],events=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                            raise RuntimeError("Frozen inherited descriptor observer changed: " + output)
                        outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        classpath = ["--classpath", args.android_jar.resolve()]
        for target in dependencies:
            classpath += ["--classpath", target]
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        dex = work / "dex"
        dex.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original inherited descriptor DEX count changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original inherited descriptor definitions")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-inherited-default-disassembly")
        if "invoke-super" not in disassembly or "Lfixture/Api;.fold:(Ljava/lang/Object;Lfixture/Bi;)Ljava/lang/Object;" not in disassembly:
            raise RuntimeError("D8 lost the original inherited self-owned invoke-super witness")
        expected = {c: deepcopy(records[c]) for c in classes}
        dropped = {}
        for c in classes:
            for identity, row in [(c, expected[c]), *expected[c]["methods"].items()]:
                for key in ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations"):
                    if row.get(key):
                        dropped.setdefault(identity, {})[key] = row[key]
                    row[key] = []
        (work / "d8-class-annotation-loss.json").open("x").write(json.dumps(dropped, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial"
                or coverage.get("projected_method_count") != 2 or coverage.get("minimum_java_release") != 9
                or rows[ACCESS].get("projection_kind") != "static-interface-default-accessor"
                or not rows[ACCESS].get("metadata_differences") or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Inherited descriptor recovery changed original entries or contracts")
        helper = rows[TARGET]["default_body_helper"]
        if (rows[TARGET].get("projection_kind") != "interface-default-body" or rows[ACCESS].get("default_body_helper") != helper):
            raise RuntimeError("Generic default and exact accessor lost their one shared helper")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=os.pathsep.join(map(str, dependencies)), release=9)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c != "Lfixture/Bi;"}
        expected_helpers |= {helper, "Lfixture/Api$DefaultImpls;-><init>()V"}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Inherited descriptor recovery invented or omitted an original entry/helper")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        helper_rows = {r["class"] + "->" + r["name"] + r["prototype"]: r for r in coverage.get("generated_source_helpers", [])}
        if helper_rows[helper].get("generic_signature") != SIGNATURE or after[OWNER]["methods"][helper]["signature"] != SIGNATURE:
            raise RuntimeError("Generic default helper changed the complete original method Signature")
        for c in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if after[c].get(key) != expected[c].get(key):
                    raise RuntimeError("Undisclosed inherited descriptor class metadata changed: " + c + ":" + key)
            for identity, metadata in expected[c]["methods"].items():
                wanted = deepcopy(metadata)
                if identity == ACCESS:
                    wanted["access"] &= ~0x1000
                if after[c]["methods"].get(identity) != wanted or after_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Undisclosed inherited descriptor method metadata or Exceptions changed: " + identity)
        if execute((rebuilt, harness), "rebuilt") != baseline:
            raise RuntimeError("Inherited descriptor recovery changed default dispatch, effects, failure or identities")
        body = (fixture / "kotlin/fixture/Api.kt").read_text()
        mutations = (
            ("wrong-seed", "operation.invoke(seed, this)", "operation.invoke(Trace.seed as R, this)", 0),
            ("wrong-self", "Trace.before(this, seed)", "Trace.before(null, seed)", 0),
            ("reloaded-result", "return result", "return Trace.produced as R", 0),
            ("duplicate-callback", "val result = operation.invoke(seed, this)", "val result = operation.invoke(seed, this); operation.invoke(seed, this)", 0),
            ("dropped-before", "Trace.before(this, seed)", "", 0),
            ("dropped-after", "Trace.after(result)", "", 0),
            ("early-after", "val result = operation.invoke(seed, this)\n        Trace.after(result)", "Trace.after(seed)\n        val result = operation.invoke(seed, this)", 0),
            ("wrapped-failure", "Trace.after(result)", "try { Trace.after(result) } catch (failure: RuntimeException) { throw IllegalStateException() }", 6),
        )
        for label, old, changed, mode in mutations:
            if body.count(old) != 1:
                raise RuntimeError("Owned generic default counterexample anchor changed: " + label)
            source = work / label / "Api.kt"
            source.parent.mkdir()
            source.open("x").write(body.replace(old, changed))
            variant = work / (label + "-classes")
            verify.run([*compiler, "-classpath", os.pathsep.join(map(str, (kotlin / "kotlin-stdlib.jar", originals))),
                        "-d", variant, source], "kotlinc-" + label)
            result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (variant, originals, harness, *dependencies))),
                                     names[1], "2", "0", str(mode)], text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned generic default fold" not in result.stderr:
                raise RuntimeError("Frozen generic default observer missed " + label)
        label = "virtual-redispatch"
        source = work / label / (names[0] + ".java")
        source.parent.mkdir()
        body = (FIXTURES / "harness" / source.name).read_text()
        old = "return Api.DefaultImpls.fold(receiver, seed, operation);"
        if body.count(old) != 1:
            raise RuntimeError("Original generic default caller anchor changed")
        source.open("x").write(body.replace(old, "return receiver.fold(seed, operation);"))
        variant = work / (label + "-classes")
        verify.compile_local([source], variant, classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (variant, originals, harness, *dependencies))),
                                 names[1], "1", "1", "0"], text=True, capture_output=True, timeout=600, env=verify.environment)
        (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
        if result.returncode == 0 or "owned generic default fold" not in result.stderr:
            raise RuntimeError("Frozen generic default observer missed virtual redispatch")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or {str(p): sha(p) for p in inputs} != input_hashes or {str(p): sha(p) for p in dependencies} != dependency_hashes):
            raise RuntimeError("Original compiler inputs or frozen observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "stages": 2, "independent_wrong_programs": 9,
                    "original_Code": "compiler output unchanged", "complete_method_and_helper_Signature": "matched", "all_generated_Java": "compiled",
                    "original_metadata_and_Exceptions": "matched except disclosed synthetic flag and compiler CLASS annotation loss",
                    "default_dispatch_and_receiver_input_result_identity": "matched", "dependency_contracts": args.dependency_contracts,
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
