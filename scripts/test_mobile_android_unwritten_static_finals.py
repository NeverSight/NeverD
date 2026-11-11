#!/usr/bin/env python3
"""Verify unwritten static finals without constant inlining or initialization drift."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def encode_final_fields(path: Path) -> dict:
    before = path.read_bytes()
    shape = class_records(before)
    strings = shape["strings"]
    position = shape["pool_end"] + 6
    position += 2 + 2 * int.from_bytes(before[position:position + 2], "big")
    count = int.from_bytes(before[position:position + 2], "big")
    position += 2
    changed, selected = bytearray(before), []
    for _ in range(count):
        flags, name, descriptor, attrs = struct.unpack_from(">HHHH", before, position)
        if strings[name].startswith(b"zero"):
            if flags != 0x9:
                raise RuntimeError("Owned default field flags changed")
            struct.pack_into(">H", changed, position, flags | 0x10)
            selected.append(strings[name].decode() + ":" + strings[descriptor].decode())
        position += 8
        for _ in range(attrs):
            position += 6 + int.from_bytes(before[position + 2:position + 6], "big")
    if len(selected) != 12 or method_code(before) != method_code(bytes(changed)):
        raise RuntimeError("Default field encoding changed Code or original inventory")
    path.write_bytes(changed)
    return {"fields": selected, "access_before": 0x9, "access_after": 0x19,
            "Code_descriptors_Signatures_and_attributes": "unchanged"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-trace", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-unwritten-static-finals-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "unwritten_static_finals/java"
        originals, encoded, harness, fresh = (work / name for name in ("originals", "encoded", "harness", "fresh"))
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        fresh_source = FIXTURES / "harness/FreshFinalCaller.java"
        verify.compile_local([fresh_source], fresh, classpath=originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("UnwrittenFinalCaller", "UnwrittenFinalHarness")], harness,
                             classpath=os.pathsep.join(map(str, (originals, fresh))), release=8)
        original_hashes, harness_hashes, fresh_hashes = hashes(originals), hashes(harness), hashes(fresh)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        classes_all, methods_all = compiler_inventory(verify, originals)
        if len(classes_all) != 5 or len(methods_all) != 11:
            raise RuntimeError("Owned default-final inventory changed")

        def execute(runtime, final_flag, label):
            result = []
            for route in range(5):
                for mode in range(6):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "UnwrittenFinalHarness", str(route), str(mode), str(final_flag).lower()], f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Original default-final behavior matrix changed: " + output)
                    result.append(output)
            return result

        baseline = execute((originals, harness, fresh), False, "mutable-baseline")
        original_records = class_identity.compiler_classes(originals)
        shutil.copytree(originals, encoded)
        receipt = encode_final_fields(encoded / "fixture/Owner.class")
        (work / "field-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        records = class_identity.compiler_classes(encoded)
        expected = copy.deepcopy(original_records)
        for field in expected["Lfixture/Owner;"]["fields"].values():
            if field["name"].startswith("zero"):
                if field["constant_value"] is not None:
                    raise RuntimeError("Owned original default field became a constant")
                field["access"] |= 0x10
        for owner in records:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "methods", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if records[owner].get(key) != expected[owner].get(key):
                    raise RuntimeError("Final-field encoding changed another class fact: " + owner + ":" + key)
        if execute((encoded, harness, fresh), True, "encoded-baseline") != baseline:
            raise RuntimeError("Changing only field flags changed original behavior")
        encoded_hashes, exceptions = hashes(encoded), exception_inventory(encoded)
        dependencies = {"Lfixture/Trace;"} if args.dependency_trace else set()
        classes = classes_all - dependencies
        methods = {identity for identity in methods_all if identity.split("->", 1)[0] in classes}
        dependency = work / "owned-trace.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            jar.writestr("fixture/Trace.class", (encoded / "fixture/Trace.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_trace:
            classpath += ["--classpath", dependency]
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(encoded.rglob("*.class"))
                 if "L" + path.relative_to(encoded).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned default-final DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes or
                {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original default-final runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "default-field-disassembly")
        for line in disassembly.splitlines():
            if "sput" in line and "Lfixture/Owner;.zero" in line:
                raise RuntimeError("D8 introduced a write to an original default field")
        if not any("sget" in line and ".zeroI:" in line for line in disassembly.splitlines()):
            raise RuntimeError("D8 lost the original runtime default-field read")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Default-final recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != 8:
            raise RuntimeError("Default-final recovery unexpectedly raised its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=8,
                             classpath=dependency if args.dependency_trace else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Default-final recovery invented or omitted a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original default-final class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original default-final method metadata/Exceptions changed: " + method)
        rebuilt_fresh = work / "rebuilt-fresh"
        verify.compile_local([fresh_source], rebuilt_fresh, classpath=rebuilt, release=8)
        if hashes(rebuilt_fresh) != fresh_hashes:
            raise RuntimeError("A new consumer changed its runtime getstatic after recovery")
        runtime = (rebuilt, harness, rebuilt_fresh, dependency) if args.dependency_trace else (rebuilt, harness, rebuilt_fresh)
        if execute(runtime, True, "rebuilt") != baseline:
            raise RuntimeError("Default-final recovery changed an original initialization effect")
        wrong = work / "inlined/fixture/Owner.java"
        wrong.parent.mkdir(parents=True)
        source = (fixture / "fixture/Owner.java").read_text()
        pattern = r"public static (.+) (zero\w+);"
        def initialize(match):
            typ, name = match.groups()
            zero = {"boolean": "false", "byte": "0", "char": "0", "short": "0", "int": "0",
                    "long": "0L", "float": "0.0f", "double": "0.0d"}.get(typ, "null")
            return "public static final " + typ + " " + name + " = " + zero + ";"
        wrong_source, changed_count = re.subn(pattern, initialize, source)
        if changed_count != 12:
            raise RuntimeError("Owned default field declarations changed")
        wrong.write_text(wrong_source)
        wrong_classes, wrong_fresh = work / "inlined-classes", work / "inlined-fresh"
        verify.compile_local([wrong], wrong_classes, classpath=encoded, release=8)
        verify.compile_local([fresh_source], wrong_fresh, classpath=os.pathsep.join(map(str, (wrong_classes, encoded))), release=8)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong_classes, encoded, harness, wrong_fresh))),
                                 "UnwrittenFinalHarness", "4", "0", "true"], text=True, capture_output=True,
                                timeout=300, env=verify.environment)
        (work / "inlining-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle did not detect lost class initialization")
        if (hashes(originals) != original_hashes or hashes(encoded) != encoded_hashes or
                hashes(harness) != harness_hashes or hashes(fresh) != fresh_hashes or
                hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash or
                hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": 30, "attempts_per_case": 2, "dependency_trace": args.dependency_trace,
                    "encoded_final_flags_only": "12 fields; Code unchanged", "mutable_encoded_and_rebuilt_behavior": "matched",
                    "original_metadata_Exceptions_and_ConstantValue_absence": "unchanged",
                    "reentrant_defaults_class_initialization_and_failure_order": "matched",
                    "new_consumer_getstatic_bytes": "unchanged", "independent_constant_inlining_counterexample": "detected",
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
