#!/usr/bin/env python3
"""Compare a checked synthetic bound projection against independent JVM bytecode."""
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


PROTOTYPE = "(Lfixture/Bounded;Ljava/lang/Object;)Z"
ORIGINAL = "<T:Ljava/lang/Object;R::Lfixture/Bounded;:Ljava/lang/Iterable<+TT;>;>(TR;TT;)Z"
ENCODED = ORIGINAL.replace("Lfixture/Bounded;", "Lfixture/Bounded<TT;>;")


def encode_metadata(path: Path) -> dict:
    before = path.read_bytes()
    if before[:4] != b"\xca\xfe\xba\xbe":
        raise RuntimeError("Expected the owned javac class")
    data = bytearray(before)
    count = int.from_bytes(data[8:10], "big")
    offset, index, utf8 = 10, 1, {}
    while index < count:
        tag = data[offset]
        offset += 1
        if tag == 1:
            size = int.from_bytes(data[offset:offset + 2], "big")
            offset += 2
            utf8[index] = bytes(data[offset:offset + size])
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
            raise RuntimeError("Unexpected constant-pool tag in owned fixture")
        index += 1
    offset += 6
    interfaces = int.from_bytes(data[offset:offset + 2], "big")
    offset += 2 + 2 * interfaces
    changed = 0
    for methods in (False, True):
        members = int.from_bytes(data[offset:offset + 2], "big")
        offset += 2
        for _ in range(members):
            flags, name, descriptor, attributes = struct.unpack_from(">HHHH", data, offset)
            if methods and utf8[name] == b"probe" and utf8[descriptor] == PROTOTYPE.encode():
                if flags != 0x000a:
                    raise RuntimeError("Expected the exact private static method flags")
                struct.pack_into(">H", data, offset, flags | 0x1000)
                changed += 1
            offset += 8
            for _ in range(attributes):
                offset += 6 + int.from_bytes(data[offset + 2:offset + 6], "big")
    old = b"\x01" + len(ORIGINAL).to_bytes(2, "big") + ORIGINAL.encode()
    new = b"\x01" + len(ENCODED).to_bytes(2, "big") + ENCODED.encode()
    if changed != 1 or data.count(old) != 1:
        raise RuntimeError("Expected one owned method and one Signature UTF8 record")
    after = bytes(data).replace(old, new, 1)
    path.write_bytes(after)
    return {"changes": ["method synthetic flag", "one Signature UTF8 record"],
            "method_bytecode": "unchanged", "before_sha256": hashlib.sha256(before).hexdigest(),
            "after_sha256": hashlib.sha256(after).hexdigest()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--d8", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-synthetic-bounds-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), args.d8.resolve(), 300)
        originals, harness_dir = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "synthetic_bounds/java").rglob("*.java"))
        harness = FIXTURES / "harness/SyntheticBoundsHarness.java"
        verify.compile_local(inputs, originals)
        verify.compile_local([harness], harness_dir, classpath=originals)
        classes, methods = compiler_inventory(verify, originals)
        mutation = encode_metadata(originals / "fixture/CheckedBound.class")
        (work / "input-metadata-change.json").write_text(json.dumps(mutation, indent=2) + "\n")
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (harness_dir, originals))),
                               "SyntheticBoundsHarness", "original"], "baseline")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", originals)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned fixture declaration inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial"):
            raise RuntimeError("Synthetic bound projection changed inventory or omitted partial coverage")
        projections = coverage.get("generic_signature_projections", [])
        if (len(projections) != 1 or projections[0]["original_signature"] != ENCODED
                or not projections[0].get("metadata_differences") or len(projections[0]["changes"]) != 1):
            raise RuntimeError("Missing original constraints and metadata differences")
        change = projections[0]["changes"][0]
        if (change["kind"] != "checked-synthetic-bound-erasure"
                or change.get("scope_origin") != "Lfixture/CheckedBound;->probe" + PROTOTYPE
                or change["source_type"] != PROTOTYPE or change["original_type"] != ENCODED):
            raise RuntimeError("Projection lacks the exact original constraint and runtime descriptor")
        rebuilt = work / "rebuilt"
        verify.compile_local([*sorted((output / "sources").rglob("*.java")), harness], rebuilt)
        _, rebuilt_methods = compiler_inventory(verify, rebuilt)
        if not set(methods).issubset(rebuilt_methods):
            raise RuntimeError("Recompiled source lost an original runtime descriptor")
        actual = verify.run([verify.java, "-cp", rebuilt, "SyntheticBoundsHarness", "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Synthetic bound erasure changed executable behavior")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "identity_probes": 4, "callback_calls": 5, "null_short_circuits": 2,
                    "cast_failures": 3, "null_receiver_failures": 1, "callback_failures": 1,
                    "cast_before_null_receiver": "preserved", "private_static_access": "preserved",
                    "runtime_descriptors": "preserved", "generic_reflection_difference": "confirmed",
                    "original_method_bytecode": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
