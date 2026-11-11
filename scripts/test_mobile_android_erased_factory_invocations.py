#!/usr/bin/env python3
"""Verify inherited generic static factories through unchanged erased calls."""
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
    parser.add_argument("--private-factory", choices=("static", "instance"))
    parser.add_argument("--generic-owner", action="store_true")
    args = parser.parse_args()
    if args.generic_owner and args.private_factory:
        parser.error("--generic-owner uses the original public static methods")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-erased-factory-invocations-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "erased_factory_invocations/java"
        if args.generic_owner:
            copied = work / "generic-owner-fixture"
            shutil.copytree(fixture, copied); fixture = copied
            provider = fixture / "fixture/api/Provider.java"; body = provider.read_text()
            if body.count("class Provider {") != 1: raise RuntimeError("Original generic owner declaration anchor changed")
            provider.write_text(body.replace("class Provider {", "class Provider<T> {", 1))
        if args.private_factory:
            copied = work / "fixture"
            shutil.copytree(fixture, copied)
            fixture = copied
            provider = fixture / "fixture/api/Provider.java"
            body = provider.read_text()
            before = "public static <T> Box<T> make(Factory<? extends T> factory, long flags) {\n        Trace.flags = flags; Trace.hit(2);"
            dispatch = "static " if args.private_factory == "static" else ""
            receiver = "" if args.private_factory == "static" else "new Provider()."
            after = ("public static Box<?> make(Factory<?> factory, long flags) {\n"
                     "        Trace.flags = flags; return " + receiver + "produce(factory);\n    }\n"
                     "    private " + dispatch + "final <T> Box<T> produce(Factory<? extends T> factory) {\n        Trace.hit(2);")
            if body.count(before) != 1:
                raise RuntimeError("Original private factory source anchor changed")
            provider.write_text(body.replace(before, after))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        observers = [FIXTURES / "harness" / (name + ".java") for name in
                     ("ErasedFactoryCaller", "ErasedFactoryHarness", "CarrierElementCaller", "CarrierElementHarness", "SeedFactoryCaller", "SeedFactoryHarness")]
        if args.generic_owner:
            authored = work / "generic-owner-observers"; authored.mkdir()
            for index, original in enumerate(observers):
                if not original.stem.endswith("Harness"): continue
                name = {"ErasedFactoryHarness": "make", "CarrierElementHarness": "element", "SeedFactoryHarness": "seed"}[original.stem]
                parameter = "Object.class" if name == "seed" else "Factory.class"
                text = original.read_text(); before = "    public static void main(String[] args) throws Throwable {"
                if text.count(before) != 1: raise RuntimeError("Original generic owner observer anchor changed")
                metadata = '''
        Class<?> owner = Class.forName("fixture.api.Provider",false,''' + original.stem + '''.class.getClassLoader());
        java.lang.reflect.Method method = owner.getDeclaredMethod("''' + name + '''",''' + parameter + ''',long.class);
        java.lang.reflect.TypeVariable<?>[] declared=owner.getTypeParameters(), scoped=method.getTypeParameters();
        require(declared.length==1&&scoped.length==1&&declared[0].getName().equals("T")&&scoped[0].getName().equals("T"));
        require(declared[0].getBounds()[0]==Object.class&&scoped[0].getBounds()[0]==Object.class&&!declared[0].equals(scoped[0]));
        require(declared[0].getGenericDeclaration()==owner&&scoped[0].getGenericDeclaration().equals(method));
'''
                target = authored / original.name; target.open("x").write(text.replace(before, before + metadata, 1)); observers[index] = target
        verify.compile_local(observers, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 7 or len(all_methods) != (20 if args.private_factory else 19):
            raise RuntimeError("Owned erased-factory-invocations inventory changed")
        providers = {"Lfixture/api/Provider;", "Lfixture/api/Middle;", "Lfixture/api/Facade;"}
        if args.private_factory:
            providers = {"Lfixture/Factory;", "Lfixture/Box;"}
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
            variants = ((0, 1, 2, 3, 4, 5), (0,), (0,), (0,))
            for variant, modes in enumerate(variants):
                for route in range(4):
                    for mode in modes:
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "ErasedFactoryHarness", str(variant), str(route), str(mode)], f"{label}-{variant}-{route}-{mode}")
                        if not re.fullmatch(f"variant={variant},route={route},mode={mode},trace=[0-9]+\\n", output):
                            raise RuntimeError("Original generic factory matrix changed: " + output)
                        result.append(output)
            for variant in range(2):
                for route in range(4):
                    for mode in range(8):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "CarrierElementHarness", str(variant), str(route), str(mode)],
                            f"{label}-element-{variant}-{route}-{mode}")
                        expected = f"variant={variant},route={route},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Original carrier element matrix changed: " + output)
                        result.append(output)
            for route in range(4):
                for mode in range(8):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "SeedFactoryHarness", str(route), str(mode)], f"{label}-seed-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Original seed factory matrix changed: " + output)
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
            raise RuntimeError("Owned generic factory fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original erased-factory-invocations runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-argument-disassembly")
        if "Lfixture/api/Facade;.make:" not in disassembly:
            raise RuntimeError("D8 lost the inherited static facade call witness")
        if args.private_factory:
            target = "Lfixture/api/Provider;.produce:(Lfixture/Factory;)Lfixture/Box;"
            opcode = "invoke-static" if args.private_factory == "static" else "invoke-direct"
            if not any(opcode in line and target in line for line in disassembly.splitlines()):
                raise RuntimeError("D8 lost the original private unary factory call witness")
            identity = "Lfixture/api/Provider;->produce(Lfixture/Factory;)Lfixture/Box;"
            signature = "<T:Ljava/lang/Object;>(Lfixture/Factory<+TT;>;)Lfixture/Box<TT;>;"
            if records["Lfixture/api/Provider;"]["methods"][identity]["signature"] != signature:
                raise RuntimeError("Compiler changed the private factory's complete formal relation")
            (work / "private-factory-witness.json").open("x").write(json.dumps(
                {"identity": identity, "signature": signature, "dispatch": opcode}, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Generic factory recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != 8:
            raise RuntimeError("Generic factory unexpectedly raised its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if owner != "Lfixture/Factory;"}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Generic factory recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original generic factory class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original generic factory method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Erased factory call changed an original effect or result")
        original_signature = b"<T:Ljava/lang/Object;>(Lfixture/Factory<+TT;>;J)Lfixture/Box<TT;>;"
        if args.private_factory:
            original_signature = original_signature.replace(b";J)", b";)")
        element_signature = b"<T:Ljava/lang/Object;>(Lfixture/Factory<+TT;>;J)TT;"
        seed_signature = b"<T:Ljava/lang/Object;>(TT;J)Lfixture/Box<TT;>;"
        for label, original, changed in (
                ("contravariant", original_signature, original_signature.replace(b"<+TT;>", b"<-TT;>")),
                ("extra-formal", original_signature, original_signature.replace(b"T:Ljava/lang/Object;>", b"T:Ljava/lang/Object;U:Ljava/lang/Object;>")),
                ("element-contravariant", element_signature, element_signature.replace(b"<+TT;>", b"<-TT;>")),
                ("element-extra-formal", element_signature, element_signature.replace(b"T:Ljava/lang/Object;>", b"T:Ljava/lang/Object;U:Ljava/lang/Object;>")),
                ("missing-seed-formal", seed_signature, seed_signature.replace(b"(TT;", b"(Ljava/lang/Object;")),
                ("unseeded-variant-result", seed_signature, seed_signature.replace(b"(TT;", b"(Ljava/lang/Object;").replace(b"Box<TT;>", b"Box<+TT;>")),
                ("changed-seed-result", seed_signature, seed_signature.replace(b"Box<TT;>", b"Box<Ljava/lang/Object;>")),
                ("seed-extra-formal", seed_signature, seed_signature.replace(b"T:Ljava/lang/Object;>", b"T:Ljava/lang/Object;U:Ljava/lang/Object;>"))):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/api/Provider.class", original, changed)
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
            if label == "missing-seed-formal":
                # The existing closed-bound proof admits a result-only formal
                # with reifiable inputs. Check that receipt through compilation
                # and the frozen seed observer; a variant result still fails.
                if result.returncode or not rejected.exists():
                    raise RuntimeError("Result-only carrier formal lost its existing proof")
                receipt = json.loads((rejected / "report.json").read_text())["android_method_recovery"]
                if ({row["identity"] for row in receipt["methods"]} != methods
                        or receipt["unrecovered_method_count"] or receipt.get("projected_method_count", 0)
                        or receipt.get("class_source_bindings") or receipt.get("generic_signature_projections")):
                    raise RuntimeError("Result-only carrier recovery changed original contracts")
                compiled = work / (label + "-compiled")
                dependency_path = negative_classpath[-1] if args.dependency_provider else None
                verify.compile_local(sorted((rejected / "sources").rglob("*.java")), compiled,
                                     classpath=dependency_path, release=8)
                observed_classes, observed_methods = compiler_inventory(verify, compiled)
                if observed_classes != classes or set(observed_methods) != methods | expected_helpers:
                    raise RuntimeError("Result-only carrier recovery changed original entries")
                expected_records = class_identity.compiler_classes(negative)
                observed_records = class_identity.compiler_classes(compiled)
                for owner in classes:
                    for identity, metadata in expected_records[owner]["methods"].items():
                        if observed_records[owner]["methods"].get(identity) != metadata:
                            raise RuntimeError("Result-only carrier method metadata changed")
                result_runtime = (compiled, harness, dependency_path) if args.dependency_provider else (compiled, harness)
                for route in range(4):
                    for mode in range(8):
                        observed = verify.run([verify.java, "-cp", os.pathsep.join(map(str, result_runtime)),
                            "SeedFactoryHarness", str(route), str(mode)], f"result-only-{route}-{mode}")
                        if observed != baseline[-32 + route * 8 + mode]:
                            raise RuntimeError("Result-only carrier changed an original seed effect")
                continue
            if result.returncode == 0 or rejected.exists() or "generic invocation requires" not in result.stderr:
                raise RuntimeError("Generic factory accepted an unproved method contract: " + label)
        invalid = work / "javac-negative/fixture/api/Provider.java"
        invalid.parent.mkdir(parents=True)
        invalid.write_text((fixture / "fixture/api/Provider.java").read_text().replace("Factory<? extends T>", "Factory<? super T>"))
        result = subprocess.run([str(verify.javac), "--release", "8", "-proc:none", "-classpath", str(originals),
                                 "-d", str(work / "javac-negative-classes"), str(invalid)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "javac-negative.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "incompatible types" not in result.stderr:
            raise RuntimeError("Independent javac accepted a contravariant factory result")
        # A separate implementation that reads twice must fail the unchanged oracle.
        wrong = work / "duplicate/fixture/api/Provider.java"
        wrong.parent.mkdir(parents=True)
        body = (fixture / "fixture/api/Provider.java").read_text()
        marker = "T value = factory.read();"
        if body.count(marker) != 1:
            raise RuntimeError("Owned generic factory source changed")
        wrong.write_text(body.replace(marker, "factory.read(); " + marker))
        verify.compile_local([wrong], work / "duplicate-classes", classpath=originals, release=8)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (work / "duplicate-classes", originals, harness))),
                                 "ErasedFactoryHarness", "0", "1", "0"], text=True, capture_output=True,
                                timeout=300, env=verify.environment)
        (work / "duplicate-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle did not detect a repeated callback")
        wrong_element = work / "duplicate-element/fixture/api/Provider.java"
        wrong_element.parent.mkdir(parents=True)
        marker = "T result = producer.read();"
        if body.count(marker) != 1:
            raise RuntimeError("Owned carrier element source changed")
        wrong_element.write_text(body.replace(marker, "producer.read(); " + marker))
        verify.compile_local([wrong_element], work / "duplicate-element-classes", classpath=originals, release=8)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (work / "duplicate-element-classes", originals, harness))),
                                 "CarrierElementHarness", "0", "1", "0"], text=True, capture_output=True,
                                timeout=300, env=verify.environment)
        (work / "duplicate-element-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle did not detect a repeated element read")
        wrong_seed = work / "duplicate-seed/fixture/api/Provider.java"
        wrong_seed.parent.mkdir(parents=True)
        seed_body = "Trace.flags = flags; Trace.hit(2);\n        return new Box<T>(value);"
        if body.count(seed_body) != 1:
            raise RuntimeError("Owned seed factory source changed")
        wrong_seed.write_text(body.replace(seed_body, "Trace.flags = flags; Trace.hit(2);\n        new Box<T>(value); return new Box<T>(value);"))
        verify.compile_local([wrong_seed], work / "duplicate-seed-classes", classpath=originals, release=8)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (work / "duplicate-seed-classes", originals, harness))),
                                 "SeedFactoryHarness", "1", "0"], text=True, capture_output=True,
                                timeout=300, env=verify.environment)
        (work / "duplicate-seed-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle did not detect a repeated seed allocation")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "private_factory": args.private_factory,
                    "generic_owner": args.generic_owner,
                    "original_metadata_and_exceptions": "unchanged", "inherited_static_initialization_and_erased_results": "matched",
                    "element_identity_mutable_aliases_and_original_later_casts": "matched",
                    "seed_identity_and_exact_carrier_allocation": "matched",
                    "result_only_carrier_formal_and_original_seed_effects": "matched",
                    "original_exception_identity": "matched", "same_code_counterexamples": "rejected",
                    "independent_repeated_callback_counterexample": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
