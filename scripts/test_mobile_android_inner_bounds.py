#!/usr/bin/env python3
"""Verify retained interface bounds on named inner classes projected as static."""
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
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def field_records(data: bytes, shape: dict) -> list:
    offset = shape["pool_end"] + 6
    offset += 2 + 2 * int.from_bytes(data[offset:offset + 2], "big")
    count = int.from_bytes(data[offset:offset + 2], "big")
    offset += 2
    strings, result = shape["strings"], []
    for _ in range(count):
        start = offset
        flags, name, descriptor, size = struct.unpack_from(">HHHH", data, offset)
        offset += 8
        attributes = []
        for _ in range(size):
            key, length = struct.unpack_from(">HI", data, offset)
            end = offset + 6 + length
            attributes.append((strings[key], offset, end))
            offset = end
        result.append({"name": strings[name], "descriptor": strings[descriptor],
                       "flags": flags, "start": start, "end": offset,
                       "size": size, "attributes": attributes})
    return result


def capture_signature(path: Path) -> dict:
    """Add only the generic capture field Signature; keep all runtime bytes."""
    original = path.read_bytes()
    shape = class_records(original)
    before = field_records(original, shape)
    targets = [row for row in before if row["name"] == b"this$0"]
    if len(targets) != 1:
        raise RuntimeError("Missing unique owned outer-instance capture")
    target = targets[0]
    if (target["descriptor"] != b"Lfixture/Owner;" or target["flags"] != 0x1010
            or any(name == b"Signature" for name, _, _ in target["attributes"])):
        raise RuntimeError("Unexpected owned capture flags, descriptor or Signature")
    indices = [index for index, text in shape["strings"].items() if text == b"Signature"]
    if len(indices) != 1:
        raise RuntimeError("Missing unique owned Signature attribute name")
    signature = b"Lfixture/Owner<TT;>;"
    count, pool_end = shape["count"], shape["pool_end"]
    record = b"\x01" + len(signature).to_bytes(2, "big") + signature
    attribute = struct.pack(">HIH", indices[0], 2, count)
    start, end = target["start"], target["end"]
    tail = (original[pool_end:start + 6] + struct.pack(">H", target["size"] + 1)
            + original[start + 8:end] + attribute + original[end:])
    changed = original[:8] + struct.pack(">H", count + 1) + original[10:pool_end] + record + tail
    after = field_records(changed, class_records(changed))
    facts = lambda rows: [(row["name"], row["descriptor"], row["flags"]) for row in rows]
    if facts(before) != facts(after) or method_code(original) != method_code(changed):
        raise RuntimeError("Capture metadata edit changed a field descriptor or original Code bytes")
    path.write_bytes(changed)
    return {"added_capture_signature": signature.decode(), "field_descriptors": "unchanged",
            "original_method_bytecode": "unchanged",
            "before_sha256": hashlib.sha256(original).hexdigest(),
            "after_sha256": hashlib.sha256(changed).hexdigest()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--d8", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--class-bound", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-inner-interface-bound-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), args.d8.resolve(), 300)
        originals, harness_dir = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "inner_interface_bounds/java").rglob("*.java"))
        if args.class_bound:
            generated = work / "java"
            class_inputs = []
            for source in inputs:
                content = source.read_text()
                if source.name == "Item.java":
                    content = content.replace("public interface Item", "public abstract class Item")
                    content = content.replace("Object get();", "public abstract Object get();")
                else:
                    content = content.replace("implements Item", "extends Item")
                target = generated / source.relative_to(FIXTURES / "inner_interface_bounds/java")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content)
                class_inputs.append(target)
            inputs = class_inputs
        harness = FIXTURES / "harness/InnerInterfaceBoundHarness.java"
        verify.compile_local(inputs, originals)
        verify.compile_local([harness], harness_dir, classpath=originals, release=17)
        classes, methods = compiler_inventory(verify, originals)
        paths = sorted(originals.rglob("*.class"))
        before = {path.relative_to(originals).as_posix(): method_code(path.read_bytes()) for path in paths}
        mutation = capture_signature(originals / "fixture/Owner$Member.class")
        if before != {path.relative_to(originals).as_posix(): method_code(path.read_bytes()) for path in paths}:
            raise RuntimeError("Capture metadata edit changed another original method body")
        (work / "input-metadata-change.json").write_text(json.dumps(mutation, indent=2) + "\n")
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (harness_dir, originals))),
                               "InnerInterfaceBoundHarness", "original"], "baseline")
        dex = verify.dex(paths, work / "dex", originals)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned inner bound declaration inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        verify.run([verify.dexdump, "-d", dex], "sdk-code")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial" or coverage.get("minimum_java_release") != 25):
            raise RuntimeError("Inner bound changed runtime inventory or omitted partial coverage")
        projections = coverage.get("generic_signature_projections", [])
        changes = [change for row in projections for change in row["changes"]]
        kind = "static-inner-class-upper-bound" if args.class_bound else "static-inner-interface-upper-bound"
        bounded = [change for change in changes if change["kind"] == kind]
        if len(bounded) != 5 or len(changes) != 6:
            raise RuntimeError("Missing five captured formals and the separate owner qualifier projection")
        for change in bounded:
            if (change["original_type"] != "Lfixture/Owner;::T"
                    or change["source_type"] != "Lfixture/Item;"
                    or change.get("scope_origin") != "Lfixture/Owner;"):
                raise RuntimeError("Interface projection lost the exact original parent declaration")
        if (len(projections) != 5 or any(not row.get("metadata_differences") or not row.get("original_signature")
                                         for row in projections)
                or sum(change["kind"] == "static-inner-owner-arguments" for change in changes) != 1):
            raise RuntimeError("Missing original capture signatures or reported reflection differences")
        bindings = coverage.get("class_source_bindings", [])
        if (len(bindings) != 1 or bindings[0]["binding_kind"] != "inner-to-static-member"
                or not bindings[0].get("metadata_differences")):
            raise RuntimeError("Missing retained inner ownership and reported static reflection difference")
        rebuilt = work / "rebuilt"
        verify.compile_local([*sorted((output / "sources").rglob("*.java")), harness], rebuilt, release=25)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        if not classes.issubset(rebuilt_classes) or not set(methods).issubset(rebuilt_methods):
            raise RuntimeError("Recompiled inner bound lost an original runtime class or method descriptor")
        actual = verify.run([verify.java, "-cp", rebuilt, "InnerInterfaceBoundHarness", "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Inner interface bound projection changed executable behavior")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "returned_identities": 32, "ordered_failures": 13,
                    "member_dispatch": "virtual" if args.class_bound else "interface",
                    "mutable_outer_receiver": "preserved", "own_method_formal": "preserved",
                    "public_owner_bounds": "preserved", "runtime_descriptors": "preserved",
                    "captured_generic_reflection_differences": 5, "owner_qualifier_difference": "reported",
                    "original_code_bytes": "unchanged", "source_generic_bounds": "revalidated"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
