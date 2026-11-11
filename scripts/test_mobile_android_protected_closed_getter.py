#!/usr/bin/env python3
"""Verify a protected closed getter with an original public erased bridge."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def encode_public_erased_entry(path: Path, *, name: bytes = b"initialValue",
                              prototype: bytes = b"()Ljava/lang/Object;") -> dict:
    """Change only the independently authored getter's erased access bits."""
    original = path.read_bytes()
    shape = class_records(original)
    strings = shape["strings"]
    offset = shape["pool_end"] + 6
    offset += 2 + 2 * int.from_bytes(original[offset:offset + 2], "big")

    def member_end(position: int) -> int:
        size = int.from_bytes(original[position + 6:position + 8], "big")
        position += 8
        for _ in range(size):
            position += 6 + int.from_bytes(original[position + 2:position + 6], "big")
        return position

    fields = int.from_bytes(original[offset:offset + 2], "big")
    offset += 2
    for _ in range(fields):
        offset = member_end(offset)
    methods = int.from_bytes(original[offset:offset + 2], "big")
    offset += 2
    targets = []
    for _ in range(methods):
        access, method_name, method_prototype = struct.unpack_from(">HHH", original, offset)
        if strings[method_name] == name and strings[method_prototype] == prototype:
            if access != 0x1044:
                raise RuntimeError("Owned erased getter access changed")
            targets.append(offset)
        offset = member_end(offset)
    if len(targets) != 1:
        raise RuntimeError("Owned erased getter is not unique")
    position = targets[0]
    changed = original[:position] + struct.pack(">H", 0x1041) + original[position + 2:]
    if method_code(original) != method_code(changed):
        raise RuntimeError("Erased access edit changed Code or descriptors")
    path.write_bytes(changed)
    return {"original_flags": 0x1044, "input_flags": 0x1041,
            "code_and_all_other_metadata": "unchanged",
            "before_sha256": hashlib.sha256(original).hexdigest(),
            "after_sha256": hashlib.sha256(changed).hexdigest()}


def hashes(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-protected-closed-getter-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = FIXTURES / "protected_closed_getter/java"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/ProtectedClosedGetterHarness.java"], harness,
                             classpath=originals)
        owner = "Lfixture/OwnedLocal;"
        bridge = owner + "->initialValue()Ljava/lang/Object;"
        target_method = owner + "->initialValue()Ljava/lang/String;"
        bridges = {bridge, target_method}

        def encode(root: Path) -> list:
            target = root / "fixture/OwnedLocal.class"
            return [encode_public_erased_entry(target)]

        (work / "owned-input-flags.json").write_text(json.dumps(encode(originals), indent=2) + "\n")
        records = class_identity.compiler_classes(originals)
        classes, methods = compiler_inventory(verify, originals)
        if classes != {owner} or len(methods) != 4:
            raise RuntimeError("Owned protected-getter declaration inventory changed")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "ProtectedClosedGetterHarness", "original"], "baseline")
        lines = baseline.splitlines()
        cases = re.fullmatch(r"values=(\d+),identities=(\d+),failures=(\d+)", lines[0]) if lines else None
        if (not cases or int(cases[1]) != 965 or int(cases[2]) != 965 or int(cases[3]) != 6 or
                lines[1:] != ["public-erased-entry-and-thread-local-state=matched,target-access=reported"]):
            raise RuntimeError("Owned protected-getter baseline missed getter state or erased dispatch cases: " + baseline)

        def make_dex(root: Path, output: Path) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", output, *sorted(root.rglob("*.class"))], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned protected-getter DEX inventory changed")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original protected-getter declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != set(methods) or coverage["class_count"] != 1 or coverage["method_count"] != 4 or
                coverage["unrecovered_method_count"] or coverage["status"] != "partial" or
                coverage.get("generic_signature_projections") or
                {r["identity"] for r in rows.values() if r["status"] == "source-projected"} != bridges or
                rows[target_method].get("projection_kind") != "public-erased-protected-getter-target" or
                any(not rows[key].get("metadata_differences") for key in bridges)):
            raise RuntimeError("Protected-getter recovery lost its exact superclass or forwarding receipts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("javac changed inventory beyond reported helpers")
        actual_records = class_identity.compiler_classes(rebuilt)
        before, after = records[owner], actual_records[owner]
        for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
            if after.get(key) != before.get(key):
                raise RuntimeError("Protected-getter original class metadata changed: " + key)
        for identity, metadata in before["methods"].items():
            wanted = dict(metadata)
            if identity == target_method:
                wanted["access"] = 0x1
            if after["methods"][identity] != wanted:
                raise RuntimeError("Protected-getter method metadata changed beyond target access: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "ProtectedClosedGetterHarness", "rebuilt"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Getter state, public erased dispatch, object identity or exceptions changed")

        def reject(root: Path, label: str) -> None:
            negative_dex = make_dex(root, work / ("negative-" + label + "-dex"))
            rejected = work / ("rejected-" + label)
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                     str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / ("negative-" + label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Protected-getter proof accepted changed inherited arguments or a conflicting source descendant")

        negative = work / "negative-scope"
        shutil.copytree(originals, negative)
        receipt = change_signature(negative / "fixture/OwnedLocal.class",
                                   b"Ljava/lang/ThreadLocal<Ljava/lang/String;>;",
                                   b"Ljava/lang/ThreadLocal<Ljava/lang/Integer;>;")
        (work / "negative-scope-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        reject(negative, "scope")
        negative_sources = work / "negative-owner-java"
        shutil.copytree(fixture, negative_sources)
        target = negative_sources / "fixture/OwnedLocal.java"
        text = target.read_text()
        statement = "public final class OwnedLocal"
        if text.count(statement) != 1:
            raise RuntimeError("Missing exact independently authored final getter owner")
        target.write_text(text.replace(statement, "public class OwnedLocal", 1))
        (negative_sources / "fixture/GetterChild.java").write_text(
            "package fixture; public final class GetterChild extends OwnedLocal {\n"
            "  @Override protected String initialValue() { return \"owned child\"; }\n}\n")
        negative_owner = work / "negative-owner"
        verify.compile_local(sorted(negative_sources.rglob("*.java")), negative_owner)
        (work / "negative-owner-flags.json").write_text(json.dumps(encode(negative_owner), indent=2) + "\n")
        reject(negative_owner, "owner")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or
                hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original input, SDK declarations or immutable harness bytes changed")
        evidence = {"status": "success", "class_count": 1, "method_count": 4,
                    "value_cases": int(cases[1]), "identity_cases": int(cases[2]), "failure_cases": 6,
                    "original_public_erased_getter_and_thread_local_state": "matched",
                    "target_access_projection": "protected to public",
                    "full_metadata_and_complete_inventory": "unchanged except reported target access and helpers",
                    "scope_and_source_descendant_counterexamples": "rejected",
                    "input_sdk_and_immutable_harness": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
