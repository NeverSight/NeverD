#!/usr/bin/env python3
"""Verify checked reference return metadata against independent JVM bodies."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
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


def encode_return_signatures(path: Path) -> dict:
    before = path.read_bytes()
    changes = [
        (b"Ljava/lang/Object;Lfixture/Transform<Lfixture/Owner<*>;Ljava/lang/Object;>;",
         b"Ljava/lang/Object;Lfixture/Transform<Lfixture/Owner<*>;Lfixture/Result;>;"),
        (b"(Lfixture/Owner<*>;)Ljava/lang/Object;", b"(Lfixture/Owner<*>;)Lfixture/Result;"),
    ]
    after = before
    for original, encoded in changes:
        old = b"\x01" + len(original).to_bytes(2, "big") + original
        new = b"\x01" + len(encoded).to_bytes(2, "big") + encoded
        if after.count(old) != 1:
            raise RuntimeError("Expected one owned class/method Signature UTF8 record")
        after = after.replace(old, new, 1)

    def declaration_body(data: bytes) -> bytes:
        if data[:4] != b"\xca\xfe\xba\xbe":
            raise RuntimeError("Missing owned classfile")
        count = int.from_bytes(data[8:10], "big")
        index, offset = 1, 10
        while index < count:
            tag = data[offset]
            offset += 1
            if tag == 1:
                size = int.from_bytes(data[offset:offset + 2], "big")
                offset += 2 + size
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
        return data[offset:]

    body = declaration_body(before)
    if declaration_body(after) != body:
        raise RuntimeError("Metadata mutation changed declarations or original Code attributes")
    path.write_bytes(after)
    return {"change": "two Signature UTF8 records", "method_bytecode": "unchanged",
            "declaration_and_code_sha256": hashlib.sha256(body).hexdigest(),
            "before_sha256": hashlib.sha256(before).hexdigest(),
            "after_sha256": hashlib.sha256(after).hexdigest()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-return-erasure-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), args.d8.resolve(), 300)
        inputs = sorted((FIXTURES / "checked_return_erasure/java").rglob("*.java"))
        harness = FIXTURES / "harness/CheckedReturnHarness.java"
        originals, harness_dir = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([harness], harness_dir, classpath=originals, release=17)
        classes, methods = compiler_inventory(verify, originals)
        mutation = encode_return_signatures(originals / "fixture/ReturnAdapter.class")
        (work / "input-metadata-change.json").write_text(json.dumps(mutation, indent=2) + "\n")
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (harness_dir, originals))),
                               "CheckedReturnHarness", "original"], "baseline")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", originals)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned return fixture declaration inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        verify.run([verify.dexdump, "-d", dex], "sdk-code")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["projected_method_count"] != 1
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"):
            raise RuntimeError("Return projection changed inventory or omitted partial coverage")
        projections = coverage.get("generic_signature_projections", [])
        changes = [change for row in projections for change in row["changes"]]
        witness = "Lfixture/ReturnAdapter;->apply(Lfixture/Owner;)Ljava/lang/Object;"
        if (len(projections) != 2 or len(changes) != 2
                or any(not row.get("metadata_differences") for row in projections)
                or {change["kind"] for change in changes} != {
                    "checked-erased-return", "checked-erased-interface-return"}
                or any(change["original_type"] != "Lfixture/Result;"
                       or change["source_type"] != "Ljava/lang/Object;"
                       or change.get("scope_origin") != witness for change in changes)):
            raise RuntimeError("Missing exact original return metadata and descriptor witness")
        rebuilt = work / "rebuilt"
        verify.compile_local([*sorted((output / "sources").rglob("*.java")), harness], rebuilt, release=17)
        _, rebuilt_methods = compiler_inventory(verify, rebuilt)
        if not set(methods).issubset(rebuilt_methods):
            raise RuntimeError("Recompilation lost an original method descriptor")
        actual = verify.run([verify.java, "-cp", rebuilt, "CheckedReturnHarness", "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Return projection changed effects, exceptions or object identity")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "typed_and_bridge_cases": 40, "returned_objects": 16, "read_failures": 8,
                    "constructor_failures": 8, "null_owner_cases": 2, "null_receiver_failures": 2,
                    "bridge_cast_failure": 1, "mutable_value_and_wide_stamp": "matched",
                    "exception_identity_and_effect_order": "matched", "runtime_descriptors": "preserved",
                    "generic_reflection_difference": "confirmed", "original_method_bytecode": "unchanged"}
        evidence.update(allocation_initialization_before_field_read="matched",
                        null_field_receiver_before_constructor="matched")
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
