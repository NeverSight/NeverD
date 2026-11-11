#!/usr/bin/env python3
"""Verify reference-array loops before generic super constructors."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-reference-array-super-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "reference_array_super"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=25)
        provider_originals = work / "original-provider-classes"
        verify.compile_local([fixture / ("java/fixture/" + name + ".java")
                              for name in ("Parent", "Builder", "Path", "Element")],
                             provider_originals, classpath=originals, release=8)
        for path in provider_originals.rglob("*.class"):
            shutil.copy2(path, originals / path.relative_to(provider_originals))
        verify.compile_local(sorted((fixture / "harness").glob("*.java")), harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk_hash = sha(args.android_jar)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 6 or len(all_methods) != 11:
            raise RuntimeError("Owned reference-array constructor inventory changed")
        providers = {"Lfixture/Parent;", "Lfixture/Builder;", "Lfixture/Element;", "Lfixture/Path;"}
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = sha(dependency)
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            result = []
            for route in range(4):
                for mode in range(23):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "ReferenceArrayHarness", str(route), str(mode)], f"{label}-{route}-{mode}")
                    pattern = f"route={route},mode={mode},attempt=[01],events=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                        raise RuntimeError("Frozen reference-array constructor observer changed")
                    result.append(output)
            return result

        baseline = execute((originals, harness), "baseline")
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned reference-array constructor DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original reference-array constructor definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-call-disassembly")
        if ("new-array" not in disassembly or "aput-object" not in disassembly
                or "if-ge" not in disassembly or "Lfixture/Element;.<init>:()V" not in disassembly
                or "Lfixture/Parent;.<init>:(Lfixture/Builder;[Lfixture/Path;)V" not in disassembly):
            raise RuntimeError("D8 lost the original reference-array constructor loop")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != 25):
            raise RuntimeError("Reference-array recovery changed original definitions or source contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=25,
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Reference-array recovery invented or omitted a runtime entry/helper")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, actual = records[owner], after[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != actual.get(key):
                    raise RuntimeError("Original reference-array class metadata changed: " + key)
            for method, metadata in before["methods"].items():
                if actual["methods"].get(method) != metadata or after_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original reference-array method metadata or Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Reference-array recovery changed casts, overloads, references or failure state")
        parent = b"(Lfixture/Builder<TK;TV;>;[Lfixture/Path<TK;TV;TT;>;)V"
        element = b"<K:Ljava/lang/Object;V:Ljava/lang/Object;>Lfixture/Path<TK;TV;TK;>;"
        child = b"(Lfixture/Builder<TK;TV;>;I)V"
        mutations = (
            ("swapped-array-slots", "Parent", parent, parent.replace(b"Path<TK;TV;TT;>", b"Path<TV;TK;TT;>")),
            ("raw-array-component", "Parent", parent, parent.replace(b"Path<TK;TV;TT;>", b"Path")),
            ("projected-array-component", "Parent", parent, parent.replace(b"Path<TK;TV;TT;>", b"Path<***>")),
            ("method-formal-parent", "Parent", parent, b"<X:Ljava/lang/Object;>" + parent),
            ("swapped-element-edge", "Element", element, element.replace(b"Path<TK;TV;TK;>", b"Path<TV;TK;TK;>")),
            ("changed-element-repeat", "Element", element, element.replace(b"Path<TK;TV;TK;>", b"Path<TK;TV;TV;>")),
            ("extra-element-formal", "Element", element, element.replace(b";>Lfixture", b";X:Ljava/lang/Object;>Lfixture")),
            ("swapped-builder", "Child", child, child.replace(b"Builder<TK;TV;>", b"Builder<TV;TK;>")),
        )
        for label, owner, original, changed in mutations:
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / ("fixture/" + owner + ".class"), original, changed)
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            negative_classpath = list(classpath)
            if args.dependency_provider:
                negative_jar = work / (label + "-provider.jar")
                with zipfile.ZipFile(negative_jar, "x", compression=zipfile.ZIP_DEFLATED) as jar:
                    for provider in sorted(providers):
                        name = provider[1:-1] + ".class"
                        jar.writestr(name, (negative / name).read_bytes())
                negative_classpath[-1] = negative_jar
            negative_dex = work / (label + "-dex")
            negative_dex.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", *negative_classpath, "--output", negative_dex,
                        *[negative / path.relative_to(originals) for path in paths]], label + "-d8")
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex / "classes.dex"),
                                    *map(str, negative_classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if (result.returncode != 1 or rejected.exists() or not any(
                    token in result.stderr.lower() for token in ("generic", "constructor", "signature"))):
                raise RuntimeError("Reference-array constructor accepted an unproved signature: " + label)
        bodies = {name: (fixture / ("java/fixture/" + name + ".java")).read_text()
                  for name in ("Child", "Element", "Parent", "Path")}
        wrong_programs = (
            ("single-iteration", "Child", "for(int i=0;i<count;++i)", "if(count>0) { int i=0;", 2),
            ("wrong-array-length", "Child", "new Path[count]", "new Path[count+1]", 0),
            ("reused-element", "Child", "path[i]=new Element<K,V>();", "path[i]=i==0?new Element<K,V>():path[0];", 2),
            ("reversed-array", "Child", "path[i]=new Element<K,V>();", "path[count-1-i]=new Element<K,V>();", 2),
            ("wrong-runtime-array", "Child", "new Path[count]", "new Element[count]", 2),
            ("lost-marker", "Child", "this.marker=builder.marker;", "this.marker=null;", 2),
            ("lost-array-mutation", "Parent", "path[0]=null;", "", 2),
            ("early-array-read", "Parent", "Trace.parent=this;Trace.hit(3);", "Trace.parent=this;Trace.first=path.length==0?null:path[0];Trace.hit(3);", 13),
            ("early-completed-element", "Element", "Trace.hit(2);++Trace.completed;", "++Trace.completed;Trace.hit(2);", 9),
            ("lost-element-identity", "Path", "this.serial=++Trace.started;", "this.serial=1;++Trace.started;", 2),
        )
        for label, owner, before, changed, mode in wrong_programs:
            body = bodies[owner]
            if body.count(before) != 1 or before == changed:
                raise RuntimeError("Owned reference-array counterexample anchor changed: " + label)
            wrong = work / label / ("fixture/" + owner + ".java")
            wrong.parent.mkdir(parents=True)
            changed_body = body.replace(before, changed)
            if label == "single-iteration":
                changed_body = changed_body.replace("path[i]=new Element<K,V>();", "path[i]=new Element<K,V>(); }")
            wrong.open("x").write(changed_body)
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=25)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                    "ReferenceArrayHarness", "0", str(mode)], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned reference array super changed" not in result.stderr:
                raise RuntimeError("Frozen reference-array observer missed " + label)
        if ({"originals": hashes(originals), "harness": hashes(harness)} != frozen
                or sha(args.android_jar) != sdk_hash or sha(dependency) != dependency_hash):
            raise RuntimeError("Original reference-array inputs, frozen observers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "dependency_provider": args.dependency_provider, "original_metadata_and_exceptions": "unchanged",
                    "original_generic_array_and_element_edge_contract": "matched",
                    "loop_allocations_arrays_initialization_partial_fields_and_exact_failures": "matched",
                    "same_code_signature_mutations": len(mutations), "independent_wrong_programs": len(wrong_programs),
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
