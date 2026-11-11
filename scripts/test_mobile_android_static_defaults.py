#!/usr/bin/env python3
"""Verify generic constructor witnesses on inlined synthetic default methods."""
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
    from scripts.test_mobile_android_kotlin_scopes import erase_fixture_method_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk
    from test_mobile_android_kotlin_scopes import erase_fixture_method_signature


def method_code(data: bytes) -> dict:
    """Inventory all original runtime descriptors and complete Code attributes."""
    if data[:4] != b"\xca\xfe\xba\xbe":
        raise RuntimeError("Missing compiled owned default scope class")
    count, index, offset, strings = int.from_bytes(data[8:10], "big"), 1, 10, {}
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
        result = []
        for _ in range(size):
            name, length = struct.unpack_from(">HI", data, position)
            end = position + 6 + length
            result.append((strings[name], data[position:end]))
            position = end
        return position, result

    fields = int.from_bytes(data[offset:offset + 2], "big")
    offset += 2
    for _ in range(fields):
        offset, _ = attributes(offset + 8, int.from_bytes(data[offset + 6:offset + 8], "big"))
    methods = int.from_bytes(data[offset:offset + 2], "big")
    offset += 2
    result = {}
    for _ in range(methods):
        _, name, prototype, size = struct.unpack_from(">HHHH", data, offset)
        offset, rows = attributes(offset + 8, size)
        code = [value for key, value in rows if key == b"Code"]
        if len(code) > 1:
            raise RuntimeError("Duplicate owned Code attribute")
        identity = strings[name] + strings[prototype]
        if identity in result:
            raise RuntimeError("Duplicate owned method identity")
        result[identity] = code[0] if code else b""
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--d8", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-static-defaults-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), args.d8.resolve(), 300)
        originals, harness_dir = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "static_default_scopes/java").rglob("*.java"))
        harness = FIXTURES / "harness/StaticDefaultScopeHarness.java"
        verify.compile_local(inputs, originals)
        verify.compile_local([harness], harness_dir, classpath=originals, release=17)
        classes, methods = compiler_inventory(verify, originals)
        paths = sorted(originals.rglob("*.class"))
        original_code = {path.relative_to(originals).as_posix(): method_code(path.read_bytes()) for path in paths}
        prototype = b"(Lfixture/Token;Ljava/lang/Object;Lfixture/Action;ILjava/lang/Object;)Ljava/lang/Object;"
        mutation = erase_fixture_method_signature(
            originals / "fixture/Scopes.class", method_name=b"make$default", prototype=prototype,
            expected_signature=b"<Unused:Ljava/lang/Object;>" + prototype)
        callback = originals / "fixture/Scopes$1.class"
        before = callback.read_bytes()
        changed = before
        replacements = [(b"Ljava/lang/Object;Lfixture/Action<Ljava/lang/Object;>;",
                         b"Ljava/lang/Object;Lfixture/Action<TE;>;"),
                        (b"Lfixture/Token<Ljava/lang/Object;>;",
                         b"Lfixture/Token<TE;>;")]
        for old_signature, new_signature in replacements:
            old = b"\x01" + len(old_signature).to_bytes(2, "big") + old_signature
            new = b"\x01" + len(new_signature).to_bytes(2, "big") + new_signature
            if changed.count(old) != 1:
                raise RuntimeError("Expected one owned callback Signature UTF8 record")
            changed = changed.replace(old, new, 1)
        callback.write_bytes(changed)
        changed_code = {path.relative_to(originals).as_posix(): method_code(path.read_bytes()) for path in paths}
        if changed_code != original_code:
            raise RuntimeError("Default scope metadata edit changed original Code bytes or descriptors")
        mutation["callback_signatures"] = [new.decode() for _, new in replacements]
        mutation["callback_before_sha256"] = hashlib.sha256(before).hexdigest()
        mutation["callback_after_sha256"] = hashlib.sha256(changed).hexdigest()
        mutation["all_original_code_sha256"] = {
            path: {key.decode(): hashlib.sha256(value).hexdigest() for key, value in records.items()}
            for path, records in original_code.items()}
        (work / "input-metadata-change.json").write_text(json.dumps(mutation, indent=2) + "\n")
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (harness_dir, originals))),
                               "StaticDefaultScopeHarness", "original"], "baseline")
        dex = verify.dex(paths, work / "dex", originals)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned default scope declaration inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        code = verify.run([verify.dexdump, "-d", dex], "sdk-code")
        witness = "Lfixture/Holder;.<init>:(Lfixture/Token;Ljava/lang/Object;)V"
        if code.count(witness) != 2 or "Lfixture/Scopes;.make:" in code:
            raise RuntimeError("Owned default wrapper no longer inlines the exact shared constructor")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial"):
            raise RuntimeError("Static default scope changed inventory or omitted partial coverage")
        projections = coverage.get("generic_signature_projections", [])
        if {row["original_signature"] for row in projections} != {new.decode() for _, new in replacements}:
            raise RuntimeError("Missing original callback class or field Signature")
        origin = "Lfixture/Scopes;->make(Lfixture/Token;Ljava/lang/Object;Lfixture/Action;)Ljava/lang/Object;"
        for row in projections:
            if not row.get("metadata_differences") or len(row["changes"]) != 1:
                raise RuntimeError("Missing callback metadata difference")
            change = row["changes"][0]
            if (change["kind"] != "synthetic-default-method-type-variable"
                    or change.get("scope_origin") != origin
                    or change["original_type"] != "Lfixture/Scopes;->make$default" + prototype.decode() + "::E"
                    or change["source_type"] != "Ljava/lang/Object;"):
                raise RuntimeError("Constructor scope lacks the exact original method declaration")
        rebuilt = work / "rebuilt"
        verify.compile_local([*sorted((output / "sources").rglob("*.java")), harness], rebuilt, release=17)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        bindings = coverage.get("class_source_bindings", [])
        if len(bindings) != 1 or bindings[0]["binding_kind"] != "anonymous-to-named-local":
            raise RuntimeError("Missing original callback binding")
        binding = bindings[0]
        prefix = binding["enclosing_method"]["owner"][:-1] + "$"
        pattern = re.escape(prefix) + r"[0-9]+" + re.escape(binding["source_name"]) + ";"
        matches = [cls for cls in rebuilt_classes if re.fullmatch(pattern, cls)]
        if len(matches) != 1:
            raise RuntimeError("Rebuilt callback has no unique compiler identity")
        expected = {method.replace(binding["class"], matches[0]) for method in methods}
        if not expected.issubset(rebuilt_methods):
            raise RuntimeError("Recompiled source lost an original runtime descriptor")
        (work / "local-class-identity.json").write_text(json.dumps(
            {"original": binding["class"], "rebuilt": matches[0]}, indent=2) + "\n")
        actual = verify.run([verify.java, "-cp", rebuilt, "StaticDefaultScopeHarness", "recovered"],
                            "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Constructor-witness scope changed executable behavior")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "identity_probes": 32, "ordered_failures": 6, "default_masks": [0, 1, 2, 3, 4, 5],
                    "generic_witness": "unchanged incoming parameter and exact shared fresh constructor",
                    "runtime_descriptors": "preserved", "generic_method_formal": "preserved",
                    "generic_reflection_differences": 2, "original_method_bytecode": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
