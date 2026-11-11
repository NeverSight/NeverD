#!/usr/bin/env python3
"""Verify scoped array elements with a complete contravariant consumer and void result."""
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
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--boolean-result", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-array-consumer-void-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "array_consumer_void/java"
        observers = [FIXTURES / "harness" / (name + ".java") for name in ("ArrayConsumerCaller", "ArrayConsumerHarness")]
        if args.boolean_result:
            authored = work / "boolean-source"
            shutil.copytree(fixture, authored)
            fixture = authored
            provider = fixture / "fixture/api/Provider.java"
            text = provider.read_text()
            for before, after in (("void apply(", "boolean apply("),
                                  ("Trace.hit(8);", "Trace.hit(8); return decision <= 0;")):
                if text.count(before) != 1:
                    raise RuntimeError("Owned array predicate source anchor changed")
                text = text.replace(before, after)
            provider.write_text(text)
            client = fixture / "fixture/Client.java"
            text = client.read_text()
            for before, after in (("void call(", "boolean call("),
                                  ("Facade.apply(Trace.current, Trace.sink, begin, end);", "boolean result = Facade.apply(Trace.current, Trace.sink, begin, end);"),
                                  ("Trace.hit(9);", "Trace.hit(9); return result;")):
                if text.count(before) != 1:
                    raise RuntimeError("Owned array predicate client anchor changed")
                text = text.replace(before, after)
            client.write_text(text)
            observer_source = work / "boolean-observers"
            observer_source.mkdir()
            caller, oracle = observers
            text = caller.read_text()
            for before, after in (("void throughClient(", "boolean throughClient("),
                                  ("{ Client.call(begin, end); }", "{ return Client.call(begin, end); }"),
                                  ("void direct(", "boolean direct("),
                                  ("Facade.apply(Trace.current, Trace.sink, begin, end);", "boolean result = Facade.apply(Trace.current, Trace.sink, begin, end);"),
                                  ("Trace.hit(9);", "Trace.hit(9); return result;")):
                if text.count(before) != 1:
                    raise RuntimeError("Frozen array predicate caller anchor changed")
                text = text.replace(before, after)
            (observer_source / caller.name).open("x").write(text)
            text = oracle.read_text()
            start = text.index("    static void invoke(")
            end = text.index("    public static void main(", start)
            text = text[:start] + """    static boolean invoke(int route, int begin, int end) throws Throwable {
        if (route == 0) return Client.call(begin, end);
        if (route == 1) return ArrayConsumerCaller.throughClient(begin, end);
        if (route == 2) return ArrayConsumerCaller.direct(begin, end);
        Trace.hit(2);
        boolean result;
        if (route == 3) {
            try { result = (Boolean)Facade.class.getMethod("apply", Object[].class, Sink.class, int.class, int.class)
                .invoke(null, Trace.current, Trace.sink, begin, end); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        } else result = (boolean)MethodHandles.lookup().findStatic(Facade.class, "apply",
            MethodType.methodType(boolean.class, Object[].class, Sink.class, int.class, int.class))
            .invokeExact(Trace.current, Trace.sink, begin, end);
        Trace.hit(9); return result;
    }
""" + text[end:]
            for before, after in (("void.class", "boolean.class"),
                                  ("try { invoke(route, begin, end); }", "boolean actual = false; try { actual = invoke(route, begin, end); }"),
                                  ("else check(failure == null);", "else check(failure == null);\n            if (failure == null) check(actual == (mode != 1));")):
                if before != "void.class" and text.count(before) != 1:
                    raise RuntimeError("Frozen array predicate oracle anchor changed")
                text = text.replace(before, after)
            (observer_source / oracle.name).open("x").write(text)
            observers = [observer_source / path.name for path in observers]
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local(observers, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(
            {"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 10:
            raise RuntimeError("Owned array consumer void inventory changed")
        providers = {"Lfixture/Sink;", "Lfixture/api/Provider;", "Lfixture/api/Facade;"}
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            result = []
            for route in range(5):
                for mode in range(23):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "ArrayConsumerHarness", str(route), str(mode)], f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Owned array consumer behavior matrix changed")
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
            raise RuntimeError("Owned array consumer void DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original array consumer definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-call-disassembly")
        if "Lfixture/api/Facade;.apply:" not in disassembly or "invoke-static" not in disassembly:
            raise RuntimeError("D8 lost the inherited static array consumer call witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Array consumer recovery changed original definitions or contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=8,
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes if owner != "Lfixture/Sink;"}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Array consumer recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Array consumer class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Array consumer method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Array consumer call changed original effects or failure state")
        original_signature = b"<T:Ljava/lang/Object;>([TT;Lfixture/Sink<-TT;>;II)V"
        if args.boolean_result:
            original_signature = original_signature[:-1] + b"Z"
        mutations = (
            ("covariant-consumer", original_signature.replace(b"<-TT;>", b"<+TT;>")),
            ("unbound-consumer", original_signature.replace(b"<-TT;>", b"<*>")),
            ("unbound-array", original_signature.replace(b"[TT;", b"[Ljava/lang/Object;")),
            ("extra-formal", original_signature.replace(b">(", b"U:Ljava/lang/Object;>(")),
            ("foreign-consumer-formal", original_signature.replace(b"<-TT;>", b"<-TU;>")),
            ("nested-array", original_signature.replace(b"[TT;", b"[[TT;")),
            ("wrong-scalar", original_signature.replace(b";II)", b";JI)")),
            ("nonvoid-result", original_signature[:-1] + (b"V" if args.boolean_result else b"I")),
        )
        for label, changed in mutations:
            if changed == original_signature:
                raise RuntimeError("Signature counterexample did not change the contract: " + label)
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/api/Provider.class", original_signature, changed)
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            negative_classpath = list(classpath)
            if args.dependency_provider:
                negative_jar = work / (label + "-provider.jar")
                with zipfile.ZipFile(negative_jar, "x", compression=zipfile.ZIP_DEFLATED) as jar:
                    for owner in sorted(providers):
                        name = owner[1:-1] + ".class"
                        jar.writestr(name, (negative / name).read_bytes())
                negative_classpath[-1] = negative_jar
            negative_dex = work / (label + "-dex")
            negative_dex.mkdir()
            negative_paths = [negative / path.relative_to(originals) for path in paths]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *negative_classpath,
                        "--output", negative_dex, *negative_paths], label + "-d8")
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex / "classes.dex"),
                                     *map(str, negative_classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr.lower():
                raise RuntimeError("Array consumer invocation accepted an unproved signature: " + label)
        body = (fixture / "fixture/api/Provider.java").read_text()
        callback = "int decision = sink.compare(left, right);"
        write = "if (decision <= 0) values[begin] = replacement;\n        else values[end] = replacement;"
        wrong_programs = (
            ("bypassed-consumer", callback, "int decision = -1;", 0),
            ("repeated-consumer", callback, callback + " sink.compare(left, right);", 0),
            ("late-array-reference", write, write.replace("values[", "Trace.current["), 0),
            ("late-replacement", "T replacement = (T)Trace.replacement;\n        " + callback,
             callback + "\n        T replacement = (T)Trace.replacement;", 0),
            ("reversed-destination", "if (decision <= 0)", "if (decision > 0)", 0),
            ("dropped-write", write, "if (decision <= 0) { } else { }", 0),
            ("early-write", "Trace.hit(7);\n        " + write,
             write + "\n        Trace.hit(7);", 13),
            ("early-right-read", "T left = values[begin]; Trace.left = left; Trace.hit(6);\n        T right = values[end]; Trace.right = right;",
             "T left = values[begin]; Trace.left = left; T right = values[end]; Trace.right = right; Trace.hit(6);", 12),
            ("wrapped-failure", callback,
             "int decision; try { decision = sink.compare(left, right); } catch (RuntimeException failure) { throw new IllegalStateException(); }", 7),
        )
        if args.boolean_result:
            wrong_programs += (
                ("reversed-result", "return decision <= 0;", "return decision > 0;", 0),
                ("always-true", "return decision <= 0;", "return true;", 1),
                ("always-false", "return decision <= 0;", "return false;", 0),
            )
        for label, before, after, mode in wrong_programs:
            if body.count(before) != 1:
                raise RuntimeError("Owned array consumer counterexample anchor changed: " + label)
            wrong = work / label / "fixture/api/Provider.java"
            wrong.parent.mkdir(parents=True)
            wrong.open("x").write(body.replace(before, after))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     "ArrayConsumerHarness", "0", str(mode)], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned array consumer changed" not in result.stderr:
                raise RuntimeError("Frozen array consumer observer missed " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable observers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "dependency_provider": args.dependency_provider, "boolean_result": args.boolean_result,
                    "original_metadata_and_exceptions": "unchanged",
                    "scoped_array_and_complete_lower_consumer_void_contract": "matched",
                    "snapshot_arrays_values_write_order_callbacks_and_failure_identity": "matched",
                    "same_code_signature_mutations": len(mutations), "independent_wrong_programs": len(wrong_programs),
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
