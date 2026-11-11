#!/usr/bin/env python3
"""Verify typed allocation defaults in final anonymous members without constant reads."""
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
import zlib

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


ANONYMOUS = "Lfixture/Owner$1;"
TYPES = {"Z": "false", "B": "(byte) 0", "C": "(char) 0", "S": "(short) 0",
         "I": "0", "J": "0L", "F": "0.0f", "D": "0.0d",
         "Ljava/lang/Object;": "null", "[I": "null"}


def encode_finals(directory):
    """Change only witnessed access flags; retain every original instruction and attribute."""
    receipts = []
    for path in sorted(directory.rglob("*.class")):
        before = path.read_bytes()
        shape = class_records(before)
        strings, classes = shape["strings"], shape["classes"]
        after, edits = bytearray(before), []

        def add_final(at, role):
            flags = int.from_bytes(before[at:at + 2], "big")
            if flags & 0x10:
                raise RuntimeError("Original typed-default flag is already final")
            struct.pack_into(">H", after, at, flags | 0x10)
            edits.append({"offset": at, "role": role, "before": flags, "after": flags | 0x10})

        if path.name == "Owner$1.class":
            if strings[classes[shape["this_class"]]] != b"fixture/Owner$1":
                raise RuntimeError("Original anonymous class identity changed")
            add_final(shape["pool_end"], "class")
            pos = shape["pool_end"] + 6
            pos += 2 + 2 * int.from_bytes(before[pos:pos + 2], "big")
            count = int.from_bytes(before[pos:pos + 2], "big")
            pos += 2
            seen = set()
            for _ in range(count):
                flags, name, descriptor, attrs = struct.unpack_from(">HHHH", before, pos)
                field, field_type = strings[name].decode(), strings[descriptor].decode()
                if flags != 1 or not field.startswith("zero") or field_type not in TYPES:
                    raise RuntimeError("Original anonymous instance field changed")
                seen.add(field_type)
                add_final(pos, field + ":" + field_type)
                pos += 8
                for _ in range(attrs):
                    pos += 6 + int.from_bytes(before[pos + 2:pos + 6], "big")
            if count != 10 or seen != set(TYPES):
                raise RuntimeError("Missing original typed allocation defaults")
        for name, start, end in shape["rows"]:
            if name != b"InnerClasses":
                continue
            count = int.from_bytes(before[start + 6:start + 8], "big")
            if end != start + 8 + 8 * count:
                raise RuntimeError("Original InnerClasses boundaries changed")
            for index in range(count):
                at = start + 8 + 8 * index
                inner, outer, short_name, _ = struct.unpack_from(">HHHH", before, at)
                if strings[classes[inner]] == b"fixture/Owner$1":
                    if outer or short_name:
                        raise RuntimeError("Original class is no longer anonymous")
                    add_final(at + 6, "anonymous InnerClasses")
        if method_code(before) != method_code(bytes(after)):
            raise RuntimeError("Final flags changed original Code or descriptors")
        allowed = {row["offset"] + byte for row in edits for byte in (0, 1)}
        if any(a != b and index not in allowed for index, (a, b) in enumerate(zip(before, after))):
            raise RuntimeError("Final flags changed unwitnessed bytes")
        if edits:
            path.write_bytes(after)
            receipts.append({"class_file": path.relative_to(directory).as_posix(), "edits": edits,
                             "all_other_bytes_and_Code": "unchanged"})
    if sorted(len(row["edits"]) for row in receipts) != [1, 12]:
        raise RuntimeError("Missing exact final class/field/enclosure flag edits")
    return receipts



