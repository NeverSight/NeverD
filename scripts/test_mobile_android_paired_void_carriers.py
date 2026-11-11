#!/usr/bin/env python3
"""Verify complete paired carrier/consumer types, direct seeds and void dispatch."""
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
    parser.add_argument("--callback-tail", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-paired-void-carriers-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "paired_void_carriers/java"
        observers = [FIXTURES / "harness" / (name + ".java") for name in ("PairedVoidCaller", "PairedVoidHarness")]
        if args.callback_tail:
            copied = work / "callback-java"
            shutil.copytree(fixture, copied)
            fixture = copied
            pair = fixture / "fixture/Pair.java"
            pair.write_text("""package fixture;
public interface Pair<I,O,R> { R apply(I first,O second,int flags,Object token,long bits); }
""")
            for name in ("Provider", "Primary", "Alternate"):
                source = fixture / "fixture" / (name + ".java")
                text = source.read_text()
                if text.count("Pair<I, O>") != 1:
                    raise RuntimeError("Original paired callback source anchor changed")
                text = text.replace("Pair<I, O>", "Pair<? super O, ? super I, Object>")
                if name != "Provider":
                    text = text.replace("O result = pair.apply(input, flags, token, bits);",
                                        "Object result = pair.apply((O)Trace.second, input, flags, token, bits);")
                source.write_text(text)
            trace = fixture / "fixture/Trace.java"
            trace.write_text(trace.read_text().replace("public final class Trace {", "public final class Trace {\n    public static Object second;"))
            text = observers[1].read_text()
            replacements = (
                ("implements Pair<int[], Object>", "implements Pair<Object, int[], Object>"),
                ("Object result, seen, token;", "Object result, seen, token, second, expectedSecond;"),
                ("public Object apply(int[] input, int flag, Object key, long raw)",
                 "public Object apply(Object other, int[] input, int flag, Object key, long raw)"),
                ("calls++; Trace.hit(4); seen = input; flags = flag; token = key; bits = raw;",
                 "require(other==expectedSecond); calls++; Trace.hit(4); seen = input; flags = flag; token = key; bits = raw; second=other; Trace.second=new Object();"),
                ("require(slots[0].equals(formals[0]) && slots[1].equals(formals[1]));",
                 "require(slots.length==3&&((WildcardType)slots[0]).getLowerBounds()[0].equals(formals[1])&&((WildcardType)slots[1]).getLowerBounds()[0].equals(formals[0])&&slots[2]==Object.class);"),
                ("maker.seen = sentinel; maker.token = sentinel; maker.flags = 0; maker.bits = 0;",
                 "maker.seen = sentinel; maker.token = sentinel; maker.flags = 0; maker.bits = 0; maker.second=sentinel;maker.expectedSecond=new Object();Trace.second=maker.expectedSecond;"),
                ("require(maker.seen == (returned ? input : sentinel));",
                 "require(maker.seen == (returned ? input : sentinel));\n            require(maker.second==(returned?maker.expectedSecond:sentinel));"),
            )
            for before, after in replacements:
                if text.count(before) != 1:
                    raise RuntimeError("Frozen paired callback observer anchor changed")
                text = text.replace(before, after)
            observer = work / "PairedVoidHarness.java"
            observer.open("x").write(text)
            observers[1] = observer
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local(observers, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 6 or len(all_methods) != 12:
            raise RuntimeError("Owned paired-void-carriers inventory changed")
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
            for route in range(4):
                for kind in range(2):
                    for mode in range(14):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "PairedVoidHarness", str(route), str(kind), str(mode)], f"{label}-{route}-{kind}-{mode}")
                        expected = f"route={route},kind={kind},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Original paired void behavior matrix changed: " + output)
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
            raise RuntimeError("Owned paired-void-carrier fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original paired-void-carriers runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-argument-disassembly")
        if "Lfixture/Provider;.accept:" not in disassembly or "invoke-virtual/range" not in disassembly:
            raise RuntimeError("D8 lost the original abstract virtual call witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Paired void carrier invocation recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != 8:
            raise RuntimeError("Paired void carrier invocation unexpectedly raised its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if owner != "Lfixture/Pair;"}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Paired void carrier invocation recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original paired-void-carrier class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original paired-void-carrier method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Paired void carrier call changed an original effect or result")
        original_signature = b"<I:Ljava/lang/Object;O:Ljava/lang/Object;>(ILfixture/Pair<TI;TO;>;TI;Ljava/lang/Object;J)V"
        carrier_arguments = b"<TI;TO;>"
        repeated_arguments, variant_arguments = b"<TI;TI;>", b"<+TI;TO;>"
        if args.callback_tail:
            carrier_arguments = b"<-TO;-TI;Ljava/lang/Object;>"
            repeated_arguments = b"<-TI;-TI;Ljava/lang/Object;>"
            variant_arguments = b"<+TO;-TI;Ljava/lang/Object;>"
            original_signature = original_signature.replace(b"<TI;TO;>", carrier_arguments)
        for label, changed in (
                ("missing-seed", original_signature.replace(b">;TI;", b">;Ljava/lang/Object;")),
                ("repeated-formal", original_signature.replace(carrier_arguments, repeated_arguments)),
                ("variant-carrier", original_signature.replace(carrier_arguments, variant_arguments)),
                ("extra-formal", original_signature.replace(b"O:Ljava/lang/Object;>", b"O:Ljava/lang/Object;U:Ljava/lang/Object;>"))):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Provider.class", original_signature, changed)
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
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
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic invocation requires" not in result.stderr:
                raise RuntimeError("Paired void carrier invocation accepted an unproved method contract: " + label)
        # Independent implementations that bypass or repeat the original
        # invocation must fail the unchanged side-effect and identity oracle.
        body = (fixture / "fixture/Client.java").read_text()
        marker = "provider.accept(flags, pair, input, token, bits);"
        if body.count(marker) != 1:
            raise RuntimeError("Owned carrier call witness changed")
        for label, replacement in (
                ("bypass", ""),
                ("duplicate", marker + " " + marker)):
            wrong = work / label / "fixture/Client.java"
            wrong.parent.mkdir(parents=True)
            wrong.write_text(body.replace(marker, replacement))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     "PairedVoidHarness", "1", "0", "0"], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "paired void invocation changed" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        callback_counterexamples = []
        if args.callback_tail:
            original = (fixture / "fixture/Primary.java").read_text()
            before = "Object result = pair.apply((O)Trace.second, input, flags, token, bits);"
            for label, after, mode in (
                ("reversed-callback-inputs", "Object result = pair.apply((O)(Object)input, (I)Trace.second, flags, token, bits);", 0),
                ("changed-opaque-input", "Object result = pair.apply(null, input, flags, token, bits);", 0),
                ("early-callback-result-cast", "Object result = (int[])pair.apply((O)Trace.second, input, flags, token, bits);", 10),
            ):
                if original.count(before) != 1:
                    raise RuntimeError("Original paired callback mutation anchor changed")
                source = work / label / "fixture/Primary.java"
                source.parent.mkdir(parents=True)
                source.open("x").write(original.replace(before, after))
                compiled = work / (label + "-classes")
                verify.compile_local([source], compiled, classpath=originals, release=8)
                result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                    "PairedVoidHarness", "1", "0", str(mode)], text=True, capture_output=True, timeout=300, env=verify.environment)
                (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
                if result.returncode == 0 or "paired void invocation changed" not in result.stderr:
                    raise RuntimeError("Frozen observer missed " + label)
                callback_counterexamples.append(label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "callback_tail": args.callback_tail, "independent_callback_counterexamples": callback_counterexamples,
                    "fresh_jvm_cases": 112, "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "original_metadata_and_exceptions": "unchanged", "invariant_pair_seed_and_polymorphic_effects": "matched",
                    "original_exception_identity": "matched", "same_code_counterexamples": "rejected",
                    "independent_bypass_and_repeated_call_counterexamples": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
