#!/usr/bin/env python3
"""Verify mutable encoded integer field reads before this/super constructor calls."""
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
    from scripts.test_mobile_android_encoded_final_defaults import encoded_defaults
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from test_mobile_android_encoded_final_defaults import encoded_defaults
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def mutable_constant_fields(path: Path) -> dict:
    """Clear only ACC_FINAL on two owned fields; retain ConstantValue and Code."""
    before = path.read_bytes()
    shape = class_records(before)
    strings = shape["strings"]
    offset = shape["pool_end"] + 6
    offset += 2 + 2 * int.from_bytes(before[offset:offset + 2], "big")
    count = int.from_bytes(before[offset:offset + 2], "big"); offset += 2
    changed, names = bytearray(before), []
    for _ in range(count):
        flags, name, descriptor, attrs = struct.unpack_from(">HHHH", before, offset)
        selected = strings[name] in (b"word", b"wide")
        if selected:
            if flags != 0x19 or strings[descriptor] != (b"I" if strings[name] == b"word" else b"J"):
                raise RuntimeError("Owned encoded integer field flags or type changed")
            struct.pack_into(">H", changed, offset, 0x9)
            names.append(strings[name].decode())
        offset += 8
        attribute_names = []
        for _ in range(attrs):
            index, size = struct.unpack_from(">HI", before, offset)
            attribute_names.append(strings[index]); offset += 6 + size
        if selected and attribute_names.count(b"ConstantValue") != 1:
            raise RuntimeError("Missing original encoded value attribute")
    if sorted(names) != ["wide", "word"] or method_code(before) != method_code(bytes(changed)):
        raise RuntimeError("Mutable flag encoding changed original Code or inventory")
    path.write_bytes(changed)
    return {"fields": names, "before_flags": 0x19, "after_flags": 0x9,
            "Code_descriptors_and_ConstantValue_attributes": "unchanged"}


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-constructor-encoded-fields-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "constructor_encoded_fields/java"
        bootstrap, originals, harness = (work / name for name in ("bootstrap", "originals", "harness"))
        verify.compile_local([fixture / "fixture/Holder.java", fixture / "fixture/Trace.java"], bootstrap, release=8)
        receipt = mutable_constant_fields(bootstrap / "fixture/Holder.class")
        (work / "mutable-field-flags.json").write_text(json.dumps(receipt, indent=2) + "\n")
        verify.compile_local([p for p in sorted(fixture.rglob("*.java")) if p.name not in ("Holder.java", "Trace.java")], originals,
                             classpath=bootstrap, release=8)
        for name in ("Holder", "Trace"):
            shutil.copyfile(bootstrap / ("fixture/" + name + ".class"), originals / ("fixture/" + name + ".class"))
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("ConstructorEncodedFieldCaller", "ConstructorEncodedFieldHarness")], harness,
                             classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        classes_all, methods_all = compiler_inventory(verify, originals)
        if len(classes_all) != 6 or len(methods_all) != 10:
            raise RuntimeError("Owned constructor-field inventory changed")

        def execute(runtime, label):
            result = []
            for route in range(5):
                for mode in range(6):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "ConstructorEncodedFieldHarness", str(route), str(mode)], f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Original constructor-field behavior matrix changed: " + output)
                    result.append(output)
            return result

        baseline = execute((originals, harness), "baseline")
        encoded = originals
        records = class_identity.compiler_classes(originals)
        encoded_hashes, exceptions = hashes(encoded), exception_inventory(encoded)
        dependencies = {"Lfixture/Holder;"} if args.dependency_provider else set()
        classes = classes_all - dependencies
        methods = {identity for identity in methods_all if identity.split("->", 1)[0] in classes}
        dependency = work / "owned-holder.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            jar.writestr("fixture/Holder.class", (encoded / "fixture/Holder.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(encoded.rglob("*.class"))
                 if "L" + path.relative_to(encoded).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned constructor-field DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes or
                {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original constructor-field runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "default-field-disassembly")
        if not args.dependency_provider:
            values = encoded_defaults(source_dex.read_bytes(), "Lfixture/Holder;",
                {"word": ("I", 0x04, 37), "wide": ("J", 0x06, 0xfedcba9876543210)})
            (work / "encoded-values.json").write_text(json.dumps(values, indent=2) + "\n")
        for name in ("word", "wide"):
            if not any("sget" in line and "Lfixture/Holder;." + name + ":" in line for line in disassembly.splitlines()):
                raise RuntimeError("D8 lost the original runtime integer read: " + name)
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Constructor-field recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != 25:
            raise RuntimeError("Constructor-field recovery lost its Java 25 constructor prefix release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=25,
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Constructor-field recovery invented or omitted a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            before = copy.deepcopy(before)
            if owner == "Lfixture/Holder;":
                for field in before["fields"].values():
                    if field["name"] in ("word", "wide"):
                        if field["constant_value"] is None or field["access"] != 0x9:
                            raise RuntimeError("Owned mutable encoded field metadata changed")
                        field["constant_value"] = None
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original constructor-field class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original constructor-field method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Constructor-field recovery changed an original initialization effect")
        wrong = work / "late-read/fixture/Reader.java"
        wrong.parent.mkdir(parents=True)
        source = (fixture / "fixture/Reader.java").read_text()
        if source.count("Trace.hit(4);") != 1:
            raise RuntimeError("Owned postlude anchor changed")
        wrong.write_text(source.replace("Trace.hit(4);", "Trace.hit(4); this.word = Holder.word; this.wide = Holder.wide;"))
        wrong_classes = work / "late-read-classes"
        verify.compile_local([wrong], wrong_classes, classpath=originals, release=8)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong_classes, originals, harness))),
                                 "ConstructorEncodedFieldHarness", "0", "0"], text=True, capture_output=True,
                                timeout=300, env=verify.environment)
        (work / "late-read-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "constructor encoded field contract" not in result.stderr:
            raise RuntimeError("Independent oracle missed a read after superclass mutation")
        if (hashes(originals) != original_hashes or hashes(encoded) != encoded_hashes or
                hashes(harness) != harness_hashes or
                hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash or
                hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": 30, "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "mutable_field_encoding": "flags only; original ConstantValue and Code unchanged",
                    "before_this_and_super_reads_initialization_and_snapshots": "matched",
                    "source_metadata": "unchanged except mutable encoded values become source initializers",
                    "original_inventory_Exceptions_and_helpers": "matched",
                    "first_and_repeated_failures": "matched", "late_read_counterexample": "detected",
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
