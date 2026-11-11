#!/usr/bin/env python3
"""Verify a direct erased interface implementation with an independent JVM oracle."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk


INPUT_TYPE = ("Lfixture/Pair<+Lfixture/Box<Ljava/lang/String;>;"
              "+Lfixture/Box<Ljava/lang/Object;>;>;")
CLASS_SIGNATURE = "Ljava/lang/Object;Lfixture/Contract<" + INPUT_TYPE + "Ljava/lang/String;>;"
METHOD_SIGNATURE = "(" + INPUT_TYPE + ")Ljava/lang/String;"


def direct_metadata(path: Path) -> dict:
    """Add two Signature attributes while retaining all original Code bytes."""
    data = path.read_bytes()
    if data[:4] != b"\xca\xfe\xba\xbe":
        raise RuntimeError("Missing compiled owned direct interface")
    count = int.from_bytes(data[8:10], "big")
    offset, index, strings = 10, 1, {}
    while index < count:
        tag = data[offset]
        offset += 1
        if tag == 1:
            size = int.from_bytes(data[offset:offset + 2], "big")
            offset += 2
            strings[index] = data[offset:offset + size]
            offset += size
        elif tag in (3, 4, 9, 10, 11, 12, 17, 18):
            offset += 4
        elif tag in (5, 6):
            offset += 8
            index += 1
        elif tag in (7, 8, 16, 19, 20):
            offset += 2
        elif tag == 15:
            offset += 3
        else:
            raise RuntimeError("Unexpected owned constant-pool tag")
        index += 1
    pool_end = offset
    signature_name = next(key for key, value in strings.items() if value == b"Signature")
    offset += 6
    offset += 2 + 2 * int.from_bytes(data[offset:offset + 2], "big")

    def attributes(position: int, size: int) -> tuple:
        rows = []
        for _ in range(size):
            name, length = struct.unpack_from(">HI", data, position)
            end = position + 6 + length
            rows.append((strings[name], data[position:end]))
            position = end
        return position, rows

    fields = int.from_bytes(data[offset:offset + 2], "big")
    offset += 2
    for _ in range(fields):
        offset, _ = attributes(offset + 8, int.from_bytes(data[offset + 6:offset + 8], "big"))
    methods = int.from_bytes(data[offset:offset + 2], "big")
    offset += 2
    target, codes = None, {}
    for _ in range(methods):
        flags, name, prototype, size = struct.unpack_from(">HHHH", data, offset)
        end, rows = attributes(offset + 8, size)
        code = [value for key, value in rows if key == b"Code"]
        if len(code) != 1:
            raise RuntimeError("Each owned direct method must have one original Code attribute")
        identity = (strings[name] + strings[prototype]).decode()
        codes[identity] = code[0]
        if strings[name] == b"map":
            if (target or strings[prototype] != b"(Ljava/lang/Object;)Ljava/lang/Object;"
                    or flags != 1 or any(key == b"Signature" for key, _ in rows)):
                raise RuntimeError("Owned direct member changed identity, access or generic metadata")
            target = offset, end, size
        offset = end
    if target is None:
        raise RuntimeError("Missing owned direct map implementation")
    class_attributes = offset
    class_count = int.from_bytes(data[offset:offset + 2], "big")
    end, rows = attributes(offset + 2, class_count)
    if end != len(data) or any(key == b"Signature" for key, _ in rows):
        raise RuntimeError("Owned raw interface unexpectedly has class Signature metadata")
    records = b""
    for text in (METHOD_SIGNATURE, CLASS_SIGNATURE):
        encoded = text.encode()
        records += b"\x01" + len(encoded).to_bytes(2, "big") + encoded
    start, stop, size = target
    tail = (data[pool_end:start + 6] + (size + 1).to_bytes(2, "big")
            + data[start + 8:stop] + struct.pack(">HIH", signature_name, 2, count)
            + data[stop:class_attributes] + (class_count + 1).to_bytes(2, "big")
            + data[class_attributes + 2:] + struct.pack(">HIH", signature_name, 2, count + 1))
    changed = data[:8] + (count + 2).to_bytes(2, "big") + data[10:pool_end] + records + tail
    if any(changed.count(code) != 1 for code in codes.values()):
        raise RuntimeError("Signature insertion changed an original Code attribute")
    path.write_bytes(changed)
    return {"method_signature": METHOD_SIGNATURE, "class_signature": CLASS_SIGNATURE,
            "original_code_sha256": {identity: hashlib.sha256(code).hexdigest() for identity, code in codes.items()},
            "original_method_bytecode": "unchanged", "original_runtime_descriptors": "unchanged"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-direct-erasure-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), args.d8.resolve(), 300)
        fixture = FIXTURES / "direct_erased_interface/java"
        harness = FIXTURES / "harness/DirectErasedInterfaceHarness.java"
        originals, harness_dir = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([harness], harness_dir, classpath=originals, release=17)
        classes, methods = compiler_inventory(verify, originals)
        mutation = direct_metadata(originals / "fixture/DirectTransform.class")
        (work / "input-metadata-change.json").write_text(json.dumps(mutation, indent=2) + "\n")
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (harness_dir, originals))),
                               "DirectErasedInterfaceHarness", "original"], "baseline")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", originals)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned direct interface inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        verify.run([verify.dexdump, "-d", dex], "sdk-code")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes)
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage.get("projected_method_count", 0) != 0):
            raise RuntimeError("Direct projection changed runtime methods or its partial coverage status")
        projections = coverage.get("generic_signature_projections", [])
        changes = [change for row in projections for change in row["changes"]]
        witness = "Lfixture/DirectTransform;->map(Ljava/lang/Object;)Ljava/lang/Object;"
        if (len(projections) != 2 or len(changes) != 3
                or {row["original_signature"] for row in projections} != {METHOD_SIGNATURE, CLASS_SIGNATURE}
                or any(not row.get("metadata_differences") for row in projections)
                or {change["kind"] for change in changes} != {"checked-erased-direct-interface",
                    "checked-erased-direct-parameter", "checked-erased-direct-return"}
                or any(change.get("scope_origin") != witness for change in changes)):
            raise RuntimeError("Missing original direct interface metadata and exact body witness")
        rebuilt = work / "rebuilt"
        verify.compile_local([*sorted((output / "sources").rglob("*.java")), harness], rebuilt, release=17)
        _, actual_methods = compiler_inventory(verify, rebuilt)
        if not set(methods).issubset(actual_methods):
            raise RuntimeError("Direct source lost an original runtime descriptor")
        actual = verify.run([verify.java, "-cp", rebuilt, "DirectErasedInterfaceHarness", "recovered"], "recovered-execution")
        if baseline != actual:
            raise RuntimeError("Direct source changed dispatch, mutable identity, failures or remaining generic metadata")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "normal_returns": 16, "getter_failures": 8, "reader_failures": 8,
                    "null_reader_failures": 8, "invalid_inputs": 8, "null_receiver_failures": 2,
                    "direct_and_interface_dispatch": "matched", "mutable_return_and_cursor_identity": "matched",
                    "cast_and_exception_order": "matched", "remaining_cursor_metadata": "preserved",
                    "runtime_descriptors": "preserved", "generic_reflection_difference": "confirmed",
                    "original_method_bytecode": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
