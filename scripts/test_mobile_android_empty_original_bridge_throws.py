#!/usr/bin/env python3
"""Verify originally exception-free bridges against closed checked ancestor contracts."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
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
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code

BRIDGE_DESCRIPTOR = b"(Ljava/lang/Object;[I)Lfixture/Box;"
BRIDGE = "Lfixture/Core;->visit" + BRIDGE_DESCRIPTOR.decode()
TYPED_DESCRIPTOR = b"(Lfixture/Box;[I)Lfixture/Box;"
EXCEPTIONS = ["Lfixture/First;", "Lfixture/Second;"]


def hashes(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


def omit_original_exceptions(path: Path, *, method_name: bytes = b"visit",
                             typed_descriptor: bytes = TYPED_DESCRIPTOR,
                             bridge_descriptor: bytes = BRIDGE_DESCRIPTOR,
                             expected_exceptions: list[str] = EXCEPTIONS) -> dict:
    """Remove two selected Exceptions attributes, preserving every Code byte and CP entry."""
    before = path.read_bytes()
    shape = class_records(before)
    strings = shape["strings"]
    position = shape["pool_end"] + 6
    position += 2 + 2 * int.from_bytes(before[position:position + 2], "big")

    def attributes(offset: int, count: int) -> tuple:
        rows = []
        for _ in range(count):
            name, size = struct.unpack_from(">HI", before, offset)
            end = offset + 6 + size
            rows.append((strings[name], offset, end))
            offset = end
        return offset, rows

    fields = int.from_bytes(before[position:position + 2], "big")
    position += 2
    for _ in range(fields):
        position, _ = attributes(position + 8, int.from_bytes(before[position + 6:position + 8], "big"))
    methods = int.from_bytes(before[position:position + 2], "big")
    position += 2
    patches, selected = [], []
    for _ in range(methods):
        flags, name, descriptor, count = struct.unpack_from(">HHHH", before, position)
        end, rows = attributes(position + 8, count)
        if strings[name] == method_name and strings[descriptor] in (typed_descriptor, bridge_descriptor):
            expected_flags = 0x1041 if strings[descriptor] == bridge_descriptor else 1
            if flags != expected_flags:
                raise RuntimeError("Owned original bridge or typed flags changed")
            exceptions = [(start, stop) for key, start, stop in rows if key == b"Exceptions"]
            if len(exceptions) != 1:
                raise RuntimeError("Missing exact owned Exceptions attribute")
            start, stop = exceptions[0]
            payload = before[start + 6:stop]
            if int.from_bytes(payload[:2], "big") != len(expected_exceptions):
                raise RuntimeError("Owned ordered checked exceptions changed")
            declared = ["L" + strings[shape["classes"][int.from_bytes(payload[i:i + 2], "big")]].decode() + ";"
                        for i in range(2, len(payload), 2)]
            if declared != expected_exceptions:
                raise RuntimeError("Owned checked exception order changed")
            patches.extend(((position + 6, position + 8, (count - 1).to_bytes(2, "big")), (start, stop, b"")))
            selected.append(strings[descriptor].decode())
        position = end
    if sorted(selected) != sorted((typed_descriptor.decode(), bridge_descriptor.decode())):
        raise RuntimeError("Missing the two original owned exception declarations")
    changed = before
    for start, stop, replacement in sorted(patches, reverse=True):
        changed = changed[:start] + replacement + changed[stop:]
    if method_code(before) != method_code(changed) or changed[:shape["pool_end"]] != before[:shape["pool_end"]]:
        raise RuntimeError("Exceptions encoding changed original Code, descriptors or constant pool")
    class_records(changed)
    path.write_bytes(changed)
    return {"methods": selected, "original_declared_throws": expected_exceptions, "encoded_declared_throws": [],
            "all_Code_descriptors_and_constant_pool": "unchanged"}


def exception_inventory(root: Path) -> dict:
    result = {}
    for path in sorted(root.rglob("*.class")):
        data = path.read_bytes()
        shape = class_records(data)
        strings = shape["strings"]
        owner = "L" + strings[shape["classes"][shape["this_class"]]].decode() + ";"
        position = shape["pool_end"] + 6
        position += 2 + 2 * int.from_bytes(data[position:position + 2], "big")
        for fields in (True, False):
            count = int.from_bytes(data[position:position + 2], "big")
            position += 2
            for _ in range(count):
                _, name, descriptor, attributes = struct.unpack_from(">HHHH", data, position)
                position += 8
                exceptions = None
                for _ in range(attributes):
                    key, length = struct.unpack_from(">HI", data, position)
                    end = position + 6 + length
                    if strings[key] == b"Exceptions":
                        payload = data[position + 6:end]
                        number = int.from_bytes(payload[:2], "big")
                        if fields or exceptions is not None or len(payload) != 2 + 2 * number:
                            raise RuntimeError("Malformed owned compiler Exceptions attribute")
                        exceptions = ["L" + strings[shape["classes"][int.from_bytes(payload[i:i + 2], "big")]].decode() + ";"
                                      for i in range(2, len(payload), 2)]
                    position = end
                if not fields:
                    identity = owner + "->" + strings[name].decode() + strings[descriptor].decode()
                    result[identity] = exceptions or []
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parent", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-empty-original-bridge-throws-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "empty_original_bridge_throws/java/fixture"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/EmptyOriginalBridgeThrowsHarness.java"], harness, classpath=originals)
        receipt = omit_original_exceptions(originals / "fixture/Core.class")
        (work / "original-exceptions-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        declarations = {"Lfixture/Api;", "Lfixture/Parent;"}
        classes = all_classes - (declarations if args.dependency_parent else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        original_exceptions = exception_inventory(originals)
        dependency = work / "owned-parents.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Api", "Parent"):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_parent:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "EmptyOriginalBridgeThrowsHarness", "original"], "baseline")
        lines = baseline.splitlines()
        match = re.fullmatch(r"values=(\d+),identities=(\d+),states=(\d+),failures=(\d+)", lines[0]) if len(lines) == 2 else None
        counts = tuple(map(int, match.groups())) if match else ()
        if counts != (48, 576, 324, 72) or lines[1] != "original-empty-target-throws=retained,bridge-exception-difference=reported,super-and-erased-dispatch=retained":
            raise RuntimeError("Owned exception-free bridge case matrix changed: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned exception-free bridge fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original exception-free bridge runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 1 or projected != {BRIDGE}
                or coverage["status"] != "partial" or coverage.get("generic_signature_projections")
                or coverage.get("class_source_bindings")):
            raise RuntimeError("Exception-free bridge changed inventory or unrelated source signatures")
        if (rows[BRIDGE].get("source_bridge_declared_throws") != EXCEPTIONS
                or rows[BRIDGE].get("bridge_exception_source") not in {
                    "Lfixture/Api;->visit" + BRIDGE_DESCRIPTOR.decode(), "Lfixture/Parent;->visit" + BRIDGE_DESCRIPTOR.decode()}
                or not rows[BRIDGE].get("metadata_differences")):
            raise RuntimeError("Original empty bridge lacks its explicit ordered Exceptions difference")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_parent else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {entry["class"] + "->" + entry["name"] + entry["prototype"]
                   for entry in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != methods | helpers:
            raise RuntimeError("Rebuilt exception-free bridge invented or renamed an original runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        rebuilt_exceptions = exception_inventory(rebuilt)
        for method in methods:
            wanted = EXCEPTIONS if method == BRIDGE else original_exceptions[method]
            if rebuilt_exceptions.get(method) != wanted:
                raise RuntimeError("Original Exceptions changed outside the reported bridge: " + method)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original class or field metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"][method] != metadata:
                    raise RuntimeError("Original method metadata changed outside the reported Exceptions attribute: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_parent else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             "EmptyOriginalBridgeThrowsHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original super call, checked failure identity, bridge cast or state order changed")
        for label in ("missing-synthetic", "wrong-parent-argument"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "missing-synthetic":
                receipt = change_method_flags(negative / "fixture/Core.class", BRIDGE_DESCRIPTOR, 0x1041, 0x41, name=b"visit")
            else:
                receipt = change_signature(negative / "fixture/Core.class", b"Lfixture/Parent<Lfixture/Box;>;",
                                           b"Lfixture/Parent<Ljava/lang/String;>;")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Incomplete original bridge proof was admitted: " + label)
        invalid = work / "wrong-parent-java/fixture/Core.java"
        invalid.parent.mkdir(parents=True)
        content = (fixture / "Core.java").read_text()
        if content.count("extends Parent<Box>") != 1:
            raise RuntimeError("Missing owned parent counterexample")
        invalid.write_text(content.replace("extends Parent<Box>", "extends Parent<String>", 1))
        expected_parent = invalid.with_name("ExpectedParent.java")
        expected_parent.write_text("package fixture; class ExpectedParent { Parent<Box> expected(Core value) { return value; } }\n")
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-cp", str(originals), "-d",
                                   str(work / "wrong-parent-compiler"), str(invalid), str(expected_parent)], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-parent-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or "incompatible types" not in compiler.stderr:
            raise RuntimeError("Independent javac accepted an incompatible complete parent binding")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 1, "value_cases": counts[0], "identity_cases": counts[1],
                    "state_cases": counts[2], "failure_cases": counts[3],
                    "profile": "dependency-parent" if args.dependency_parent else "source-parent",
                    "original_empty_typed_exceptions_and_runtime_dispatch": "retained",
                    "original_empty_bridge_exceptions": "reported inherited ordered Exceptions after javac regeneration",
                    "super_effects_checked_failure_identity_and_late_result_cast": "matched",
                    "original_inventory_and_other_metadata": "unchanged except reported private helpers",
                    "same_Code_flags_and_full_parent_counterexamples": "rejected",
                    "independent_javac_incompatible_parent_binding": "rejected",
                    "input_harness_and_sdk_bytes": "unchanged", "coverage": coverage["status"]}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
