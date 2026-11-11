#!/usr/bin/env python3
"""Verify scoped inherited interface default calls without virtual redispatch."""
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-scoped-interface-super-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "scoped_interface_super"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=8)
        verify.compile_local(sorted((fixture / "harness").glob("*.java")), harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk_hash = sha(args.android_jar)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 7 or len(all_methods) != 11:
            raise RuntimeError("Owned scoped interface constructor inventory changed")
        providers = all_classes - {"Lfixture/Child;"}
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
                for mode in range(17):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "ScopedSuperHarness", str(route), str(mode)], f"{label}-{route}-{mode}")
                    pattern = f"route={route},mode={mode},attempt=[01],events=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                        raise RuntimeError("Frozen scoped interface constructor observer changed")
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
            raise RuntimeError("Owned scoped interface constructor DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original scoped interface constructor definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-call-disassembly")
        if "invoke-super" not in disassembly or "Lfixture/Facade;.subList:(II)Lfixture/View;" not in disassembly:
            raise RuntimeError("D8 lost the original qualified inherited default call witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != (1 if args.dependency_provider else 2) or coverage["status"] != "partial"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != (8 if args.dependency_provider else 9)):
            raise RuntimeError("Scoped interface recovery changed original definitions or source contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes - {"Lfixture/Facade;", "Lfixture/Marker;", "Lfixture/View;"}}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Scoped interface recovery invented or omitted a runtime entry/helper")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, actual = records[owner], after[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != actual.get(key):
                    raise RuntimeError("Original scoped interface class metadata changed: " + key)
            for method, metadata in before["methods"].items():
                if actual["methods"].get(method) != metadata or after_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original scoped interface method metadata or Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Scoped interface recovery changed casts, overloads, references or failure state")
        child_signature = b"(II)Lfixture/View<TE;>;"
        facade_signature = b"<E:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Root<TE;>;Lfixture/Marker;"
        mutations = (
            ("raw-edge", "Facade", facade_signature, facade_signature.replace(b"<TE;>", b"")),
            ("changed-edge", "Facade", facade_signature, facade_signature.replace(b"<TE;>", b"<Ljava/lang/Object;>")),
            ("unknown-edge", "Facade", facade_signature, facade_signature.replace(b"<TE;>", b"<TX;>")),
            ("star-edge", "Facade", facade_signature, facade_signature.replace(b"<TE;>", b"<*>")),
            ("changed-own-result", "Child", child_signature, child_signature.replace(b"<TE;>", b"<Ljava/lang/Object;>")),
            ("changed-default-result", "Root", child_signature, child_signature.replace(b"<TE;>", b"<Ljava/lang/Object;>")),
            ("unknown-default-result", "Root", child_signature, child_signature.replace(b"<TE;>", b"<TX;>")),
            ("method-formal-default", "Root", child_signature, b"<X:Ljava/lang/Object;>" + child_signature),
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
                    token in result.stderr.lower() for token in ("generic", "super invocation", "source", "signature", "interface"))):
                raise RuntimeError("Scoped interface constructor accepted an unproved signature: " + label)
        bodies = {name: (fixture / ("java/fixture/" + name + ".java")).read_text() for name in ("Root", "Child")}
        wrong_programs = (
            ("class-super", "Child", "Facade.super.subList(from, to)", "(View<E>)super.subList(from, to)", 0),
            ("virtual-redispatch", "Child", "Facade.super.subList(from, to)", "this.subList(from, to)", 0),
            ("swapped-arguments", "Child", "Facade.super.subList(from, to)", "Facade.super.subList(to, from)", 0),
            ("lost-self", "Root", "Trace.seen = this;", "Trace.seen = null;", 0),
            ("inserted-query", "Root", "Trace.hit(3);", "size(); Trace.hit(3);", 0),
            ("reloaded-result", "Root", "return output;", "return (View<E>)Trace.result;", 0),
            ("lost-after", "Child", "Trace.after = output;", "Trace.after = null;", 0),
            ("duplicate-default", "Root", "Trace.hit(4);", "Trace.hit(4); Trace.hit(4);", 0),
            ("early-cast", "Root", "Trace.hit(3);\n        View<E> output = (View<E>)Trace.result;", "View<E> output = (View<E>)Trace.result;\n        Trace.hit(3);", 2),
        )
        for label, owner, before, changed, mode in wrong_programs:
            body = bodies[owner]
            if body.count(before) != 1 or before == changed:
                raise RuntimeError("Owned scoped interface counterexample anchor changed: " + label)
            wrong = work / label / ("fixture/" + owner + ".java")
            wrong.parent.mkdir(parents=True)
            wrong.open("x").write(body.replace(before, changed))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                    "ScopedSuperHarness", "0", str(mode)], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned scoped interface super changed" not in result.stderr:
                raise RuntimeError("Frozen scoped interface observer missed " + label)
        if ({"originals": hashes(originals), "harness": hashes(harness)} != frozen
                or sha(args.android_jar) != sdk_hash or sha(dependency) != dependency_hash):
            raise RuntimeError("Original scoped interface inputs, frozen observers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "dependency_provider": args.dependency_provider, "original_metadata_and_exceptions": "unchanged",
                    "scoped_inherited_default_and_qualified_dispatch": "matched",
                    "receiver_arguments_result_snapshots_cast_timing_initialization_and_exact_failures": "matched",
                    "same_code_signature_mutations": len(mutations), "independent_wrong_programs": len(wrong_programs),
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
