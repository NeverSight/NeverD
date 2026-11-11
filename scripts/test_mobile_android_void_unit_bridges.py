#!/usr/bin/env python3
"""Check JVM-only void/Unit bridges without changing original method Code bytes."""
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def encode_void_bridge(path: Path, signature: bytes, *, rename_target: bool = True,
                       original_signature: bytes | None = None,
                       method_name: bytes = b"invoke") -> dict:
    """Name an owned void target and mark its original forwarding bridge."""
    original = path.read_bytes()
    old_name = b"\x01\x00\x03run"
    new_name = b"\x01" + len(method_name).to_bytes(2, "big") + method_name
    if rename_target and original.count(old_name) != 1:
        raise RuntimeError("Missing unique owned void target name")
    data = original.replace(old_name, new_name, 1) if rename_target else original
    if original_signature is not None:
        old = b"\x01" + len(original_signature).to_bytes(2, "big") + original_signature
        new = b"\x01" + len(signature).to_bytes(2, "big") + signature
        if data.count(old) != 1:
            raise RuntimeError("Missing exact owned raw generic class Signature")
        data = data.replace(old, new, 1)
    shape = class_records(data)
    if original_signature is None and any(name == b"Signature" for name, _, _ in shape["rows"]):
        raise RuntimeError("Raw owned implementation unexpectedly has a class Signature")
    pool_end, strings, count = shape["pool_end"], shape["strings"], shape["count"]
    tail = bytearray(data[pool_end:])
    offset = pool_end + 6
    offset += 2 + 2 * int.from_bytes(data[offset:offset + 2], "big")

    def end_of_member(position: int) -> int:
        attributes = int.from_bytes(data[position + 6:position + 8], "big")
        position += 8
        for _ in range(attributes):
            position += 6 + int.from_bytes(data[position + 2:position + 6], "big")
        return position

    fields = int.from_bytes(data[offset:offset + 2], "big")
    offset += 2
    for _ in range(fields):
        offset = end_of_member(offset)
    methods = int.from_bytes(data[offset:offset + 2], "big")
    offset += 2
    bridges = voids = 0
    for _ in range(methods):
        flags, name, prototype = struct.unpack_from(">HHH", data, offset)
        if strings[name] == method_name:
            if strings[prototype].endswith(b")Ljava/lang/Object;"):
                if flags != 1:
                    raise RuntimeError("Owned bridge is not an ordinary public method")
                struct.pack_into(">H", tail, offset - pool_end, 0x1041)
                bridges += 1
            elif strings[prototype].endswith(b")V"):
                voids += 1
        offset = end_of_member(offset)
    if bridges != 1 or voids != 1:
        raise RuntimeError("Missing unique void target and erased bridge")
    if original_signature is not None:
        changed = data[:pool_end] + tail
    else:
        struct.pack_into(">H", tail, shape["class_attributes"] - pool_end,
                         shape["attribute_count"] + 1)
        records = b""
        for value in (b"Signature", signature):
            records += b"\x01" + len(value).to_bytes(2, "big") + value
        attribute = struct.pack(">HIH", count, 2, count + 1)
        changed = data[:8] + (count + 2).to_bytes(2, "big") + data[10:pool_end] + records + tail + attribute
    before = method_code(original)
    expected = {(method_name + key[len(b"run"):]) if rename_target and key.startswith(b"run(") else key: code
                for key, code in before.items()}
    if expected != method_code(changed):
        raise RuntimeError("Owned bridge encoding changed Code bytes or runtime descriptors")
    path.write_bytes(changed)
    return {"signature": signature.decode(), "target_name": "run -> " + method_name.decode() if rename_target else method_name.decode() + " unchanged",
            "bridge_access": "public bridge synthetic", "code_and_descriptors": "unchanged",
            "before_sha256": hashlib.sha256(original).hexdigest(),
            "after_sha256": hashlib.sha256(changed).hexdigest(),
            "code_sha256": {key.decode(): hashlib.sha256(code).hexdigest() for key, code in expected.items()}}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-void-unit-bridges-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness_dir = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "void_unit_bridges/java").rglob("*.java"))
        verify.compile_local(inputs, originals)
        mutations = {}
        for name, signature in (("Worker", b"Ljava/lang/Object;Lfixture/Action<Lkotlin/Unit;>;"),
                                ("ArgWorker", b"Ljava/lang/Object;Lfixture/ArgAction<Lfixture/Value;Lkotlin/Unit;>;")):
            mutations[name] = encode_void_bridge(originals / ("fixture/" + name + ".class"), signature)
        (work / "owned-input-encoding.json").write_text(json.dumps(mutations, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        harness = FIXTURES / "harness/VoidUnitBridgeHarness.java"
        verify.compile_local([harness], harness_dir, classpath=originals)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               "VoidUnitBridgeHarness", "invoke", "invoke"], "baseline")
        if baseline.splitlines() != ["identities=27,failures=9,initialization-and-calls=matched",
                                     "reflection=original"]:
            raise RuntimeError("Owned void/Unit baseline did not exercise all expected cases")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", args.android_jar.resolve())
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned void/Unit input inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["projected_method_count"] != 4
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"):
            raise RuntimeError("Void/Unit bridge inventory or conservative coverage changed")
        target_names = {}
        expected = set(methods)
        for row in coverage["methods"]:
            if row["status"] != "source-projected":
                continue
            if (row["projection_kind"] not in ("void-unit-target", "void-unit-interface-bridge")
                    or not row.get("metadata_differences") or not row.get("source_identity")):
                raise RuntimeError("Void/Unit source mapping is not explicit")
            if row["projection_kind"] == "void-unit-target":
                target_names[row["class"]] = row["bridge_source_name"]
                expected.remove(row["identity"])
                expected.add(row["source_identity"])
            else:
                expected.add(row["source_identity"])
                if (row["bridge_target"] not in methods or row["bridge_dispatch"] not in methods
                        or row["result_field"] != "Lkotlin/Unit;->INSTANCE:Lkotlin/Unit;"):
                    raise RuntimeError("Void/Unit bridge lost its exact body witness")
        if target_names.get("Lfixture/Worker;") != "__neverdVoidBridgeTarget_":
            raise RuntimeError("Generated void target name collided with an original declaration")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if actual_classes != classes or not expected.issubset(actual_methods):
            raise RuntimeError("Rebuilt source lost descriptors after the recorded void method renames")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir))),
                             "VoidUnitBridgeHarness", target_names["Lfixture/Worker;"],
                             target_names["Lfixture/ArgWorker;"]], "recovered-execution")
        before, after = baseline.splitlines(), actual.splitlines()
        if (before[:-1] != after[:-1] or before[-1] != "reflection=original"
                or after[-1] != "reflection=reported-source-mapping"):
            raise RuntimeError("Unit source changed initialization, casts, call effects, identity or reflection")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "jvm_output": "matched except confirmed reflection mapping", "bridge_count": 2,
                    "identity_cases": 27, "ordered_failure_cases": 9,
                    "typed_void_calls": "no Unit read", "unit_initialization_after_successful_call": "matched",
                    "parameter_cast_before_target_effects": "matched", "exceptions_and_mutable_identity": "matched",
                    "code_bytes_and_runtime_descriptors": "unchanged owned input", "source_method_renames": "recorded",
                    "original_erased_bridges": "regenerated", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
