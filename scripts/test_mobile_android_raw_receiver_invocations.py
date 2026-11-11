#!/usr/bin/env python3
"""Verify generic instance methods through exact raw class and interface receivers."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
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

PROVIDERS = {"Lfixture/Provider;", "Lfixture/Api;", "Lfixture/Box;"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--dependent-bounds", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-raw-receiver-invocations-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "raw_receiver_invocations/java"
        if args.dependent_bounds:
            authored = work / "dependent-source"
            shutil.copytree(fixture, authored)
            for owner in ("Provider", "Api"):
                path = authored / ("fixture/" + owner + ".java")
                text = path.read_text()
                if text.count("<U> Box<? extends U>") != 1:
                    raise RuntimeError("Original raw receiver method formal changed")
                path.write_text(text.replace("<U> Box<? extends U>", "<U extends T> Box<? extends U>"))
            fixture = authored
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        harness_sources = [FIXTURES / "harness" / (name + ".java") for name in
                           ("RawReceiverHarness", "RawReceiverCaller")]
        if args.dependent_bounds:
            oracle = work / "dependent-harness"
            oracle.mkdir()
            for path in harness_sources:
                text = path.read_text()
                if path.name == "RawReceiverHarness.java":
                    anchor = "        int calls = 0, failures = 0, tokens = 0;"
                    if text.count(anchor) != 1:
                        raise RuntimeError("Raw receiver reflection oracle anchor changed")
                    text = text.replace(anchor, """        for (Class<?> owner : new Class<?>[] {Provider.class, Api.class}) {
            Method method = owner.getMethod("read", Box.class, Object.class, long.class, double.class);
            TypeVariable<?>[] formals = method.getTypeParameters();
            require(formals.length == 1 && formals[0].getBounds().length == 1);
            require(formals[0].getBounds()[0].equals(owner.getTypeParameters()[0]));
            ParameterizedType input = (ParameterizedType) method.getGenericParameterTypes()[0];
            require(input.getActualTypeArguments()[0].equals(formals[0]));
            ParameterizedType result = (ParameterizedType) method.getGenericReturnType();
            WildcardType slot = (WildcardType) result.getActualTypeArguments()[0];
            require(slot.getLowerBounds().length == 0 && slot.getUpperBounds().length == 1);
            require(slot.getUpperBounds()[0].equals(formals[0]));
        }
