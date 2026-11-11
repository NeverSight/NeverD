#!/usr/bin/env python3
"""Verify explicit DEX default values followed by original final-field writes."""
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


def encoded_defaults(data: bytes, owner: str, expected: dict | None = None) -> dict:
    """Read owned D8 static-value entries independently of NeverD."""
    def u32(offset):
        return int.from_bytes(data[offset:offset + 4], "little")
    def uleb(offset):
        result = 0
        for shift in range(0, 35, 7):
            value = data[offset]; offset += 1
            result |= (value & 127) << shift
            if value < 128:
                return result, offset
        raise RuntimeError("Owned DEX has a malformed ULEB")
    strings = []
    for index in range(u32(56)):
        _, offset = uleb(u32(u32(60) + index * 4))
        strings.append(data[offset:data.index(0, offset)].decode("utf-8"))
    types = [strings[u32(u32(68) + index * 4)] for index in range(u32(64))]
    for index in range(u32(96)):
        start = u32(100) + index * 32
        if types[u32(start)] != owner:
            continue
        offset = u32(start + 24)
        static_count, offset = uleb(offset)
        for _ in range(3):
            _, offset = uleb(offset)
        fields, field_index = [], 0
        for _ in range(static_count):
            delta, offset = uleb(offset); field_index += delta
            _, offset = uleb(offset)
            field = u32(84) + field_index * 8
            fields.append((strings[u32(field + 4)], types[int.from_bytes(data[field + 2:field + 4], "little")]))
        position = u32(start + 28)
        if not position:
            raise RuntimeError("Owned D8 output lacks its encoded static defaults")
        count, position = uleb(position)
        if count > len(fields):
            raise RuntimeError("Owned DEX static value count exceeds declarations")
        result = {}
        for name, typ in fields[:count]:
            header = data[position]; position += 1
            kind, argument = header & 31, header >> 5
            if kind in (0x1e, 0x1f):
                bits = argument
            else:
                size = argument + 1
                bits = int.from_bytes(data[position:position + size], "little"); position += size
                if kind in (0x10, 0x11):
                    bits <<= ((4 if kind == 0x10 else 8) - size) * 8
            result[name] = {"type": typ, "kind": kind, "bits": bits}
        if expected is None:
            expected = {"aZ": ("Z", 0x1f, 0), "aB": ("B", 0x00, 0), "aC": ("C", 0x03, 0), "aS": ("S", 0x02, 0),
                        "aI": ("I", 0x04, 0), "aJ": ("J", 0x06, 0), "aF": ("F", 0x10, 0), "aD": ("D", 0x11, 0)}
        for name, (typ, kind, bits) in expected.items():
            if result.get(name) != {"type": typ, "kind": kind, "bits": bits}:
                raise RuntimeError("D8 changed the expected typed static value: " + name)
        return {name: result[name] for name in expected}
    raise RuntimeError("Missing owned static default class")


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-encoded-final-defaults-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "encoded_final_defaults/java"
        originals, encoded, harness, fresh = (work / name for name in ("originals", "encoded", "harness", "fresh"))
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        fresh_source = FIXTURES / "harness/FreshEncodedFinalCaller.java"
        verify.compile_local([fresh_source], fresh, classpath=originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("EncodedFinalCaller", "EncodedFinalDefaultsHarness")], harness,
                             classpath=os.pathsep.join(map(str, (originals, fresh))), release=8)
        original_hashes, harness_hashes, fresh_hashes = hashes(originals), hashes(harness), hashes(fresh)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        classes_all, methods_all = compiler_inventory(verify, originals)
        if len(classes_all) != 5 or len(methods_all) != 9:
            raise RuntimeError("Owned default-final inventory changed")

        def execute(runtime, label):
            result = []
            for route in range(5):
                for mode in range(6):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "EncodedFinalDefaultsHarness", str(route), str(mode)], f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Original default-final behavior matrix changed: " + output)
                    result.append(output)
            return result

        baseline = execute((originals, harness, fresh), "baseline")
        encoded = originals
        records = class_identity.compiler_classes(originals)
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
        defaults = encoded_defaults(source_dex.read_bytes(), "Lfixture/Owner;")
        (work / "encoded-defaults.json").write_text(json.dumps(defaults, indent=2) + "\n")
        for name in [*defaults, "aRef", "aArray"]:
            if not any("sput" in line and "Lfixture/Owner;." + name + ":" in line for line in disassembly.splitlines()):
                raise RuntimeError("D8 lost an original final-field write: " + name)
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
        expected_helpers.add("Lfixture/Owner;->__neverdConstant(I)I")
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Default-final recovery invented or omitted a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            before = copy.deepcopy(before)
            if owner == "Lfixture/Owner;":
                anchors = [row for row in before["fields"].values() if row["name"] == "zzzAnchor"]
                if len(anchors) != 1 or anchors[0]["constant_value"] is None:
                    raise RuntimeError("Owned encoded-value anchor lost its ConstantValue")
                # Existing non-inlining emission replaces this unrelated
                # constant attribute with the reported pure constant helper.
                anchors[0]["constant_value"] = None
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
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Default-final recovery changed an original initialization effect")
        wrong = work / "early-write/fixture/Owner.java"
        wrong.parent.mkdir(parents=True)
        source = (fixture / "fixture/Owner.java").read_text()
        if source.count("aI = Trace.i;") != 1 or source.count("Trace.hit(1);") != 1:
            raise RuntimeError("Owned initialization order anchors changed")
        wrong.write_text(source.replace("aI = Trace.i;", "").replace("Trace.hit(1);", "aI = Trace.i; Trace.hit(1);"))
        wrong_classes = work / "early-write-classes"
        verify.compile_local([wrong], wrong_classes, classpath=encoded, release=8)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong_classes, encoded, harness, fresh))),
                                 "EncodedFinalDefaultsHarness", "4", "0"], text=True, capture_output=True,
                                timeout=300, env=verify.environment)
        (work / "early-write-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "encoded final defaults contract" not in result.stderr:
            raise RuntimeError("Independent oracle did not detect premature final-field initialization")
        if (hashes(originals) != original_hashes or hashes(encoded) != encoded_hashes or
                hashes(harness) != harness_hashes or hashes(fresh) != fresh_hashes or
                hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash or
                hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": 30, "attempts_per_case": 2, "dependency_trace": args.dependency_trace,
                    "genuine_d8_encoded_defaults": defaults, "original_and_rebuilt_behavior": "matched",
                    "original_metadata_Exceptions_and_ConstantValue_absence": "unchanged except the reported non-inlining helper for zzzAnchor",
                    "reentrant_defaults_class_initialization_and_failure_order": "matched",
                    "new_consumer_getstatic_bytes": "unchanged", "independent_early_write_counterexample": "detected",
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