def encode_receiver_copy(path, verify, work):
    """Witness an equivalent register layout at Object's empty constructor call."""
    dump = verify.run([verify.dexdump, "-d", path], "sdk-code-before")
    match = re.search(r"\] fixture\.Owner\$1\.<init>:\(\)V\n"
                      r"([0-9a-f]+):[^\n]+\|0000: invoke-direct/range \{v21\}, "
                      r"Ljava/lang/Object;\.<init>:\(\)V[^\n]+\n"
                      r"[0-9a-f]+:[^\n]+\|0003: move-object/from16 v0, v21", dump)
    if not match:
        raise RuntimeError("Original D8 receiver preparation changed")
    before = path.read_bytes()
    at = int(match[1], 16)
    opcode, target, receiver, move, source = struct.unpack_from("<5H", before, at)
    if (opcode, receiver, move, source) != (0x0176, 21, 0x0008, 21):
        raise RuntimeError("Original constructor word layout changed")
    # Both instructions are retained. Move uninitialized this first, then use
    # that exact alias for Object preparation. No other entry reaches the gap;
    # every following code address, handler and debug address stays fixed.
    after = bytearray(before)
    struct.pack_into("<5H", after, at, 0x0008, 21, 0x1070, target, 0)
    after[12:32] = hashlib.sha1(after[32:]).digest()
    struct.pack_into("<I", after, 8, zlib.adler32(after[12:]) & 0xffffffff)
    allowed = set(range(8, 32)) | set(range(at, at + 10))
    if any(a != b and i not in allowed for i, (a, b) in enumerate(zip(before, after))):
        raise RuntimeError("Receiver layout changed other DEX bytes")
    path.write_bytes(after)
    (work / "receiver-layout-witness.json").open("x").write(json.dumps({
        "constructor": "Lfixture/Owner$1;-><init>()V", "offset": at,
        "before": list(struct.unpack_from("<5H", before, at)),
        "after": list(struct.unpack_from("<5H", after, at)),
        "change": "move unchanged this-copy before empty Object preparation",
        "other_bytes": "unchanged except DEX integrity fields"}, indent=2) + "\n")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-view", action="store_true")
    parser.add_argument("--baseline-only", action="store_true")
    parser.add_argument("--moved-receiver", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-unwritten-typed-finals-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        version = verify.run([verify.java, "--version"], "jdk-version")
        major = re.search(r"(?m)^(?:openjdk|java) (\d+)", version)
        if not major:
            raise RuntimeError("Unknown owned JVM version")
        java_options = ["--enable-final-field-mutation=ALL-UNNAMED"] if int(major[1]) >= 26 else []
        fixture = FIXTURES / "unwritten_typed_finals/java"
        if args.moved_receiver:
            copied = work / "authored"
            shutil.copytree(fixture, copied)
            shutil.copy2(FIXTURES / "moved_receiver_finals/java/fixture/Owner.java",
                         copied / "fixture/Owner.java")
            fixture = copied
        harness_name = "MovedReceiverFinalsHarness" if args.moved_receiver else "UnwrittenTypedFinalsHarness"
        expected_methods = 25 if args.moved_receiver else 24
        originals, encoded, harness = (work / name for name in ("originals", "encoded", "harness"))
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("UnwrittenTypedFinalsCaller", harness_name)],
                             harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != expected_methods:
            raise RuntimeError("Original typed-default inventory changed")

        def execute(runtime, finals, label):
            results = []
            for route in range(4):
                for mode in range(8):
                    out = verify.run([verify.java, *java_options, "-cp", os.pathsep.join(map(str, runtime)),
                                      harness_name, str(route), str(mode), str(finals).lower()],
                                     f"{label}-{route}-{mode}")
                    expected = [f"route={route},mode={mode},attempt={attempt},checks=60" for attempt in range(2)]
                    if out.splitlines() != expected:
                        raise RuntimeError("Original typed-default observer changed")
                    results.append(out)
            return results

        baseline = execute((originals, harness), False, "mutable")
        before = class_identity.compiler_classes(originals)
        shutil.copytree(originals, encoded)
        receipt = encode_finals(encoded)
        (work / "final-flags-witness.json").open("x").write(json.dumps(receipt, indent=2) + "\n")
        records, wanted = class_identity.compiler_classes(encoded), copy.deepcopy(before)
        wanted[ANONYMOUS]["access"] |= 0x10
        wanted[ANONYMOUS]["inner_class"]["access"] |= 0x10
        for metadata in wanted[ANONYMOUS]["fields"].values():
            if metadata["constant_value"] is not None:
                raise RuntimeError("Original instance field became a constant")
            metadata["access"] |= 0x10
        for owner in records:
            for key in wanted[owner]:
                if key != "sha256" and wanted[owner][key] != records[owner].get(key):
                    raise RuntimeError("Final flags changed other original metadata: " + owner + ":" + key)
        if execute((encoded, harness), True, "encoded") != baseline:
            raise RuntimeError("Final flags changed original allocation/default/read behavior")
        if args.baseline_only:
            print(json.dumps({"status": "baseline-verified", "class_count": 3, "method_count": expected_methods,
                              "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "stages": 2}))
            return 0
        encoded_hashes, exceptions = hashes(encoded), exception_inventory(encoded)
        classes = all_classes - ({"Lfixture/View;"} if args.dependency_view else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        dependency = work / "owned-view.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            jar.writestr("fixture/View.class", (encoded / "fixture/View.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_view:
            classpath += ["--classpath", dependency]
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(encoded.rglob("*.class"))
                 if "L" + path.relative_to(encoded).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original typed-default DEX inventory changed")
        if args.moved_receiver:
            encode_receiver_copy(source_dex, verify, work)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original typed-default entry")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        if args.moved_receiver:
            # Verify the independently witnessed register layout in the actual
            # input; inventory alone cannot prove receiver placement.
            code_dump = verify.run([verify.dexdump, "-d", source_dex], "sdk-code")
            match = re.search(r"\] fixture\.Owner\$1\.<init>:\(\)V(.*?)(?=\n\s*Class #|\n\s*name\s*:)",
                              code_dump, re.S)
            if not match:
                raise RuntimeError("Missing original anonymous constructor disassembly")
            code_text = match[1]
            if not re.search(r"move-object/from16.*?invoke-direct.*?Ljava/lang/Object;\.<init>",
                             code_text, re.S):
                raise RuntimeError("Encoded DEX did not retain the witnessed receiver alias prefix")

        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = set(records[ANONYMOUS]["methods"]) | {"Lfixture/Owner;-><clinit>()V"}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage["status"] != "partial" or coverage["projected_method_count"] != 12
                or coverage.get("generic_signature_projections")
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != projected
                or any(rows[identity].get("projection_kind") != "anonymous-to-named-member" for identity in projected)):
            raise RuntimeError("Typed-default recovery changed original contracts or anonymous projections")
        initializers = coverage.get("default_instance_final_initializers", [])
        if len(initializers) != 10 or {row["type"] for row in initializers} != set(TYPES):
            raise RuntimeError("Missing all typed-default initialization receipts")
        for row in initializers:
            field_type = row["type"]
            suffix = {"Ljava/lang/Object;": "O", "[I": "A"}.get(field_type, field_type)
            zero = TYPES[field_type]
            if (row["class"] != ANONYMOUS or row["field"] != "zero" + suffix
                    or row["original_write_count"] != 0 or row["ConstantValue"] != "absent"
                    or row["source_initializer"] != f"(null == null ? {zero} : {zero})"
                    or row["initialization_point"] != "after-Object-constructor-before-original-body"):
                raise RuntimeError("Typed default lost its exact proof receipt")
        bindings = coverage.get("class_source_bindings", [])
        if (len(bindings) != 1 or bindings[0]["class"] != ANONYMOUS
                or bindings[0]["binding_kind"] != "anonymous-to-named-member"
                or bindings[0]["source_enclosing_class"] != "Lfixture/Owner;"
                or bindings[0]["binary_name_status"] != "changed" or not bindings[0]["metadata_differences"]):
            raise RuntimeError("Missing original anonymous member source binding")
        binding = bindings[0]
        renamed = binding["source_enclosing_class"][:-1] + "$" + binding["source_name"] + ";"

        def mapped(value):
            return value.replace(ANONYMOUS, renamed)

        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_view else None,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {mapped(row["class"] + "->" + row["name"] + row["prototype"])
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {mapped(owner) + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if owner != "Lfixture/View;"}
        if (actual_classes != {mapped(owner) for owner in classes}
                or set(actual_methods) != {mapped(identity) for identity in methods} | expected_helpers
                or helpers != expected_helpers):
            raise RuntimeError("Typed-default recovery lost or invented runtime entries")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            previous, after = copy.deepcopy(records[owner]), actual_records[mapped(owner)]
            previous["fields"] = {mapped(identity): value for identity, value in previous["fields"].items()}
            previous["methods"] = {mapped(identity): value for identity, value in previous["methods"].items()}
            if owner == ANONYMOUS:
                previous["enclosing_method"] = None
                previous["inner_class"] = {"inner": renamed, "outer": "Lfixture/Owner;",
                                           "name": binding["source_name"], "access": 0x18}
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if previous.get(key) != after.get(key):
                    raise RuntimeError("Typed-default metadata changed beyond its binding: " + owner + ":" + key)
            for identity, metadata in previous["methods"].items():
                original = identity.replace(renamed, ANONYMOUS)
                if after["methods"].get(identity) != metadata or actual_exceptions.get(identity) != exceptions[original]:
                    raise RuntimeError("Typed-default original method metadata/Exceptions changed: " + original)
        runtime = (rebuilt, harness, dependency) if args.dependency_view else (rebuilt, harness)
        if execute(runtime, True, "rebuilt") != baseline:
            raise RuntimeError("Typed defaults, raw floating bits, identities or reflective field reads changed")
        original = (fixture / "fixture/Owner.java").read_text()
        mutants = (("constant-boolean", "return zeroZ;", "return false;", 0),
                   ("constant-float", "return zeroF;", "return 0.0f;", 1),
                   ("wrong-reference", "return zeroO;", "return null;", 0),
                   ("truncated-wide", "return zeroJ;", "return (int) zeroJ;", 0),
                   ("negative-zero", "public double zeroD;", "public double zeroD = -0.0d;", 0))
        if args.moved_receiver:
            mutants += (("duplicate-preparation", "calls++;", "calls += 2;", 0),
                        ("wrong-wide-frame-argument", "sum = a1 +", "sum = 0 +", 0),
                        ("lost-publication", "observed = value;", "observed = null;", 0))
        for label, anchor, replacement, mode in mutants:
            if original.count(anchor) != 1:
                raise RuntimeError("Typed-default counterexample anchor changed")
            changed = work / label / "fixture/Owner.java"
            changed.parent.mkdir(parents=True)
            changed.open("x").write(original.replace(anchor, replacement))
            wrong = work / (label + "-classes")
            verify.compile_local([changed], wrong, classpath=originals, release=8)
            encode_finals(wrong)
            result = subprocess.run([verify.java, *java_options, "-cp", os.pathsep.join(map(str, (wrong, encoded, harness))),
                                     harness_name, "1", str(mode), "true"],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned unwritten typed final" not in result.stderr:
                raise RuntimeError("Frozen typed-default observer missed " + label)
        if (hashes(originals) != original_hashes or hashes(encoded) != encoded_hashes
                or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs or frozen observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "stages": 3,
                    "dependency_view": args.dependency_view, "moved_receiver": args.moved_receiver,
                    "typed_default_receipts": 10,
                    "raw_bits_reference_identity_and_repeated_reflective_mutation": "matched",
                    "all_metadata_and_ConstantValue_absence": "unchanged except witnessed flags and named-member binding",
                    "original_Code_descriptors_and_other_attributes": "unchanged",
                    "independent_wrong_programs": len(mutants)}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
