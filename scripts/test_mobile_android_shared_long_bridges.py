#!/usr/bin/env python3
"""Compare scoped original shared long callbacks with recovered JVM execution."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_shared_upper_bounds import class_records, owner_metadata
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_shared_upper_bounds import class_records, owner_metadata
    from test_mobile_android_static_defaults import method_code


def shared_metadata(path: Path, *, boxed: bytes = b"Ljava/lang/Long;") -> dict:
    """Encode one owned enclosure; preserve every original descriptor and Code."""
    original = path.read_bytes()
    old = b"<S:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Function;"
    new = b"Ljava/lang/Object;Lfixture/Function<" + boxed + b"TS;>;"
    signature = change_signature(path, old, new)
    data = path.read_bytes()
    shape = class_records(data)
    strings, classes = shape["strings"], shape["classes"]
    owners = [key for key, value in classes.items() if strings[value] == b"fixture/Owner"]
    if len(owners) != 1 or any(name == b"EnclosingMethod" for name, _, _ in shape["rows"]):
        raise RuntimeError("Owned shared callback enclosure changed")
    count, pool_end = shape["count"], shape["pool_end"]
    tail = bytearray(data[pool_end:])
    targets = 0
    for name, start, stop in shape["rows"]:
        if name != b"InnerClasses":
            continue
        size = int.from_bytes(data[start + 6:start + 8], "big")
        if stop != start + 8 + 8 * size:
            raise RuntimeError("Owned callback InnerClasses size changed")
        for index in range(size):
            at = start + 8 + 8 * index
            inner, outer, short_name, flags = struct.unpack_from(">HHHH", data, at)
            if inner != shape["this_class"]:
                continue
            if outer != owners[0] or strings[short_name] != b"Worker" or flags != 25:
                raise RuntimeError("Owned callback nesting identity changed")
            struct.pack_into(">HH", tail, at + 2 - pool_end, 0, 0)
            targets += 1
    if targets != 1:
        raise RuntimeError("Missing unique owned callback nesting row")
    struct.pack_into(">H", tail, shape["class_attributes"] - pool_end, shape["attribute_count"] + 1)
    records = b""
    for value in (b"EnclosingMethod", b"make", b"(Ljava/lang/Object;)Ljava/lang/Object;"):
        records += b"\x01" + len(value).to_bytes(2, "big") + value
    records += struct.pack(">BHH", 12, count + 1, count + 2)
    enclosure = struct.pack(">HIHH", count, 4, owners[0], count + 3)
    changed = data[:8] + (count + 4).to_bytes(2, "big") + data[10:pool_end] + records + tail + enclosure
    if method_code(original) != method_code(changed):
        raise RuntimeError("Owned enclosure edit changed Code or descriptors")
    path.write_bytes(changed)
    return {"signature": signature, "enclosing_method": "fixture.Owner.make(Object)",
            "original_code_sha256": {key.decode(): hashlib.sha256(code).hexdigest()
                                     for key, code in method_code(original).items()},
            "after_sha256": hashlib.sha256(changed).hexdigest()}


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
        prefix="neverd-shared-long-bridges-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness_dir = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "shared_long_reference_bridges/java").rglob("*.java"))
        inputs += [FIXTURES / "standard_long_reference_bridges/java/fixture/Function.java",
                   FIXTURES / "value_carrier_bridges/java/fixture/Trace.java"]
        verify.compile_local(inputs, originals)
        harness = "SharedLongReferenceBridgeHarness"
        verify.compile_local([FIXTURES / ("harness/" + harness + ".java")], harness_dir,
                             classpath=originals)
        worker = originals / "fixture/Owner$Worker.class"
        edits = {"worker": shared_metadata(worker),
                 "owner": owner_metadata(originals / "fixture/Owner.class"),
                 "flags": bridge_flag(worker, "invoke", "(Ljava/lang/Object;)Ljava/lang/Object;",
                                      original_flags=0x1, input_flags=0x1041)}
        (work / "owned-input-encoding.json").write_text(json.dumps(edits, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        hashes = {path.relative_to(originals).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in sorted(originals.rglob("*.class"))}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               harness, "original"], "baseline")
        if baseline.splitlines() != ["values=84,identities=90,failures=10", "reflection=original"]:
            raise RuntimeError("Owned shared callback baseline missed expected cases")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", args.android_jar.resolve())
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed shared callback original identities")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"):
            raise RuntimeError("Shared callback recovery lost original bodies or coverage")
        changes = [change for row in coverage.get("generic_signature_projections", [])
                   for change in row["changes"]]
        scopes = [change for change in changes if change["kind"] == "shared-method-type-variable"]
        if (len(scopes) != 4 or any(change["original_type"] !=
                "Lfixture/Owner;->make(Ljava/lang/Object;)Ljava/lang/Object;::S" for change in scopes)
                or sum(change["kind"] == "checked-value-carrier-interface" for change in changes) != 1):
            raise RuntimeError("Shared callback projection lost exact original enclosing scope")
        bridge = "Lfixture/Owner$Worker;->invoke(Ljava/lang/Object;)Ljava/lang/Object;"
        row = next(row for row in coverage["methods"] if row["identity"] == bridge)
        if (row.get("projection_kind") != "value-carrier-interface-bridge"
                or row.get("unbox_methods") != ["Ljava/lang/Number;->longValue()J"]
                or not row.get("metadata_differences")):
            raise RuntimeError("Shared callback lost original long conversion report")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir))),
                             harness, "recovered"], "recovered-execution")
        if actual.splitlines() != [baseline.splitlines()[0], "reflection=reported-shared-scope"]:
            raise RuntimeError("Shared callback changed values, identity, effects or exceptions")
        negative = work / "negative-originals"
        for path in originals.rglob("*.class"):
            target = negative / path.relative_to(originals)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(path.read_bytes())
        receipt = change_signature(negative / "fixture/Owner$Worker.class", b"(J)TS;", b"(J)TV;")
        (work / "negative-metadata-change.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = verify.dex(sorted(negative.rglob("*.class")), work / "negative-dex",
                                  args.android_jar.resolve())
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic inheritance" not in result.stderr:
            raise RuntimeError("Different enclosing formal with identical erasure and Code was accepted")
        if hashes != {path.relative_to(originals).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                      for path in sorted(originals.rglob("*.class"))}:
            raise RuntimeError("Owned original class files changed during verification")
        (work / "verification.json").write_text(json.dumps({"class_count": len(classes),
            "method_count": len(methods), "baseline": baseline.splitlines()[0],
            "original_code": "unchanged", "negative_scope": "rejected"}, indent=2) + "\n")
        print("shared-long-bridges: " + baseline.splitlines()[0])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
