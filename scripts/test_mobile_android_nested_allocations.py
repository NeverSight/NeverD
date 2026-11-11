#!/usr/bin/env python3
"""Verify nested allocations, virtual calls, fluent chains and reused registers."""
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
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_protected_array_callbacks import change_method_flags


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nested-allocations-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "nested_allocations/java"
        originals, harness = work / "originals", work / "harness"
        inputs = sorted(fixture.rglob("*.java"))
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("NestedAllocationHarness", "NestedAllocationCaller")], harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 15:
            raise RuntimeError("Owned allocation-call inventory changed")
        classes = all_classes - ({"Lfixture/Provider;"} if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)

        def provider_jar(root: Path, target: Path) -> None:
            with zipfile.ZipFile(target, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("fixture/Provider.class", (root / "fixture/Provider.class").read_bytes())

        dependency = work / "owned-provider.jar"
        provider_jar(originals, dependency)
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime: tuple[Path, ...], prefix: str) -> list[str]:
            actual = []
            for mode in range(10):
                for route in range(2):
                    trace = (1 if mode == 1 else 12 if mode in (2, 7) else 1233 if mode == 3
                             else 123434 if mode == 4 else 12345345 if mode == 5 else 1234563456)
                    expected = f"mode={mode},route={route},trace={trace}"
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                         "NestedAllocationHarness", str(mode), str(route)], f"{prefix}-{mode}-{route}")
                    if output.splitlines() != [expected]:
                        raise RuntimeError("Owned nested allocation order matrix changed: " + output)
                    actual.append(output)
            for mode in (0, 2, 3, 4, 7, 8, 9):
                for route in range(2):
                    trace = 2 if mode in (2, 7) else 233 if mode == 3 else 23434
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "NestedAllocationHarness", "linear", str(mode), str(route)], f"{prefix}-linear-{mode}-{route}")
                    if output.splitlines() != [f"linear={mode},route={route},trace={trace}"]:
                        raise RuntimeError("Owned linear virtual allocation order changed: " + output)
                    actual.append(output)
            for mode in range(5):
                for route in range(2):
                    expected = f"cast={mode},route={route},trace={2 if mode in (2, 3) else 244}"
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "NestedAllocationHarness", "cast", str(mode), str(route)], f"{prefix}-cast-{mode}-{route}")
                    if output.splitlines() != [expected]:
                        raise RuntimeError("Owned allocation-before-cast order changed: " + output)
                    actual.append(output)
            output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                 "NestedAllocationHarness", "strings"], prefix + "-strings")
            if output.splitlines() != ["string-builder-chains=matched,constructions=8,ordered-failures=2"]:
                raise RuntimeError("Owned fluent allocation matrix changed: " + output)
            actual.append(output)
            output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                 "NestedAllocationHarness", "frames"], prefix + "-frames")
            if output.splitlines() != ["integer-results=72,null-receiver-failures=6"]:
                raise RuntimeError("Owned integer allocation argument matrix changed: " + output)
            actual.append(output)
            return actual

        baseline = execute((originals, harness), "baseline")

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned allocation-call fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original allocation-call runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-call-disassembly")
        witnesses = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Creator;" or not name or name[1] not in {"make", "failure", "checked", "frame", "read", "literal"}:
                continue
            ops = [op.removesuffix("/range") for op in re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])]
            if name[1] in {"read", "literal"}:
                expected = ["new-instance"] + (["const-string"] if name[1] == "literal" else []) + [
                    "invoke-virtual", "move-result-object", "invoke-direct", "return-object"]
                if ops != expected:
                    raise RuntimeError("D8 changed the single allocation/virtual argument witness")
                witnesses.add(name[1])
                continue
            if name[1] == "frame":
                if (ops.count("new-instance") != 2 or ops.count("move-result") != 1
                        or "getLineNumber" not in block[2] or "getFileName" not in block[2]):
                    raise RuntimeError("D8 changed the original integer allocation argument witness")
                file_call = block[2].index(";.getFileName:")
                line_call = block[2].index(";.getLineNumber:")
                late_literal = block[2].index('"frame" // string@')
                if not file_call < line_call < late_literal:
                    raise RuntimeError("D8 changed the late-string argument effect order")
                witnesses.add(name[1])
                continue
            if name[1] == "checked":
                if (ops.count("new-instance") != 1 or ops.count("check-cast") != 1
                        or not ops.index("new-instance") < ops.index("check-cast") < ops.index("invoke-direct")):
                    raise RuntimeError("D8 changed the original allocation-before-cast witness")
                witnesses.add(name[1])
                continue
            if ops.count("new-instance") != 2 or ops[:2] != ["new-instance", "new-instance"]:
                raise RuntimeError("D8 changed original nested allocation witnesses")
            if name[1] == "make" and not (ops.index("invoke-virtual") < ops.index("invoke-direct") < ops.index("invoke-static")):
                raise RuntimeError("D8 changed the nested constructor argument order")
            if name[1] == "failure" and ops.count("invoke-virtual") != 3:
                raise RuntimeError("D8 changed the original fluent receiver chain")
            witnesses.add(name[1])
        if witnesses != {"make", "failure", "checked", "frame", "read", "literal"}:
            raise RuntimeError("Missing original nested allocation witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Allocation-call recovery changed runtime entries or original contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Allocation-call recovery invented a runtime entry or unexpected helper")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original allocation-call class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"][method] != metadata or actual_exceptions[method] != exceptions[method]:
                    raise RuntimeError("Original allocation-call method metadata or Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Allocation-call invocation changed initialization, casts or ordered failures")
        negative = work / "private-provider-originals"
        shutil.copytree(originals, negative)
        receipt = change_method_flags(negative / "fixture/Provider.class", b"()Ljava/lang/Object;",
                                      0x1, 0x2, name=b"read")
        (work / "private-provider-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_classpath = list(classpath)
        if args.dependency_provider:
            negative_jar = work / "private-provider.jar"
            provider_jar(negative, negative_jar)
            negative_classpath[-1] = negative_jar
        negative_dex = dex(negative, work / "private-provider-dex")
        rejected = work / "private-provider-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, negative_classpath),
                                 "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "private-provider-recovery.log").write_text(result.stdout + result.stderr)
        expected_failure = "virtual invocation cannot dispatch to a private method"
        if result.returncode == 0 or rejected.exists() or expected_failure not in result.stderr:
            raise RuntimeError("Allocation-call counterexample did not fail its declared-call contract")
        hoisted = work / "hoisted/fixture/Creator.java"
        hoisted.parent.mkdir(parents=True)
        text = (fixture / "fixture/Creator.java").read_text()
        needle = "return new Outer<Object>(new Inner(provider.read()), Provider.bytes(length));"
        if text.count(needle) != 1:
            raise RuntimeError("Owned hoisted allocation counterexample no longer matches")
        hoisted.write_text(text.replace(needle, "Inner prepared = new Inner(provider.read());\n"
            "        return new Outer<Object>(prepared, Provider.bytes(length));"))
        verify.compile_local([hoisted], work / "hoisted-classes", classpath=originals)
        result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (work / "hoisted-classes", originals, harness))),
                                 "NestedAllocationHarness", "0", "0"], text=True, capture_output=True,
                                timeout=300, env=verify.environment)
        (work / "hoisted-call-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle failed to detect moving the call before allocation")
        linear_wrong = work / "linear-hoisted/fixture/Creator.java"
        linear_wrong.parent.mkdir(parents=True)
        needle = "return new Inner(provider.read());"
        if text.count(needle) != 1:
            raise RuntimeError("Owned linear allocation source changed")
        linear_wrong.write_text(text.replace(needle, "Object value = provider.read(); return new Inner(value);"))
        verify.compile_local([linear_wrong], work / "linear-hoisted-classes", classpath=originals)
        result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (work / "linear-hoisted-classes", originals, harness))),
                                 "NestedAllocationHarness", "linear", "0", "1"], text=True, capture_output=True,
                                timeout=300, env=verify.environment)
        (work / "linear-hoisted-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle did not detect a call before its original new")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "successes": 12, "failures": 28, "fresh_jvm_cases": 46, "integer_results": 72, "null_receiver_failures": 6,
                    "linear_virtual_allocation_cases": 14, "cast_successes": 8, "cast_failures": 12,
                    "exception_constructions": 8, "to_string_failures": 2,
                    "dependency_provider": args.dependency_provider,
                    "original_class_and_method_metadata_and_exceptions": "unchanged",
                    "nested_allocations_virtual_calls_and_reused_registers": "matched", "same_code_counterexample": "rejected",
                    "independent_hoisted_call_counterexample": "detected", "independent_linear_hoist_counterexample": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
