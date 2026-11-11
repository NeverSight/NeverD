#!/usr/bin/env python3
"""Verify closed getter snapshots and a checked generic interface upcast."""
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-closed-getter-upcast-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "closed_getter_upcast"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=25)
        provider_originals = work / "original-provider-classes"
        verify.compile_local([path for path in sorted((fixture / "java").rglob("*.java")) if path.name != "Child.java"],
                             provider_originals, classpath=originals, release=8)
        for path in provider_originals.rglob("*.class"):
            shutil.copy2(path, originals / path.relative_to(provider_originals))
        verify.compile_local(sorted((fixture / "harness").glob("*.java")), harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk_hash = sha(args.android_jar)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 8 or len(all_methods) != 12:
            raise RuntimeError("Owned closed getter upcast inventory changed")
        providers = {"Lfixture/Trace;", "Lfixture/Parent;", "Lfixture/Provider;", "Lfixture/Mapping;", "Lfixture/Middle;", "Lfixture/Leaf;", "Lfixture/Token;"}
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
                for mode in range(22):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "ClosedGetterHarness", str(route), str(mode)], f"{label}-{route}-{mode}")
                    pattern = f"route={route},mode={mode},attempt=[01],events=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                        raise RuntimeError("Frozen closed getter upcast observer changed")
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
            raise RuntimeError("Owned closed getter upcast DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original closed getter upcast definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-call-disassembly")
        if ("Lfixture/Provider;.first:()Ljava/lang/Object;" not in disassembly
                or "Lfixture/Provider;.mapping:()Lfixture/Leaf;" not in disassembly
                or "Lfixture/Parent;.<init>:(Ljava/lang/Object;Lfixture/Mapping;)V" not in disassembly
                or "check-cast" not in disassembly):
            raise RuntimeError("D8 lost the original closed getters, cast or generic superclass call")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != 25):
            raise RuntimeError("Closed getter upcast recovery changed original definitions or source contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=25,
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes if owner != "Lfixture/Mapping;"}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Closed getter upcast recovery invented or omitted a runtime entry/helper")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, actual = records[owner], after[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != actual.get(key):
                    raise RuntimeError("Original closed getter upcast class metadata changed: " + key)
            for method, metadata in before["methods"].items():
                if actual["methods"].get(method) != metadata or after_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original closed getter upcast method metadata or Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Closed getter upcast recovery changed casts, overloads, references or failure state")
        signature = b"()Lfixture/Leaf<TE;Lfixture/Token;>;"
        mutations = (
            ("unknown-getter-formal", "Provider", signature, signature.replace(b"TE;", b"TX;")),
            ("method-formal-result", "Provider", signature, b"<M:Ljava/lang/Object;>()Lfixture/Leaf<TM;Lfixture/Token;>;"),
            ("raw-getter-result", "Provider", signature, b"()Lfixture/Leaf;"),
            ("changed-provider-bound", "Provider", b"<E:Ljava/lang/Object;>Ljava/lang/Object;", b"<E:Ljava/lang/Number;>Ljava/lang/Object;"),
            ("variant-receiver", "Child", b"(Lfixture/Provider<TE;>;)V", b"(Lfixture/Provider<+TE;>;)V"),
            ("unknown-caller-formal", "Child", b"(Lfixture/Provider<TE;>;)V", b"(Lfixture/Provider<TX;>;)V"),
            ("rebound-interface", "Middle", b"<K:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Mapping<TK;TV;>;", b"<K:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Mapping<TV;TK;>;"),
            ("raw-interface", "Middle", b"<K:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Mapping<TK;TV;>;", b"<K:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Mapping;"),
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
                    token in result.stderr.lower() for token in ("generic", "constructor", "signature", "requires an unchanged declared reference or scalar call"))):
                raise RuntimeError("Closed getter upcast constructor accepted an unproved signature: " + label)
        bodies = {name: (fixture / ("java/fixture/" + name + ".java")).read_text() for name in ("Child", "Provider", "Parent")}
        wrong_programs = (
            ("lost-first-getter", "Child", "Object first=receiver.first();", "Object first=null;", 0),
            ("repeated-mapping-getter", "Child", "Object erased=receiver.mapping();", "receiver.mapping();Object erased=receiver.mapping();", 0),
            ("late-first-snapshot", "Child", "super(first,mapping)", "super(receiver.firstValue,mapping)", 0),
            ("lost-mapping-snapshot", "Provider", "Trace.hit(4);return saved;", "Trace.hit(4);return mappingValue;", 0),
            ("wrong-first-result", "Provider", "Trace.hit(2);return saved;", "Trace.hit(2);return firstValue;", 0),
            ("wrong-receiver", "Child", "this.receiver=receiver;", "this.receiver=null;", 0),
            ("late-parent-record", "Parent", "Trace.seen=this;Trace.hit(6);", "Trace.hit(6);Trace.seen=this;", 14),
            ("lost-provider-mutation", "Provider", "firstValue=Trace.replacementAgain;", "", 0),
        )
        for label, owner, before, changed, mode in wrong_programs:
            body = bodies[owner]
            if body.count(before) != 1 or before == changed:
                raise RuntimeError("Owned closed getter upcast counterexample anchor changed: " + label)
            wrong = work / label / ("fixture/" + owner + ".java")
            wrong.parent.mkdir(parents=True)
            wrong.open("x").write(body.replace(before, changed))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=25 if owner == "Child" else 8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                    "ClosedGetterHarness", "0", str(mode)], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned closed getter upcast changed" not in result.stderr:
                raise RuntimeError("Frozen closed getter upcast observer missed " + label)
        if ({"originals": hashes(originals), "harness": hashes(harness)} != frozen
                or sha(args.android_jar) != sdk_hash or sha(dependency) != dependency_hash):
            raise RuntimeError("Original closed getter upcast inputs, frozen observers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "dependency_provider": args.dependency_provider, "original_metadata_and_exceptions": "unchanged",
                    "closed_getter_results_and_complete_generic_interface_upcast": "matched",
                    "ordered_getter_snapshots_initialization_partial_effects_and_exact_failures": "matched",
                    "same_code_signature_mutations": len(mutations), "independent_wrong_programs": len(wrong_programs),
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
