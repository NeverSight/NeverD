#!/usr/bin/env python3
"""Verify ordered interface field initialization against immutable JVM originals."""
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-interface-initializers-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "interface_initializers/java"
        originals, harness = work / "originals", work / "harness"
        inputs = sorted(fixture.rglob("*.java"))
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("InterfaceInitializerHarness", "InterfaceInitializerCaller")], harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 12:
            raise RuntimeError("Owned interface initializer inventory changed")
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
            for mode in range(5):
                for route in range(4):
                    trace = 1 if mode == 1 else 12 if mode == 2 else 123 if mode == 3 else 1234
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "InterfaceInitializerHarness", str(mode), str(route)], f"{prefix}-{mode}-{route}")
                    if output.splitlines() != [f"mode={mode},route={route},trace={trace}"]:
                        raise RuntimeError("Original interface field initialization order changed: " + output)
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
                raise RuntimeError("Owned interface initializer fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original interface initializer runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-call-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Api;" or not name or name[1] != "<clinit>":
                continue
            operations = [op for op in re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])]
            if (operations.count("invoke-virtual") != 2 or operations.count("invoke-static") != 2
                    or operations.count("sget-object") != 1 or operations.count("sput-object") != 3):
                raise RuntimeError("D8 changed original interface initializer operations")
            writes = re.findall(r"sput-object[^\n]*;\.([^:]+):", block[2])
            if writes != ["Z_FIRST", "A_SECOND", "COPY"]:
                raise RuntimeError("D8 changed original interface field write order")
            witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original interface initializer witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Interface initializer recovery changed runtime entries or original contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes if owner != "Lfixture/Api;"}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Interface initializer recovery invented a runtime entry or unexpected helper")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original interface initializer class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"][method] != metadata or actual_exceptions[method] != exceptions[method]:
                    raise RuntimeError("Original interface initializer method metadata or Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Interface initializer invocation changed initialization cycles or ordered failures")
        negative = work / "private-provider-originals"
        shutil.copytree(originals, negative)
        receipt = change_method_flags(negative / "fixture/Provider.class", b"(Ljava/lang/String;)Ljava/lang/String;",
                                      0x9, 0xa, name=b"first")
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
                or "interface initializer requires an unchanged declared" not in result.stderr):
            raise RuntimeError("Interface initializer counterexample did not fail its declared-call contract")
        reordered = work / "reordered/fixture/Api.java"
        reordered.parent.mkdir(parents=True)
        lines = (fixture / "fixture/Api.java").read_text().splitlines()
        first = next(i for i, line in enumerate(lines) if "String Z_FIRST =" in line)
        second = next(i for i, line in enumerate(lines) if "String A_SECOND =" in line)
        lines[first], lines[second] = lines[second], lines[first]
        reordered.write_text("\n".join(lines) + "\n")
        verify.compile_local([reordered], work / "reordered-classes", classpath=originals)
        result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (work / "reordered-classes", originals, harness))),
                                 "InterfaceInitializerHarness", "0", "0"], text=True, capture_output=True,
                                timeout=300, env=verify.environment)
        (work / "reordered-field-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle failed to detect reordered interface fields")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": 20, "successful_reads": 24, "initialization_failures": 16,
                    "erroneous_class_reads": 80, "dependency_provider": args.dependency_provider,
                    "original_class_and_method_metadata_and_exceptions": "unchanged",
                    "ordered_interface_initialization_and_cycles": "matched", "same_code_counterexample": "rejected",
                    "independent_reordered_fields_counterexample": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