""" + anchor)
                (oracle / path.name).write_text(text)
            harness_sources = sorted(oracle.glob("*.java"))
        verify.compile_local(harness_sources, harness, classpath=originals)
        bound_signature = b"<U:TT;>(Lfixture/Box<TU;>;TT;JD)Lfixture/Box<+TU;>;"
        if args.dependent_bounds:
            for owner in ("Provider", "Api"):
                receipt = change_signature(originals / ("fixture/" + owner + ".class"),
                                           bound_signature, bound_signature.replace(b"U:TT;", b"U::TT;"))
                (work / (owner + "-bound-position.json")).write_text(json.dumps(receipt, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 10:
            raise RuntimeError("Owned raw receiver inventory changed")
        classes = all_classes - (PROVIDERS if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)

        def provider_jar(root: Path, target: Path) -> None:
            with zipfile.ZipFile(target, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                for owner in sorted(PROVIDERS):
                    name = owner[1:-1] + ".class"
                    archive.writestr(name, (root / name).read_bytes())

        dependency = work / "owned-provider.jar"
        provider_jar(originals, dependency)
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                              "RawReceiverHarness"], "baseline")
        if baseline.splitlines() != ["calls=224,tokens=64,failures=192",
                "raw-receiver=exact,dispatch-and-identities=matched,wide-bits-and-ordered-failures=retained"]:
            raise RuntimeError("Owned raw-receiver matrix changed: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned raw-receiver fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original raw-receiver runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0)
                or coverage.get("class_source_bindings")):
            raise RuntimeError("Raw-receiver recovery changed runtime entries or original contracts")
        projections = coverage.get("generic_signature_projections", [])
        expected_projections = {identity for identity in methods
                                if args.dependent_bounds and "->read(" in identity}
        if ({row["declaration"] for row in projections} != expected_projections
                or len(projections) != len(expected_projections)
                or any(len(row["changes"]) != 1
                       or row["changes"][0]["kind"] != "type-variable-bound-position"
                       for row in projections)
                or not projections and coverage["status"] != "recovered"):
            raise RuntimeError("Raw-receiver bound position changes were not reported exactly")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        helper_owners = {"Caller", "Trace"} | (set() if args.dependency_provider else {"Provider", "Box"})
        expected_helpers = {"Lfixture/" + name + ";->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for name in helper_owners}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Raw-receiver recovery invented a runtime entry or unexpected helper")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original raw-receiver class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if args.dependent_bounds and method in expected_projections:
                    wanted["signature"] = wanted["signature"].replace("<U::TT;>", "<U:TT;>")
                if after["methods"][method] != wanted or actual_exceptions[method] != exceptions[method]:
                    raise RuntimeError("Original raw-receiver method metadata or Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             "RawReceiverHarness"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Raw-receiver invocation changed identity, ordered failures or virtual dispatch")
        negative = work / "bad-bound-originals"
        shutil.copytree(originals, negative)
        original_signature = b"<T:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Api<TT;>;"
        changed_signature = b"<T:Ljava/lang/Number;>Ljava/lang/Object;Lfixture/Api<TT;>;"
        receipt = change_signature(negative / "fixture/Provider.class", original_signature, changed_signature)
        (work / "bad-bound-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_classpath = list(classpath)
        if args.dependency_provider:
            negative_jar = work / "bad-bound-provider.jar"
            provider_jar(negative, negative_jar)
            negative_classpath[-1] = negative_jar
        negative_dex = dex(negative, work / "bad-bound-dex")
        rejected = work / "bad-bound-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, negative_classpath),
                                 "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "bad-bound-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Same-Code generic bound/descriptor disagreement was accepted")
        if args.dependent_bounds:
            wrong = work / "wrong-method-bound"
            shutil.copytree(originals, wrong)
            receipt = change_signature(wrong / "fixture/Provider.class",
                                       bound_signature.replace(b"U:TT;", b"U::TT;"),
                                       bound_signature.replace(b"U:TT;", b"U:Ljava/lang/Object;"))
            (work / "wrong-method-bound-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, harness))),
                                     "RawReceiverHarness"], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "wrong-method-bound-counterexample.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "raw receiver contract" not in result.stderr:
                raise RuntimeError("Frozen reflection oracle did not detect the changed method bound")
        for label, before, after in (
            ("changed-receiver", "provider.read(input, tag, bits, value)", "new Provider<Object>().read(input, tag, bits, value)"),
            ("lost-high-word", "provider.read(input, tag, bits, value)", "provider.read(input, tag, (int) bits, value)"),
            ("skipped-platform-check", "candidate.asSubclass(parent)", "candidate"),
        ):
            wrong = work / label / "fixture/Caller.java"
            wrong.parent.mkdir(parents=True)
            text = (fixture / "fixture/Caller.java").read_text()
            if text.count(before) != (1 if label == "skipped-platform-check" else 2):
                raise RuntimeError("Independent raw receiver counterexample anchor changed")
            wrong.write_text(text.replace(before, after))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     "RawReceiverHarness"], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "raw receiver contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "calls": 224, "platform_class_calls": 64, "expected_failures": 192,
                    "dependency_provider": args.dependency_provider,
                    "dependent_bounds": args.dependent_bounds,
                    "original_metadata_and_exceptions": "reported bound positions only" if args.dependent_bounds else "unchanged",
                    "runtime_inventory": "original entries and reported helpers only",
                    "raw_receiver_virtual_and_interface_dispatch": "matched",
                    "mutable_identity_wide_bits_and_ordered_failures": "matched", "platform_class_checks": "retained",
                    "same_code_bound_counterexample": "rejected", "independent_receiver_word_and_platform_counterexamples": "detected",
                    "method_bound_reflection_counterexample": "detected" if args.dependent_bounds else "not applicable",
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
