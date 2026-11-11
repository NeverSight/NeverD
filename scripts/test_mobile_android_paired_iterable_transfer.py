#!/usr/bin/env python3
"""Verify paired iterable transfers through independent result and void entry profiles."""
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
    parser.add_argument("--void-entry", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-paired-iterable-transfer-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "paired_iterable_transfer/java"
        observers = [FIXTURES / "harness" / (name + ".java") for name in ("PairedIterableCaller", "PairedIterableHarness")]
        if args.void_entry:
            authored = work / "void-source"
            shutil.copytree(fixture, authored); fixture = authored
            client = fixture / "fixture/Client.java"
            text = client.read_text()
            for before, after in (("public static Object call()", "public static void call()"),
                                  ("Map<?, ?> result = Provider.merge(Trace.input, Trace.rows);", "Provider.write(Trace.sink, Trace.rows);"),
                                  ("Trace.after = result; Trace.hit(8);", "Trace.hit(8);"),
                                  ("        return result;\n", "")):
                if text.count(before) != 1: raise RuntimeError("Owned void iterable client anchor changed")
                text = text.replace(before, after)
            client.write_text(text)
            observer_source = work / "void-observers"; observer_source.mkdir()
            caller, oracle = observers
            text = caller.read_text()
            for before, after in (("{ return Client.call(); }", "{ Client.call(); return null; }"),
                                  ("Map<?, ?> result = Provider.merge(Trace.input, Trace.rows);", "Provider.write(Trace.sink, Trace.rows);"),
                                  ("Trace.after = result; Trace.hit(8);", "Trace.hit(8);"),
                                  ("return result;", "return null;")):
                if text.count(before) != 1: raise RuntimeError("Frozen void iterable caller anchor changed")
                text = text.replace(before, after)
            (observer_source / caller.name).open("x").write(text)
            text = oracle.read_text().replace("static final boolean VOID_ENTRY = false;", "static final boolean VOID_ENTRY = true;")
            start = text.index("    static Object invoke("); end = text.index("    public static void main(", start)
            text = text[:start] + """    static Object invoke(int route) throws Throwable {
        if (route == 0) { Client.call(); return null; }
        if (route == 1) return PairedIterableCaller.call();
        if (route == 2) return PairedIterableCaller.direct();
        Trace.hit(9);
        if (route == 3) {
            try { Provider.class.getMethod("write", Map.class, Iterable.class).invoke(null, Trace.sink, Trace.rows); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        } else MethodHandles.lookup().findStatic(Provider.class, "write", MethodType.methodType(void.class, Map.class, Iterable.class))
            .invokeExact(Trace.sink, Trace.rows);
        Trace.hit(8); return null;
    }
""" + text[end:]
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
        if len(all_classes) != 4 or len(all_methods) != 11:
            raise RuntimeError("Owned paired array void inventory changed")
        providers = {"Lfixture/Provider;", "Lfixture/Pair;"}
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
                for mode in range(33):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "PairedIterableHarness", str(route), str(mode)], f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9,]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Owned paired array behavior matrix changed")
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
            raise RuntimeError("Owned paired array void DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original paired array definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-call-disassembly")
        witness = "Lfixture/Provider;." + ("write:" if args.void_entry else "merge:")
        if witness not in disassembly or "invoke-static" not in disassembly:
            raise RuntimeError("D8 lost the exact static paired array call witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Paired array recovery changed original definitions or contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=8,
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Paired array recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Paired array class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Paired array method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Paired array call changed original effects or failure state")
        variance = b"-" if args.void_entry else b"+"
        original_signature = (b"<K:Ljava/lang/Object;V:Ljava/lang/Object;>(Ljava/util/Map<" + variance + b"TK;" + variance + b"TV;>;Ljava/lang/Iterable<+Lfixture/Pair<+TK;+TV;>;>;)")
        original_signature += b"V" if args.void_entry else b"Ljava/util/Map<TK;TV;>;"
        map_slots = b"Map<" + variance + b"TK;" + variance + b"TV;>"
        wrong_variance = b"+" if args.void_entry else b"-"
        mutations = (
            ("reversed-map-slots", original_signature.replace(map_slots, b"Map<" + variance + b"TV;" + variance + b"TK;>")),
            ("repeated-produced-formal", original_signature.replace(b"Pair<+TK;+TV;>", b"Pair<+TK;+TK;>")),
            ("map-variance", original_signature.replace(map_slots, b"Map<" + wrong_variance + b"TK;" + variance + b"TV;>")),
            ("producer-variance", original_signature.replace(b"Pair<+TK;+TV;>", b"Pair<-TK;+TV;>")),
            ("wrapper-variance", original_signature.replace(b"Iterable<+Lfixture/Pair", b"Iterable<-Lfixture/Pair")),
            ("unbound-slot", original_signature.replace(b"Pair<+TK;+TV;>", b"Pair<*+TV;>")),
            ("extra-formal", original_signature.replace(b"V:Ljava/lang/Object;>", b"V:Ljava/lang/Object;W:Ljava/lang/Object;>")),
            ("unknown-slot-formal", original_signature.replace(b"Pair<+TK;+TV;>", b"Pair<+TK;+TU;>")),
            ("nonobject-bound", original_signature.replace(b"K:Ljava/lang/Object;", b"K:Ljava/lang/Number;")),
            ("wrong-result", original_signature[:-1] + b"I" if args.void_entry else original_signature.replace(b")Ljava/util/Map<TK;TV;>;", b")Ljava/util/Map<TV;TK;>;")),
        )
        for label, changed in mutations:
            if changed == original_signature:
                raise RuntimeError("Signature counterexample did not change the contract: " + label)
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Provider.class", original_signature, changed)
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
                raise RuntimeError("Paired array invocation accepted an unproved signature: " + label)
        body = (fixture / "fixture/Provider.java").read_text()
        getters = "K key = pair.component1();\n            V value = pair.component2();"
        put = "sink.put(key, value);"
        loop = "for (Pair<? extends K, ? extends V> pair : rows) {"
        wrong_programs = (
            ("bypassed-getters", getters, "K key = pair.first; V value = pair.second;", 0),
            ("reversed-getters", getters, "V value = pair.component2(); K key = pair.component1();", 0),
            ("reloaded-pair", "V value = pair.component2();", "V value = ((Pair<? extends K, ? extends V>)Trace.original[0]).component2();", 0),
            ("reloaded-map", put, "Trace.sink.put(key, value);", 0),
            ("swapped-put-slots", put, "sink.put((K)(Object)value, (V)(Object)key);", 0),
            ("checked-discarded-result", put, "String ignored = (String)sink.put(key, value);", 0),
            ("repeated-put", put, put + " " + put, 0),
            ("eager-copied-input", loop, "java.util.ArrayList<Pair<? extends K, ? extends V>> copied = new java.util.ArrayList<>(); for (Pair<? extends K, ? extends V> item : rows) copied.add(item); rows = copied; " + loop, 0),
            ("repeated-iterator", loop, loop + " rows.iterator();", 0),
            ("wrapped-getter-failure", "V value = pair.component2();", "V value; try { value = pair.component2(); } catch (RuntimeException failure) { throw new IllegalStateException(); }", 9),
        )
        if not args.void_entry:
            wrong_programs += (
                ("returned-input", "return (Map<K, V>)Trace.returned;", "return (Map<K, V>)first;", 0),
                ("early-returned-snapshot", "write(output, rows);", "Map<K,V> early = (Map<K,V>)Trace.returned; write(output, rows);", 32),
                ("early-output-snapshot", "Trace.firstCount = first.size();\n        Map<K, V> output = (Map<K, V>)Trace.output;", "Map<K, V> output = (Map<K, V>)Trace.output; Trace.firstCount = first.size();", 0),
            )
        for label, before, after, mode in wrong_programs:
            if body.count(before) != 1 or before == after:
                raise RuntimeError("Owned paired iterable counterexample anchor changed: " + label)
            wrong = work / label / "fixture/Provider.java"; wrong.parent.mkdir(parents=True)
            changed = body.replace(before, after)
            if label == "early-returned-snapshot": changed = changed.replace("return (Map<K, V>)Trace.returned;", "return early;")
            wrong.open("x").write(changed)
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     "PairedIterableHarness", "0", str(mode)], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned paired iterable transfer changed" not in result.stderr:
                raise RuntimeError("Frozen paired iterable observer missed " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable observers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "dependency_provider": args.dependency_provider, "void_entry": args.void_entry,
                    "original_metadata_and_exceptions": "unchanged",
                    "complete_two_formal_iterable_map_result_and_void_relations": "matched",
                    "iterator_map_getter_put_result_snapshots_and_exact_failures": "matched",
                    "same_code_signature_mutations": len(mutations), "independent_wrong_programs": len(wrong_programs),
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
