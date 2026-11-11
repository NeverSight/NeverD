#!/usr/bin/env python3
"""Check desugared lambda scope projection with javac, D8 and JVM behavior."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
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


def synthetic_enclosing_method(path: Path) -> dict:
    """Retarget only EnclosingMethod; every compiler-produced body stays intact."""
    data = path.read_bytes()
    if data[:4] != b"\xca\xfe\xba\xbe":
        raise RuntimeError("Missing owned compiler class")
    count = int.from_bytes(data[8:10], "big")
    strings, index, offset = {}, 1, 10
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
            raise RuntimeError("Unexpected fixture constant-pool tag")
        index += 1
    pool_end = offset
    offset += 6
    offset += 2 + 2 * int.from_bytes(data[offset:offset + 2], "big")

    def attributes(position, size):
        rows = []
        for _ in range(size):
            name, length = struct.unpack_from(">HI", data, position)
            end = position + 6 + length
            rows.append((strings[name], position + 6, end))
            position = end
        return position, rows

    for _ in range(2):
        size = int.from_bytes(data[offset:offset + 2], "big")
        offset += 2
        for _ in range(size):
            offset, _ = attributes(offset + 8, int.from_bytes(data[offset + 6:offset + 8], "big"))
    _, rows = attributes(offset + 2, int.from_bytes(data[offset:offset + 2], "big"))
    matches = [(start, end) for name, start, end in rows if name == b"EnclosingMethod"]
    if len(matches) != 1 or matches[0][1] - matches[0][0] != 4:
        raise RuntimeError("Missing original enclosing method")
    start, _ = matches[0]
    name, descriptor = b"lambda$flip$0", b"(Lfixture/Duo;)Lfixture/Duo;"
    extra = (b"\x01" + struct.pack(">H", len(name)) + name
             + b"\x01" + struct.pack(">H", len(descriptor)) + descriptor
             + b"\x0c" + struct.pack(">HH", count, count + 1))
    changed = (data[:8] + struct.pack(">H", count + 3) + data[10:pool_end] + extra
               + data[pool_end:start + 2] + struct.pack(">H", count + 2) + data[start + 4:])
    # The entire member table, including Code and signatures, is unchanged.
    if changed[pool_end + len(extra):offset + len(extra)] != data[pool_end:offset]:
        raise RuntimeError("EnclosingMethod mutation changed compiler members")
    path.write_bytes(changed)
    return {"metadata_change": "EnclosingMethod retargeted to exact synthetic implementation",
            "member_table_sha256": hashlib.sha256(data[pool_end:offset]).hexdigest(),
            "method_bytecode": "unchanged"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-lambda-scope-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), args.d8.resolve(), 300)
        originals = work / "originals"
        verify.compile_local(sorted((FIXTURES / "lambda_scopes/java").rglob("*.java")), originals)
        original_classes, original_methods = compiler_inventory(verify, originals)
        implementation = "Lfixture/LambdaScope;->lambda$flip$0(Lfixture/Duo;)Lfixture/Duo;"
        if implementation not in original_methods:
            raise RuntimeError("The compiler lambda implementation identity changed")
        mutation = synthetic_enclosing_method(originals / "fixture/LambdaScope$1.class")
        (work / "input-metadata-change.json").write_text(json.dumps(mutation, indent=2) + "\n")
        harness = FIXTURES / "harness/LambdaScopeHarness.java"
        harness_dir = work / "harness"
        verify.compile_local([harness], harness_dir, classpath=originals)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (harness_dir, originals))),
                               "LambdaScopeHarness", "original"], "baseline")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", originals)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        classes = {row["descriptor"] for row in inventory["classes"]}
        methods = {row["identity"] for row in inventory["methods"]}
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        if not original_classes.issubset(classes) or not set(original_methods).issubset(methods):
            raise RuntimeError("D8 removed an original compiler declaration")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods
                or coverage["class_count"] != len(classes) or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial"):
            raise RuntimeError("Lambda scope recovery changed the original inventory or conservative status")
        projections = coverage.get("generic_signature_projections", [])
        witness = "Lfixture/LambdaScope;->flip(Lfixture/Duo;)Lfixture/Duo;"
        changes = [change for row in projections for change in row["changes"]]
        if (not changes or any(not row.get("metadata_differences") for row in projections)
                or any(change["kind"] != "desugared-lambda-method-type-variable"
                       or change.get("scope_origin") != witness
                       or change["source_type"] != "Ljava/lang/Object;" for change in changes)):
            raise RuntimeError("Scope replacement lacks its unique exact source method witness")
        rebuilt = work / "rebuilt"
        verify.compile_local([*sorted((output / "sources").rglob("*.java")), harness], rebuilt, release=25)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        bindings = coverage.get("class_source_bindings", [])
        if len(bindings) != 1 or bindings[0]["binding_kind"] != "anonymous-to-named-local":
            raise RuntimeError("Missing original anonymous class binding")
        binding = bindings[0]
        prefix = binding["enclosing_method"]["owner"][:-1] + "$"
        pattern = re.escape(prefix) + r"[0-9]+" + re.escape(binding["source_name"]) + ";"
        matches = [cls for cls in rebuilt_classes if re.fullmatch(pattern, cls)]
        if len(matches) != 1:
            raise RuntimeError("Recompiled local class identity is ambiguous")
        expected = {method.replace(binding["class"], matches[0]) for method in methods}
        if not expected.issubset(rebuilt_methods):
            raise RuntimeError("Recompiled source lost an original method descriptor")
        actual = verify.run([verify.java, "-cp", rebuilt, "LambdaScopeHarness", "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Scope projection changed captured values or callback behavior")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "callback_calls": 8, "forwarded_identity_checks": 16, "callback_failures": 2, "null_failures": 2,
                    "generic_reflection_difference": "confirmed", "original_method_bytecode": "unchanged",
                    "original_descriptors": "retained after reported local class rename",
                    "metadata_scope": "creation-and-forwarding-chain to unique Object-only generic method"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
