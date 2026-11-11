#!/usr/bin/env python3
"""Verify complete unary carrier and binary callback relations with opaque results."""
from __future__ import annotations

import argparse
import copy
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
    from scripts.test_mobile_android_visibility_generic_bridge import compiler_facts
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_visibility_generic_bridge import compiler_facts


SIGNATURE = ("<T:Ljava/lang/Object;>(Lfixture/Carrier<+TT;>;"
             "Lfixture/Comparison<-TT;-TT;Ljava/lang/Boolean;>;)Lfixture/Carrier<TT;>;")
TARGET = "Lfixture/Provider;->select(Lfixture/Carrier;Lfixture/Comparison;)Lfixture/Carrier;"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--invariant-input", action="store_true")
    callback_shape = parser.add_mutually_exclusive_group()
    callback_shape.add_argument("--key-selector", action="store_true")
    callback_shape.add_argument("--unary-callback", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-binary-callback-carrier-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "binary_callback_carrier/java"
        source = (fixture / "fixture/Provider.java").read_text()
        inputs = sorted(fixture.rglob("*.java"))
        signature = SIGNATURE
        observers = [FIXTURES / "harness" / (name + ".java") for name in
                     ("BinaryCallbackCarrierCaller", "BinaryCallbackCarrierHarness")]
        if args.invariant_input:
            if source.count("Carrier<? extends T> source") != 1:
                raise RuntimeError("Owned producer Signature anchor changed")
            source = source.replace("Carrier<? extends T> source", "Carrier<T> source")
            changed = work / "invariant-java/fixture/Provider.java"
            changed.parent.mkdir(parents=True)
            changed.open("x").write(source)
            inputs = [changed if p.name == "Provider.java" else p for p in inputs]
            signature = signature.replace("Carrier<+TT;>", "Carrier<TT;>")
        if args.unary_callback:
            def replace_once(value, before, after):
                if value.count(before) != 1:
                    raise RuntimeError("Owned unary callback source anchor changed: " + before)
                return value.replace(before, after)
            source = replace_once(source, "Comparison<? super T,? super T,Boolean>",
                                  "Comparison<? super T,Boolean>")
            source = replace_once(source, "comparison.apply(left, right)", "comparison.apply(left)")
            signature = signature.replace("Comparison<-TT;-TT;Ljava/lang/Boolean;>",
                                          "Comparison<-TT;Ljava/lang/Boolean;>")
            inputs = []
            for original in sorted(fixture.rglob("*.java")):
                value = source if original.name == "Provider.java" else original.read_text()
                if original.name == "Comparison.java":
                    value = replace_once(value, "Comparison<A,B,R> { R apply(A first, B second); }",
                                         "Comparison<A,R> { R apply(A first); }")
                destination = work / "unary-java" / original.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.open("x").write(value)
                inputs.append(destination)
            frozen_observers = []
            for original in observers:
                value = original.read_text()
                if original.name == "BinaryCallbackCarrierHarness.java":
                    for before, after in (
                        (">,fixture.Comparison<? super T, ? super T, java.lang.Boolean>)",
                         ">,fixture.Comparison<? super T, java.lang.Boolean>)"),
                        ("Comparison<Object,Object,Boolean> callback = (a, b) -> {",
                         "Comparison<Object,Boolean> callback = (a) -> {\n            Object b = second;"),
                        ("Comparison<String,String,Boolean> wrong = (a, b) ->",
                         "Comparison<String,Boolean> wrong = (a) ->"),
                    ):
                        value = replace_once(value, before, after)
                destination = work / original.name
                destination.open("x").write(value)
                frozen_observers.append(destination)
            observers = frozen_observers
        if args.key_selector:
            def replace_once(value, before, after):
                if value.count(before) != 1:
                    raise RuntimeError("Owned key selector source anchor changed: " + before)
                return value.replace(before, after)
            for before, after in (
                ("public static <T> Carrier<T>", "public static <T,K> Carrier<T>"),
                ("Comparison<? super T,? super T,Boolean>", "Comparison<? super T,? extends K>"),
                ("Boolean accepted = comparison.apply(left, right);", "K accepted = comparison.apply(left);"),
                ("accepted.booleanValue()", "((Boolean)(Object)accepted).booleanValue()"),
            ):
                source = replace_once(source, before, after)
            signature = signature.replace("<T:Ljava/lang/Object;>", "<T:Ljava/lang/Object;K:Ljava/lang/Object;>").replace(
                "Comparison<-TT;-TT;Ljava/lang/Boolean;>", "Comparison<-TT;+TK;>")
            inputs = []
            for original in sorted(fixture.rglob("*.java")):
                value = source if original.name == "Provider.java" else original.read_text()
                if original.name == "Comparison.java":
                    value = replace_once(value, "Comparison<A,B,R> { R apply(A first, B second); }",
                                         "Comparison<A,R> { R apply(A first); }")
                destination = work / "key-java" / original.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.open("x").write(value)
                inputs.append(destination)
            frozen = []
            for original in observers:
                value = original.read_text()
                if original.name == "BinaryCallbackCarrierHarness.java":
                    for before, after in (
                        ("public static <T> fixture.Carrier<T>", "public static <T,K> fixture.Carrier<T>"),
                        (">,fixture.Comparison<? super T, ? super T, java.lang.Boolean>)",
                         ">,fixture.Comparison<? super T, ? extends K>)"),
                        ("Boolean accepted = mode == 11 ? null : mode == 12 ? Boolean.FALSE : Boolean.TRUE;",
                         "Object accepted = mode == 24 ? new Object() : mode == 11 ? null : mode == 12 ? Boolean.FALSE : Boolean.TRUE;"),
                        ("Comparison<Object,Object,Boolean> callback = (a, b) -> {",
                         "Comparison<Object,Object> callback = (a) -> {\n            Object b = second;"),
                        ("Comparison<String,String,Boolean> wrong = (a, b) ->", "Comparison<String,Boolean> wrong = (a) ->"),
                        ("mode == 9 || mode == 11 ?", "mode == 9 || mode == 11 || mode == 24 ?"),
                        ("mode != 9 && mode != 11;", "mode != 9 && mode != 11 && mode != 24;"),
                        ("mode == 16 || mode == 19)", "mode == 16 || mode == 19 || mode == 24)"),
                        ("&& mode != 16 && mode != 23;", "&& mode != 16 && mode != 23 && mode != 24;"),
                    ):
                        value = replace_once(value, before, after)
                destination = work / original.name
                destination.open("x").write(value)
                frozen.append(destination)
            observers = frozen
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals, release=8)
        verify.compile_local(observers, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(
            {"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        complete_facts = compiler_facts(originals) if args.unary_callback else None
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 9:
            raise RuntimeError("Original binary callback carrier inventory changed")
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        if records["Lfixture/Provider;"]["methods"][TARGET]["signature"] != signature:
            raise RuntimeError("Original complete callback relation changed")
        interfaces = {"Lfixture/Carrier;", "Lfixture/Comparison;"}
        providers = interfaces | {"Lfixture/Provider;"}
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def command(runtime, route, mode):
            return [verify.java, "-cp", os.pathsep.join(map(str, runtime)), "BinaryCallbackCarrierHarness",
                    str(route), str(mode), str(args.invariant_input).lower()]

        def execute(runtime, label):
            outputs = []
            for route in range(4):
                for mode in range(25 if args.key_selector else 24):
                    output = verify.run(command(runtime, route, mode), f"{label}-{route}-{mode}")
                    pattern = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                        raise RuntimeError("Original binary callback observer output changed: " + output)
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")

        def dex(root, destination, dependencies=None):
            destination.mkdir()
            paths = [p for p in sorted(root.rglob("*.class"))
                     if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *(dependencies or classpath), "--output", destination, *paths],
                       "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Original binary callback DEX inventory changed")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original binary callback entry")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("field_source_bindings")
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Binary callback binding changed original entries or generic contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
            classpath=dependency if args.dependency_provider else None, release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes - interfaces}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Binary callback binding invented or omitted a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != actual_records[owner].get(key):
                    raise RuntimeError("Original binary callback class metadata changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                if actual_records[owner]["methods"].get(identity) != metadata or actual_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original binary callback method metadata changed: " + identity)
        if args.unary_callback:
            rebuilt_facts = compiler_facts(rebuilt)
            if coverage.get("minimum_java_release", 8) != 8 or set(rebuilt_facts) != classes:
                raise RuntimeError("Unary callback compiler release or class inventory changed")
            for owner in classes:
                wanted, actual = copy.deepcopy(complete_facts[owner]), copy.deepcopy(rebuilt_facts[owner])
                actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
                if wanted["major"] != 52 or actual != wanted:
                    raise RuntimeError("Complete unary callback Code/access/Signature/parameter metadata changed: " + owner)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Binary callback binding changed original effects, results or failures")
        invocation = "comparison.apply(left)" if args.key_selector or args.unary_callback else "comparison.apply(left, right)"
        declaration = "K accepted" if args.key_selector else "Boolean accepted"
        unboxing = "((Boolean)(Object)accepted).booleanValue()" if args.key_selector else "accepted.booleanValue()"
        wrong_programs = (
            ("cached-second", (("T right = source.next();", "T right = left;"),), 0),
            ("swapped-inputs", ((invocation, "comparison.apply(right)" if args.key_selector or args.unary_callback else "comparison.apply(right, left)"),), 0),
            ("duplicate-callback", ((f"{declaration} = {invocation};", f"{invocation}; {declaration} = {invocation};"),), 0),
            ("missing-callback", ((f"{declaration} = {invocation};", f"{declaration} = " +
                                   ("(K)(Object)" if args.key_selector else "") + "Boolean.TRUE;"),), 0),
            ("early-output", (("Trace.step(1);", "Carrier cached = Trace.output; Trace.step(1);"),
                              ("return (Carrier<T>)Trace.output;", "return (Carrier<T>)cached;")), 17),
            ("input-output", (("return (Carrier<T>)Trace.output;", "return (Carrier<T>)source;"),), 0),
            ("early-unboxing", ((f"Trace.step(7);\n        Trace.flag = {unboxing} ? 1 : 0;",
                                 f"Trace.flag = {unboxing} ? 1 : 0;\n        Trace.step(7);"),), 9),
            ("late-publication", (("Trace.first = left;\n        Trace.step(3);",
                                   "Trace.step(3);\n        Trace.first = left;"),), 6),
        )
        for label, changes, mode in wrong_programs:
            changed_source = source
            for before, after in changes:
                if changed_source.count(before) != 1:
                    raise RuntimeError("Original binary callback counterexample anchor changed: " + label)
                changed_source = changed_source.replace(before, after)
            changed = work / label / "fixture/Provider.java"
            changed.parent.mkdir(parents=True)
            changed.open("x").write(changed_source)
            wrong = work / (label + "-classes")
            verify.compile_local([changed], wrong, classpath=originals, release=8)
            result = subprocess.run(command((wrong, originals, harness), 1, mode), text=True,
                capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned binary callback carrier" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect binary callback behavior: " + label)
        drifts = (
            ("first-consumer", "Comparison<-TT;", "Comparison<+TT;"),
            ("second-consumer", "-TT;-TT;", "-TT;+TT;"),
            ("callback-result", "Ljava/lang/Boolean;", "TT;"),
            ("lower-callback-result", "Ljava/lang/Boolean;", "-Ljava/lang/Boolean;"),
            ("variant-carrier-result", ")Lfixture/Carrier<TT;>;", ")Lfixture/Carrier<+TT;>;"),
            ("unbound-formal", "<T:Ljava/lang/Object;>", "<T:Ljava/lang/Object;U:Ljava/lang/Object;>"),
        )
        if args.unary_callback:
            # Producer variance and invariant input/formal completion already
            # have separate complete proofs. These controls instead change
            # array scope, declared arity or the set of bound method formals.
            input_carrier = "(Lfixture/Carrier<" + ("" if args.invariant_input else "+") + "TT;>;"
            drifts = (
                ("array-callback-input", "Comparison<-TT;", "Comparison<[TT;"),
                ("callback-arity", "Comparison<-TT;", "Comparison<-TT;-TT;"),
                ("input-carrier-arity", input_carrier, "(Lfixture/Carrier<TT;TT;>;"),
                ("result-carrier-arity", ")Lfixture/Carrier<TT;>;", ")Lfixture/Carrier<TT;TT;>;"),
                ("unbound-formal", "<T:Ljava/lang/Object;>", "<T:Ljava/lang/Object;U:Ljava/lang/Object;>"),
                ("lower-carrier-input", input_carrier, "(Lfixture/Carrier<-TT;>;"),
            )
        if args.key_selector:
            drifts = (
                ("foreign-input-role", "Comparison<-TT;+TK;>", "Comparison<-TK;+TK;>"),
                ("repeated-input-formal", "Comparison<-TT;+TK;>", "Comparison<-TT;+TT;>"),
                ("upper-input-role", "Comparison<-TT;+TK;>", "Comparison<+TT;+TK;>"),
                ("unknown-key-role", "Comparison<-TT;+TK;>", "Comparison<-TT;*>"),
                ("variant-carrier-result", ")Lfixture/Carrier<TT;>;", ")Lfixture/Carrier<-TT;>;"),
                ("unbound-formal", "<T:Ljava/lang/Object;K:Ljava/lang/Object;>",
                 "<T:Ljava/lang/Object;K:Ljava/lang/Object;U:Ljava/lang/Object;>"),
            )
        for label, before, after in drifts:
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Provider.class", signature.encode(), signature.replace(before, after).encode())
            (work / (label + "-metadata.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            negative_classpath = classpath
            if args.dependency_provider:
                negative_dependency = work / (label + "-provider.jar")
                with zipfile.ZipFile(negative_dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
                    for owner in sorted(providers):
                        name = owner[1:-1] + ".class"
                        jar.writestr(name, (negative / name).read_bytes())
                negative_classpath = ["--classpath", args.android_jar.resolve(), "--classpath", negative_dependency]
            negative_dex = dex(negative, work / (label + "-dex"), negative_classpath)
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, negative_classpath),
                "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Binary callback proof admitted incomplete metadata: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original binary callback inputs or immutable observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
            "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
            "invariant_input": args.invariant_input, "key_selector": args.key_selector, "unary_callback": args.unary_callback,
            "independent_wrong_programs": len(wrong_programs),
            "complete_metadata_mutations_rejected": len(drifts), "all_original_entries_metadata_and_exceptions": "unchanged",
            "source_callback_unboxing_and_initialization_order": "matched", "opaque_returns_and_later_caller_casts": "matched",
            "original_inputs_and_immutable_callers": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
