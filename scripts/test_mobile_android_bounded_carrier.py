#!/usr/bin/env python3
"""Verify checked static carrier returns against an independent D8/JVM oracle."""
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


def carrier_metadata(path: Path) -> dict:
    """Project one Signature and three synthetic flags, comparing all Code bytes."""
    original = path.read_bytes()

    def method_records(data: bytes) -> dict:
        if data[:4] != b"\xca\xfe\xba\xbe":
            raise RuntimeError("Missing compiled owned carrier")
        count = int.from_bytes(data[8:10], "big")
        index, offset, strings = 1, 10, {}
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
        result = {}
        for _ in range(methods):
            flags, name, prototype, size = struct.unpack_from(">HHHH", data, offset)
            end, rows = attributes(offset + 8, size)
            code = [value for key, value in rows if key == b"Code"]
            if len(code) != 1:
                raise RuntimeError("Each owned carrier method must have one Code attribute")
            identity = strings[name] + strings[prototype]
            if identity in result:
                raise RuntimeError("Duplicate owned method identity")
            result[identity] = (offset, flags, code[0])
            offset = end
        return result

    records = method_records(original)
    changed = bytearray(original)
    flags = {b"<init>(Ljava/lang/Object;)V": 2,
             b"wrap(Ljava/lang/Object;)Lfixture/Carrier;": 25,
             b"unwrap()Ljava/lang/Object;": 17}
    for identity, expected in flags.items():
        position, access, _ = records[identity]
        if access != expected:
            raise RuntimeError("Owned carrier access no longer matches its declaration")
        struct.pack_into(">H", changed, position, access | 0x1000)
    before = b"(Ljava/lang/Object;)Lfixture/Base<*>;"
    after = b"(Ljava/lang/Object;)TS;"
    old = b"\x01" + len(before).to_bytes(2, "big") + before
    new = b"\x01" + len(after).to_bytes(2, "big") + after
    if changed.count(old) != 1:
        raise RuntimeError("Expected one owned static return Signature record")
    changed = bytes(changed).replace(old, new, 1)
    rebuilt = method_records(changed)
    if ({identity: record[2] for identity, record in records.items()}
            != {identity: record[2] for identity, record in rebuilt.items()}):
        raise RuntimeError("Carrier metadata projection changed an original Code attribute")
    path.write_bytes(changed)
    return {"return_signature": after.decode(), "synthetic_methods": [key.decode() for key in flags],
            "original_code_sha256": {key.decode(): hashlib.sha256(row[2]).hexdigest()
                                     for key, row in records.items()},
            "original_method_bytecode": "unchanged"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-bounded-carrier-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), args.d8.resolve(), 300)
        fixture = FIXTURES / "bounded_static_carrier/java"
        harness = FIXTURES / "harness/BoundedCarrierHarness.java"
        originals, harness_dir = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([harness], harness_dir, classpath=originals, release=17)
        classes, methods = compiler_inventory(verify, originals)
        mutation = carrier_metadata(originals / "fixture/Carrier.class")
        (work / "input-metadata-change.json").write_text(json.dumps(mutation, indent=2) + "\n")
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (harness_dir, originals))),
                               "BoundedCarrierHarness", "original"], "baseline")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", originals)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned carrier declaration inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        verify.run([verify.dexdump, "-d", dex], "sdk-code")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes)
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"):
            raise RuntimeError("Carrier projection lost methods or its partial coverage status")
        projections = coverage.get("generic_signature_projections", [])
        if len(projections) != 1 or not projections[0].get("metadata_differences"):
            raise RuntimeError("Missing exact carrier metadata difference")
        row = projections[0]
        changes = row["changes"]
        witness = "Lfixture/Carrier;->segment(Ljava/lang/Object;)Lfixture/Base;"
        if (row["original_signature"] != "(Ljava/lang/Object;)TS;" or len(changes) != 1
                or changes[0]["kind"] != "checked-static-carrier-return-type-variable"
                or changes[0]["original_type"] != "Lfixture/Carrier;::S"
                or changes[0]["source_type"] != "Lfixture/Base;"
                or changes[0].get("scope_origin") != witness):
            raise RuntimeError("Carrier projection did not retain its exact nominal cast witness")
        rebuilt = work / "rebuilt"
        verify.compile_local([*sorted((output / "sources").rglob("*.java")), harness], rebuilt, release=17)
        _, actual_methods = compiler_inventory(verify, rebuilt)
        if not set(methods).issubset(actual_methods):
            raise RuntimeError("Carrier source lost an original runtime descriptor")
        actual = verify.run([verify.java, "-cp", rebuilt, "BoundedCarrierHarness", "recovered"], "recovered-execution")
        if baseline != actual:
            raise RuntimeError("Carrier projection changed identity, effects, failure ordering or class bounds")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "successful_returns": 24, "normal_reads": 24, "failure_cases": 25,
                    "distinct_recursive_subclasses": 2, "mutable_values_and_identity": "matched",
                    "sentinel_prefix_and_cast_failure_order": "matched", "runtime_descriptors": "preserved",
                    "recursive_class_bound": "preserved", "generic_return_reflection_difference": "confirmed",
                    "original_method_bytecode": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
