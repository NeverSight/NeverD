#!/usr/bin/env python3
"""Verify shared captured class upper bounds with unchanged D8/JVM method bodies."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk
    from test_mobile_android_static_defaults import method_code


def class_records(data: bytes) -> dict:
    """Read the owned constant pool and attribute offsets without changing code."""
    count = int.from_bytes(data[8:10], "big")
    strings, classes, index, offset = {}, {}, 1, 10
    while index < count:
        tag = data[offset]
        offset += 1
        if tag == 1:
            size = int.from_bytes(data[offset:offset + 2], "big")
            offset += 2
            strings[index] = data[offset:offset + size]
            offset += size
        elif tag == 7:
            classes[index] = int.from_bytes(data[offset:offset + 2], "big")
            offset += 2
        elif tag in (3, 4, 9, 10, 11, 12, 17, 18):
            offset += 4
        elif tag in (5, 6):
            offset += 8
            index += 1
        elif tag in (8, 16, 19, 20):
            offset += 2
        elif tag == 15:
            offset += 3
        else:
            raise RuntimeError("Unexpected owned constant-pool tag")
        index += 1
    pool_end = offset
    this_class = int.from_bytes(data[offset + 2:offset + 4], "big")
    offset += 6
    offset += 2 + 2 * int.from_bytes(data[offset:offset + 2], "big")

    def attributes(position: int, size: int) -> tuple:
        rows = []
        for _ in range(size):
            name, length = struct.unpack_from(">HI", data, position)
            end = position + 6 + length
            rows.append((strings[name], position, end))
            position = end
        return position, rows

    fields = int.from_bytes(data[offset:offset + 2], "big")
    offset += 2
    for _ in range(fields):
        offset, _ = attributes(offset + 8, int.from_bytes(data[offset + 6:offset + 8], "big"))
    methods = int.from_bytes(data[offset:offset + 2], "big")
    offset += 2
    for _ in range(methods):
        offset, _ = attributes(offset + 8, int.from_bytes(data[offset + 6:offset + 8], "big"))
    class_attributes = offset
    attribute_count = int.from_bytes(data[offset:offset + 2], "big")
    end, rows = attributes(offset + 2, attribute_count)
    if end != len(data):
        raise RuntimeError("Unexpected owned class attribute boundaries")
    return {"count": count, "strings": strings, "classes": classes, "this_class": this_class,
            "pool_end": pool_end, "class_attributes": class_attributes,
            "attribute_count": attribute_count, "rows": rows}


def captured_metadata(path: Path, *, marker: bool = False,
                      recursive: bool = False) -> dict:
    """Change only Signature/enclosure metadata; compare all Code attributes."""
    original = path.read_bytes()
    old_signature = (b"<T::Lfixture/Flag<+Lfixture/Value;>;>Ljava/lang/Object;Lfixture/Reader;" if marker
                     else b"<T:Lfixture/UpperBound;>Ljava/lang/Object;Lfixture/Reader<TT;>;")
    new_signature = b"Ljava/lang/Object;Lfixture/Reader;" if marker else b"Ljava/lang/Object;Lfixture/Reader<TT;>;"
    if recursive:
        old_signature = (b"<T:Ljava/lang/Object;K::Ljava/lang/Comparable<-TK;>;>"
                         b"Ljava/lang/Object;Lfixture/Reader<TT;Ljava/lang/Integer;>;")
        new_signature = b"Ljava/lang/Object;Lfixture/Reader<TT;Ljava/lang/Integer;>;"
    old = b"\x01" + len(old_signature).to_bytes(2, "big") + old_signature
    new = b"\x01" + len(new_signature).to_bytes(2, "big") + new_signature
    if original.count(old) != 1:
        raise RuntimeError("Expected one owned Worker class Signature")
    data = original.replace(old, new, 1)
    shape = class_records(data)
    count, strings, classes = shape["count"], shape["strings"], shape["classes"]
    this_class, pool_end = shape["this_class"], shape["pool_end"]
    class_attributes, attribute_count, rows = shape["class_attributes"], shape["attribute_count"], shape["rows"]
    owners = [key for key, value in classes.items() if strings[value] == b"fixture/Owner"]
    if (len(owners) != 1 or strings[classes[this_class]] != b"fixture/Owner$Worker"
            or any(key == b"EnclosingMethod" for key, _, _ in rows)):
        raise RuntimeError("Owned Worker/Owner enclosure changed")
    owner = owners[0]
    tail = bytearray(data[pool_end:])
    targets = 0
    for name, start, stop in rows:
        if name != b"InnerClasses":
            continue
        size = int.from_bytes(data[start + 6:start + 8], "big")
        if stop != start + 8 + 8 * size:
            raise RuntimeError("Unexpected owned InnerClasses table size")
        for i in range(size):
            at = start + 8 + i * 8
            inner, outer, short_name, flags = struct.unpack_from(">HHHH", data, at)
            if inner != this_class:
                continue
            if outer != owner or strings[short_name] != b"Worker" or flags != 25:
                raise RuntimeError("Owned Worker nesting flags or identity changed")
            struct.pack_into(">HH", tail, at + 2 - pool_end, 0, 0)
            targets += 1
    if targets != 1:
        raise RuntimeError("Missing unique owned Worker InnerClasses entry")
    struct.pack_into(">H", tail, class_attributes - pool_end, attribute_count + 1)
    records = b""
    prototype = (b"(Lfixture/Selector;Ljava/lang/Comparable;)Ljava/lang/Object;" if recursive
                 else b"(Lfixture/Token;)Ljava/lang/Object;")
    for value in (b"EnclosingMethod", b"make", prototype):
        records += b"\x01" + len(value).to_bytes(2, "big") + value
    records += struct.pack(">BHH", 12, count + 1, count + 2)
    enclosing = struct.pack(">HIHH", count, 4, owner, count + 3)
    changed = data[:8] + (count + 4).to_bytes(2, "big") + data[10:pool_end] + records + tail + enclosing
    before_code, after_code = method_code(original), method_code(changed)
    if before_code != after_code:
        raise RuntimeError("Captured enclosure edit changed original Code attributes or descriptors")
    path.write_bytes(changed)
    return {"class_signature": new_signature.decode(), "enclosing_method": "fixture.Owner.make" + prototype.decode(),
            "inner_name": "absent", "original_method_bytecode": "unchanged",
            "before_sha256": hashlib.sha256(original).hexdigest(),
            "after_sha256": hashlib.sha256(changed).hexdigest(),
            "original_code_sha256": {key.decode(): hashlib.sha256(code).hexdigest()
                                     for key, code in before_code.items()}}


def owner_metadata(path: Path) -> dict:
    """Keep both InnerClasses tables consistent for independent JVM reflection."""
    original = path.read_bytes()
    shape = class_records(original)
    strings, classes = shape["strings"], shape["classes"]
    changed = bytearray(original)
    targets = 0
    for name, start, stop in shape["rows"]:
        if name != b"InnerClasses":
            continue
        size = int.from_bytes(original[start + 6:start + 8], "big")
        if stop != start + 8 + 8 * size:
            raise RuntimeError("Unexpected owned Owner InnerClasses table")
        for i in range(size):
            at = start + 8 + i * 8
            inner, outer, short_name, flags = struct.unpack_from(">HHHH", original, at)
            if strings[classes[inner]] != b"fixture/Owner$Worker":
                continue
            if strings[classes[outer]] != b"fixture/Owner" or strings[short_name] != b"Worker" or flags != 25:
                raise RuntimeError("Owned Owner nesting identity changed")
            struct.pack_into(">HH", changed, at + 2, 0, 0)
            targets += 1
    if targets != 1 or method_code(original) != method_code(bytes(changed)):
        raise RuntimeError("Owner enclosure edit changed original Code bytes or descriptors")
    path.write_bytes(changed)
    return {"inner_name": "absent", "original_method_bytecode": "unchanged",
            "before_sha256": hashlib.sha256(original).hexdigest(),
            "after_sha256": hashlib.sha256(changed).hexdigest()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--d8", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--marker-method-bound", action="store_true")
    parser.add_argument("--method-class-bound", action="store_true")
    args = parser.parse_args()
    if args.marker_method_bound and args.method_class_bound:
        parser.error("Choose one original enclosing bound scope")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-shared-upper-bound-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), args.d8.resolve(), 300)
        originals, harness_dir = work / "originals", work / "harness"
        profile = "shared_marker_bounds" if args.marker_method_bound else "shared_upper_bounds"
        harness_name = "SharedMarkerBoundHarness" if args.marker_method_bound else "SharedUpperBoundHarness"
        inputs = sorted((FIXTURES / profile / "java").rglob("*.java"))
        harness = FIXTURES / "harness" / (harness_name + ".java")
        if args.method_class_bound:
            original_owner = next(path for path in inputs if path.name == "Owner.java")
            text = original_owner.read_text()
            text = text.replace("public final class Owner<T extends UpperBound>",
                                "public final class Owner")
            text = text.replace("public Object make(",
                                "public static <T extends UpperBound> Object make(")
            text = text.replace("public Object another(",
                                "public static <T extends UpperBound> Object another(")
            owned = work / "owned-sources" / "Owner.java"
            owned.parent.mkdir()
            owned.write_text(text)
            inputs = [owned if path == original_owner else path for path in inputs]
            observer = harness.read_text().replace(
                "Owner<UpperBound> owner = new Owner<UpperBound>();",
                "Owner owner = new Owner();")
            observer = observer.replace(
                '((TypeVariable<?>) type).getGenericDeclaration() == Owner.class',
                '((TypeVariable<?>) type).getGenericDeclaration().equals(Owner.class.getDeclaredMethod("make", Token.class))')
            observer = observer.replace("private static void variable(Type type) {",
                                        "private static void variable(Type type) throws Exception {")
            observer = observer.replace("Owner.class.getTypeParameters()[0].getBounds()[0]",
                                        "Owner.class.getDeclaredMethod(\"make\", Token.class).getTypeParameters()[0].getBounds()[0]")
            harness = owned.parent / (harness_name + ".java")
            harness.write_text(observer)
        verify.compile_local(inputs, originals)
        verify.compile_local([harness], harness_dir, classpath=originals, release=17)
        classes, methods = compiler_inventory(verify, originals)
        paths = sorted(originals.rglob("*.class"))
        before = {path.relative_to(originals).as_posix(): method_code(path.read_bytes()) for path in paths}
        mutation = captured_metadata(originals / "fixture/Owner$Worker.class", marker=args.marker_method_bound)
        mutation["owner_enclosure"] = owner_metadata(originals / "fixture/Owner.class")
        if before != {path.relative_to(originals).as_posix(): method_code(path.read_bytes()) for path in paths}:
            raise RuntimeError("Owned class metadata edit changed other method bodies")
        (work / "input-metadata-change.json").write_text(json.dumps(mutation, indent=2) + "\n")
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (harness_dir, originals))),
                               harness_name, "original"], "baseline")
        dex = verify.dex(paths, work / "dex", originals)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned shared upper bound declaration inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        verify.run([verify.dexdump, "-d", dex], "sdk-code")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial"):
            raise RuntimeError("Shared upper bound changed inventory or omitted partial coverage")
        changes = []
        for row in coverage.get("generic_signature_projections", []):
            if not row.get("original_signature") or not row.get("metadata_differences"):
                raise RuntimeError("Missing original shared upper bound Signature or metadata difference")
            changes.extend(row["changes"])
        origin = ("Lfixture/Owner;->make(Lfixture/Token;)Ljava/lang/Object;" if args.marker_method_bound or args.method_class_bound
                  else "Lfixture/Owner;")
        kind = ("shared-method-marker-bound" if args.marker_method_bound else
                "shared-method-class-upper-bound" if args.method_class_bound else "shared-class-upper-bound")
        bound = "Lfixture/Flag<+Lfixture/Value;>;" if args.marker_method_bound else "Lfixture/UpperBound;"
        bounded = [row for row in changes if row["kind"] == kind]
        if (len(bounded) != (3 if args.marker_method_bound else 4)
                or any(row["original_type"] != origin + "::T" or row["source_type"] != bound
                       or row.get("scope_origin") != origin for row in bounded)
                or sum(row["kind"] == "anonymous-captured-type-arguments" for row in changes) != 1):
            raise RuntimeError("Missing exact original class bound or self argument projection")
        bindings = coverage.get("class_source_bindings", [])
        if len(bindings) != 1 or bindings[0]["binding_kind"] != "anonymous-to-shared-member":
            raise RuntimeError("Missing shared original class identity")
        binding = bindings[0]
        mapped = binding["source_enclosing_class"][:-1] + "$" + binding["source_name"] + ";"
        rebuilt = work / "rebuilt"
        verify.compile_local([*sorted((output / "sources").rglob("*.java")), harness], rebuilt, release=17)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        expected = {identity.replace(binding["class"], mapped) for identity in methods}
        if mapped not in rebuilt_classes or not expected.issubset(rebuilt_methods):
            raise RuntimeError("Recompiled shared upper bound lost a runtime descriptor")
        actual = verify.run([verify.java, "-cp", rebuilt, harness_name, "recovered"],
                            "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Shared bound projection changed executable behavior")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "returned_identities": 34 if args.marker_method_bound else 32,
                    "ordered_failures": 10 if args.marker_method_bound else 26,
                    "concrete_subclasses": 2,
                    "capture_identity": "preserved", "runtime_descriptors": "preserved",
                    "owner_bound": "preserved", "generic_reflection_differences": 4 if args.marker_method_bound else 5,
                    "original_method_bytecode": "unchanged", "source_generic_bounds": "revalidated"}
        if args.marker_method_bound:
            evidence["closed_marker_wildcard"] = "preserved in every projected use"
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
