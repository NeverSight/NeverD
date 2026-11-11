#!/usr/bin/env python3
"""Verify two independent generic formals through original virtual and interface calls."""
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
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-erased-pair-invocations-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "erased_pair_invocations/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("ErasedPairCaller", "ErasedPairHarness")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 8 or len(all_methods) != 15:
            raise RuntimeError("Owned erased-pair-invocations inventory changed")
        providers = {"Lfixture/Provider;", "Lfixture/Root;", "Lfixture/API;"}
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
            variants = ((0, 1, 2, 3, 4, 5, 6, 7), (0,), (0,), (0,), (0,), (0,), (0,))
            for flavor in (0, 1):
                for variant, modes in enumerate(variants):
                    for route in range(4):
                        for mode in modes:
                            output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                "ErasedPairHarness", str(variant), str(route), str(mode), str(flavor)], f"{label}-{flavor}-{variant}-{route}-{mode}")
                            if not re.fullmatch(f"variant={variant},route={route},mode={mode},flavor={flavor},trace=[0-9]+\\n", output):
                                raise RuntimeError("Original generic pair matrix changed: " + output)
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
            raise RuntimeError("Owned generic pair fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original erased-pair-invocations runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-argument-disassembly")
        if ("Lfixture/Root;.make:" not in disassembly or "Lfixture/API;.make:" not in disassembly
                or "invoke-interface" not in disassembly or "invoke-virtual" not in disassembly):
            raise RuntimeError("D8 lost the original inherited virtual/interface calls")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Generic pair recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != 8:
            raise RuntimeError("Generic pair unexpectedly raised its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if owner not in {"Lfixture/Transform;", "Lfixture/Callback;", "Lfixture/API;"}}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Generic pair recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original generic pair class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original generic pair method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Erased pair call changed an original effect or result")
        original_signature = b"<I:Ljava/lang/Object;O:Ljava/lang/Object;>(Lfixture/Transform<TI;TO;>;Lfixture/Callback<TO;>;)Lfixture/Box<TI;>;"
        for label, changed in (
                ("contravariant", original_signature.replace(b"Transform<TI;TO;>", b"Transform<TI;-TO;>")),
                ("extra-formal", original_signature.replace(b"O:Ljava/lang/Object;>", b"O:Ljava/lang/Object;U:Ljava/lang/Object;>")),
                ("unused-formal", original_signature.replace(b"TO;", b"TI;"))):
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
                raise RuntimeError("Generic pair accepted an unproved method contract: " + label)
        invalid = work / "javac-negative/fixture/Provider.java"
        invalid.parent.mkdir(parents=True)
        invalid.write_text((fixture / "fixture/Provider.java").read_text().replace("Transform<I, O> transform, Callback<O> callback)", "Transform<I, ? super O> transform, Callback<O> callback)"))
        result = subprocess.run([str(verify.javac), "--release", "8", "-proc:none", "-classpath", str(originals),
                                 "-d", str(work / "javac-negative-classes"), str(invalid)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "javac-negative.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "incompatible" not in result.stderr:
            raise RuntimeError("Independent javac accepted an inconsistent pair overload")
        # A separate implementation that calls back twice must fail the unchanged oracle.
        wrong = work / "duplicate/fixture/Provider.java"
        wrong.parent.mkdir(parents=True)
        body = (fixture / "fixture/Provider.java").read_text()
        marker = "callback.accept(decoded);"
        if body.count(marker) != 1:
            raise RuntimeError("Owned generic pair source changed")
        wrong.write_text(body.replace(marker, marker + " " + marker))
        verify.compile_local([wrong], work / "duplicate-classes", classpath=originals, release=8)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (work / "duplicate-classes", originals, harness))),
                                 "ErasedPairHarness", "0", "1", "0", "0"], text=True, capture_output=True,
                                timeout=300, env=verify.environment)
        (work / "duplicate-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle did not detect a repeated callback")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "dependency_provider": args.dependency_provider,
                    "original_metadata_and_exceptions": "unchanged", "virtual_interface_overload_bindings_and_original_erased_results": "matched",
                    "original_exception_identity": "matched", "same_code_counterexamples": "rejected",
                    "independent_repeated_callback_counterexample": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
