#!/usr/bin/env python3
"""Verify ordered void and discarded-result allocation calls without helper allocations."""
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
    parser.add_argument("--instance-marker", action="store_true")
    parser.add_argument("--discarded-result", choices=("object", "integer", "wide"))
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-allocation-statements-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "allocation_statements/java"
        if args.instance_marker:
            variant_fixture = work / "instance-source"
            shutil.copytree(fixture, variant_fixture)
            marker = variant_fixture / "fixture/Marker.java"
            text = marker.read_text()
            if text.count("public static void before(") != 1 or text.count("public class Marker {") != 1:
                raise RuntimeError("Original instance-marker source anchors changed")
            marker.write_text(text.replace("public static void before(", "public void before(")
                .replace("public class Marker {", "public class Marker {\n    public static final Marker INSTANCE = new Marker();"))
            creator = variant_fixture / "fixture/Creator.java"
            text = creator.read_text()
            if text.count("Marker.before(") != 1:
                raise RuntimeError("Original instance-marker caller changed")
            creator.write_text(text.replace("Marker.before(", "Marker.INSTANCE.before("))
            fixture = variant_fixture
        originals, harness = work / "originals", work / "harness"
        inputs = sorted(fixture.rglob("*.java"))
        result_descriptor = "V"
        if args.discarded_result:
            result_type, result_expression, result_descriptor = {
                "object": ("Object", "Trace.failure", "Ljava/lang/Object;"),
                "integer": ("int", "Integer.MIN_VALUE", "I"),
                "wide": ("long", "Long.MIN_VALUE", "J"),
            }[args.discarded_result]
            marker_source = fixture / "fixture/Marker.java"
            text = marker_source.read_text()
            declaration = "public " + ("" if args.instance_marker else "static ") + "void before("
            if text.count(declaration) != 1 or text.count("    }\n}") != 1:
                raise RuntimeError("Original discarded-call source anchors changed")
            variant = work / "original-source/fixture/Marker.java"
            variant.parent.mkdir(parents=True)
            variant.write_text(text.replace(declaration, declaration.replace("void", result_type))
                .replace("    }\n}", "        return " + result_expression + ";\n    }\n}"))
            inputs = [variant if path == marker_source else path for path in inputs]
        verify.compile_local(inputs, originals, release=14)
        harness_inputs = [FIXTURES / "harness" / (name + ".java") for name in
                          ("AllocationStatementHarness", "AllocationStatementCaller")]
        if args.instance_marker:
            caller = work / "instance-caller/AllocationStatementCaller.java"
            caller.parent.mkdir()
            text = harness_inputs[1].read_text()
            if text.count("Marker.before(") != 1:
                raise RuntimeError("Original frozen instance caller changed")
            caller.open("x").write(text.replace("Marker.before(", "Marker.INSTANCE.before("))
            harness_inputs[1] = caller
        verify.compile_local(harness_inputs, harness, classpath=originals, release=14)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 10:
            raise RuntimeError("Owned allocation statement inventory changed")
        classes = all_classes - ({"Lfixture/Marker;"} if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)

        def provider_jar(root: Path, target: Path) -> None:
            with zipfile.ZipFile(target, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("fixture/Marker.class", (root / "fixture/Marker.class").read_bytes())

        dependency = work / "owned-provider.jar"
        provider_jar(originals, dependency)
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime: tuple[Path, ...], prefix: str) -> list[str]:
            actual = []
            for mode in range(5):
                for route in range(4):
                    trace = 1 if mode == 1 else 12 if mode == 2 else 1233 if mode == 3 else 123434
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "AllocationStatementHarness", str(mode), str(route)], f"{prefix}-{mode}-{route}")
                    if output.splitlines() != [f"mode={mode},route={route},trace={trace}"]:
                        raise RuntimeError("Original allocation statement order changed: " + output)
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
                raise RuntimeError("Owned allocation statement fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        (work / "frozen-dex-hashes.json").open("x").write(json.dumps({str(source_dex): hashlib.sha256(source_dex.read_bytes()).hexdigest()}, indent=2) + "\n")
        (work / "baseline-cli-sha256.txt").open("x").write(hashlib.sha256(args.neverd.read_bytes()).hexdigest() + "\n")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original allocation statement runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-call-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Creator;" or not name or name[1] != "make":
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            call_opcode = "invoke-virtual" if args.instance_marker else "invoke-static"
            if (ops.count("new-instance") != 1 or ops.count(call_opcode) != 1 or ops.count("const-class") != 1
                    or not ops.index("new-instance") < ops.index(call_opcode) < ops.index("const-class") < ops.index("invoke-direct")
                    or any(op.startswith(("if-", "goto")) or "switch" in op for op in ops)):
                raise RuntimeError("D8 changed original allocation/void-call/class-literal order")
            if args.discarded_result and any(op.startswith("move-result") for op in ops):
                raise RuntimeError("D8 consumed a declared result that the original caller discarded")
            witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original allocation statement witness")
        if args.instance_marker:
            original_creator = (fixture / "fixture/Creator.java").read_text()
            statement = 'Marker.INSTANCE.before(Integer.MIN_VALUE, "owned-\\u03bb");'
            if original_creator.count(statement) != 1:
                raise RuntimeError("Original instance statement witness changed")
            wrong_programs = {
                "lost-call": original_creator.replace(statement, ""),
                "duplicate-call": original_creator.replace(statement, statement + statement),
                "null-receiver": original_creator.replace("Marker.INSTANCE.before(", "((Marker)null).before("),
                "extra-input-check": original_creator.replace(statement, "payload.toString();" + statement),
                "hoisted-call": original_creator.replace(statement, "").replace("return new Target(", statement + "return new Target("),
            }
            for label, text in wrong_programs.items():
                file = work / label / "fixture/Creator.java"
                file.parent.mkdir(parents=True)
                file.open("x").write(text)
                compiled = work / (label + "-classes")
                verify.compile_local([file], compiled, classpath=originals, release=14)
                result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                    "AllocationStatementHarness", "0", "0"], text=True, capture_output=True, timeout=300, env=verify.environment)
                (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
                if result.returncode == 0 or "allocation statement order" not in result.stderr:
                    raise RuntimeError("Frozen observer missed wrong instance void statement: " + label)
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Allocation statement recovery changed runtime entries or original contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Allocation statement recovery invented a runtime entry or unexpected helper")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original allocation statement class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"][method] != metadata or actual_exceptions[method] != exceptions[method]:
                    raise RuntimeError("Original allocation statement method metadata or Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Allocation statement invocation changed initialization cycles or ordered failures")
        negative = work / "private-provider-originals"
        shutil.copytree(originals, negative)
        receipt = change_method_flags(negative / "fixture/Marker.class", ("(ILjava/lang/String;)" + result_descriptor).encode(),
                                      0x1 if args.instance_marker else 0x9,
                                      0x2 if args.instance_marker else 0xa, name=b"before")
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
        if (result.returncode == 0 or rejected.exists()
                or not any(message in result.stderr for message in (
                    "nested allocation requires an unchanged declared",
                    *(["virtual invocation cannot dispatch to a private method"] if args.instance_marker else [])))):
            raise RuntimeError("Allocation statement counterexample did not fail its declared-call contract")
        hoisted = work / "hoisted/fixture/Creator.java"
        hoisted.parent.mkdir(parents=True)
        text = (fixture / "fixture/Creator.java").read_text()
        marker = ('Marker.INSTANCE' if args.instance_marker else 'Marker') + '.before(Integer.MIN_VALUE, "owned-\\u03bb");'
        if text.count(marker) != 1:
            raise RuntimeError("Original statement hoist witness changed")
        hoisted.write_text(text.replace(marker, "").replace("return new Target(", marker + " return new Target("))
        verify.compile_local([hoisted], work / "hoisted-classes", classpath=originals, release=14)
        result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (work / "hoisted-classes", originals, harness))),
                                 "AllocationStatementHarness", "0", "0"], text=True, capture_output=True,
                                timeout=300, env=verify.environment)
        (work / "hoisted-statement-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle failed to detect statements before allocation")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": 20, "successful_constructions": 8, "ordered_failures": 32,
                    "dependency_provider": args.dependency_provider,
                    "instance_marker": args.instance_marker,
                    "discarded_result": args.discarded_result,
                    "original_class_and_method_metadata_and_exceptions": "unchanged",
                    "allocation_call_and_class_literal_order": "matched", "same_code_counterexample": "rejected",
                    "independent_hoisted_statement_counterexample": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
