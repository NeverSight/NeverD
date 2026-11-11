#!/usr/bin/env python3
"""Verify seeded generic carriers with primitive varargs and a fixed nominal result."""
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


PROVIDERS = {"Lfixture/Provider;", "Lfixture/Facade;"}
FACTORY = "Lfixture/Provider;->make(Ljava/lang/Object;Lfixture/Carrier;[F)Lfixture/FixedResult;"
FACTORY_SIGNATURE = "<T:Ljava/lang/Object;>(TT;Lfixture/Carrier<TT;Ljava/lang/Float;>;[F)Lfixture/FixedResult;"
RESULT_SIGNATURE = "Lfixture/ResultBase<Ljava/lang/String;>;"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--private-helper", action="store_true")
    parser.add_argument("--dependency-carrier", action="store_true")
    args = parser.parse_args()
    if args.dependency_provider and (args.private_helper or args.dependency_carrier):
        parser.error("Private-helper/carrier profiles keep the helper owner in the input")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-seeded-nominal-results-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "seeded_nominal_results/java"
        if args.private_helper:
            authored = work / "authored-java"
            shutil.copytree(fixture, authored)
            provider = authored / "fixture/Provider.java"
            text = provider.read_text()
            marker = "    public static <T> FixedResult make(T seed, Carrier<T, Float> callback, float... values) {"
            if text.count(marker) != 1:
                raise RuntimeError("Private fixed-result helper source anchor changed")
            wrapper = (marker + "\n        return classify(seed, callback, values);\n    }\n"
                       "    private static final <T> FixedResult classify(T seed, Carrier<T, Float> callback, float[] values) {")
            provider.write_text(text.replace(marker, wrapper))
            fixture = authored
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("SeededNominalResultCaller", "SeededNominalResultCallback",
                               "SeededNominalResultRawCallback", "SeededNominalResultHarness")],
                             harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (7, 13 if args.private_helper else 12):
            raise RuntimeError("Owned seeded nominal result inventory changed: " + repr((len(all_classes), len(all_methods))))
        providers = {"Lfixture/Carrier;"} if args.dependency_carrier else PROVIDERS
        has_dependency = args.dependency_provider or args.dependency_carrier
        classes = all_classes - (providers if has_dependency else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        factory = records["Lfixture/Provider;"]["methods"][FACTORY]
        if (factory["signature"] != FACTORY_SIGNATURE or factory["access"] != 0x89
                or records["Lfixture/FixedResult;"]["signature"] != RESULT_SIGNATURE
                or any(metadata["access"] & 0x40 for owner in records.values() for metadata in owner["methods"].values())):
            raise RuntimeError("Original seeded fixed-result Signature, varargs or bridge inventory changed")
        if args.private_helper:
            helper = FACTORY.replace("->make(", "->classify(")
            metadata = records["Lfixture/Provider;"]["methods"][helper]
            if metadata["signature"] != FACTORY_SIGNATURE or metadata["access"] != 0x1a:
                raise RuntimeError("Original private static final helper contract changed")
        frozen_records = class_identity.compiler_classes(harness)
        frozen_bridges = {identity for owner in frozen_records.values() for identity, metadata in owner["methods"].items()
                          if metadata["access"] & 0x40}
        bridge = "LSeededNominalResultCallback;->apply(Ljava/lang/Object;)Ljava/lang/Object;"
        if (frozen_bridges != {bridge}
                or frozen_records["LSeededNominalResultCallback;"]["methods"][bridge]["access"] != 0x1041):
            raise RuntimeError("Frozen callback lost its original checked bridge")
        (work / "original-metadata.json").write_text(json.dumps(
            {"classes": records, "exceptions": exceptions, "frozen_bridge_inventory": sorted(frozen_bridges)}, indent=2) + "\n")

        def provider_jar(root, destination):
            with zipfile.ZipFile(destination, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                for owner in sorted(providers):
                    relative = owner[1:-1] + ".class"
                    archive.writestr(relative, (root / relative).read_bytes())

        dependency = work / "owned-provider.jar"
        provider_jar(originals, dependency)
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if has_dependency:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            outputs = []
            for route in range(4):
                for mode in range(15):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "SeededNominalResultHarness", str(route), str(mode)], f"{label}-{route}-{mode}")
                    lines = output.splitlines()
                    if len(lines) != 2 or any(not re.fullmatch(
                            f"route={route},mode={mode},attempt={attempt},trace=[0-9]+", line)
                            for attempt, line in enumerate(lines)):
                        raise RuntimeError("Seeded fixed-result behavior matrix changed: " + output)
                    outputs.append(output)
            return outputs

        def dex(root, label, dependencies):
            destination = work / label
            destination.mkdir()
            inputs = [path for path in sorted(root.rglob("*.class"))
                      if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *dependencies,
                        "--output", destination, *inputs], "d8-" + label)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned seeded fixed-result fixture changed DEX inventory")
            return result

        baseline = execute((originals, harness), "baseline")
        source_dex = dex(originals, "dex", classpath)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods
                or any(row["access_flags"] & 0x40 for row in inventory["methods"])):
            raise RuntimeError("D8 changed an original seeded fixed-result runtime entry")
        if not args.dependency_provider:
            actual_factory = next(row for row in inventory["methods"] if row["identity"] == FACTORY)
            if actual_factory["access_flags"] != 0x89:
                raise RuntimeError("D8 changed the original public static varargs declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-argument-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Client;" or not name or name[1] != "call":
                continue
            ops = [(op, tail) for op, tail in re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])]
            calls = [tail for op, tail in ops if op in {"invoke-static", "invoke-static/range"}
                     and "Lfixture/Facade;.make:(Ljava/lang/Object;Lfixture/Carrier;[F)Lfixture/FixedResult;" in tail]
            if len(calls) != 1 or any(op in {"new-array", "filled-new-array", "filled-new-array/range"} for op, tail in ops):
                raise RuntimeError("D8 lost the inherited fixed-result call or original array carrier")
            witnessed = True
        if not witnessed:
            raise RuntimeError("D8 lost the original Client call witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Seeded fixed-result recovery changed original inventory or contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=8,
                             classpath=dependency if has_dependency else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if owner != "Lfixture/Carrier;"}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Seeded fixed-result recovery changed runtime entries or helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original seeded fixed-result class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original seeded fixed-result method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if has_dependency else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Seeded fixed-result invocation changed identity, array bits or ordered failures")

        negatives = (
            ("missing-seed-formal", "fixture/Provider.class", FACTORY_SIGNATURE,
             FACTORY_SIGNATURE.replace("(TT;", "(Ljava/lang/Object;")),
            ("extra-method-formal", "fixture/Provider.class", FACTORY_SIGNATURE,
             FACTORY_SIGNATURE.replace("T:Ljava/lang/Object;>", "T:Ljava/lang/Object;U:Ljava/lang/Object;>")),
            ("result-own-formal", "fixture/FixedResult.class", RESULT_SIGNATURE,
             "<E:Ljava/lang/Object;>" + RESULT_SIGNATURE),
        )
        for label, relative, before, after in negatives:
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / relative, before.encode(), after.encode())
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_classpath = list(classpath)
            if has_dependency:
                negative_jar = work / (label + "-provider.jar")
                provider_jar(negative, negative_jar)
                negative_classpath[-1] = negative_jar
            negative_dex = dex(negative, label + "-dex", negative_classpath)
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, negative_classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic invocation requires" not in result.stderr:
                raise RuntimeError("Seeded fixed-result call accepted an unproved contract: " + label)

        controls = (
            ("duplicate-callback", "Provider", "Float result = callback.apply(seed);",
             "callback.apply(seed); Float result = callback.apply(seed);", 0),
            ("copied-array", "Provider", "Trace.values = values;",
             "Trace.values = values == null ? null : values.clone();", 0),
            ("wrong-result", "Provider", "return Trace.result;",
             "return new FixedResult(new Object());", 0),
            ("eager-seed-cast", "Provider", "Trace.values = values;",
             "int[] eager = (int[])(Object)seed; Trace.values = values;", 6),
            ("changed-failure", "Trace", "throw failure;", "throw new RuntimeException(failure);", 2),
            ("lost-negative-zero", "Provider", "Trace.values = values;",
             "values[0] = 0.0f; Trace.values = values;", 0),
        )
        for label, owner, marker, replacement, mode in controls:
            source = (fixture / "fixture" / (owner + ".java")).read_text()
            if source.count(marker) != 1:
                raise RuntimeError("Independent seeded fixed-result counterexample anchor changed: " + label)
            wrong = work / label / "fixture" / (owner + ".java")
            wrong.parent.mkdir(parents=True)
            wrong.write_text(source.replace(marker, replacement))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     "SeededNominalResultHarness", "1", str(mode)], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "seeded nominal result contract" not in result.stderr:
                raise RuntimeError("Independent oracle missed a seeded fixed-result mutation: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": 60, "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "private_helper": args.private_helper, "dependency_carrier": args.dependency_carrier,
                    "frozen_checked_bridges": 1, "original_metadata_varargs_and_exceptions": "unchanged",
                    "fixed_result_and_token_identity": "matched", "array_identity_signed_zero_and_nan_payload": "matched",
                    "initialization_and_late_callee_failures": "matched", "original_exception_identity": "matched",
                    "same_code_counterexamples": len(negatives), "independent_behavior_counterexamples": len(controls),
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
