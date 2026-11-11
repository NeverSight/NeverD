#!/usr/bin/env python3
"""Verify erased bridge exceptions copied from original inherited contracts."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zipfile

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


def hashes(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


def change_owned_throws(path: Path, method: bytes, descriptor: bytes, flags: int, action: str) -> dict:
    before = path.read_bytes()
    shape = class_records(before)
    strings = shape["strings"]
    position = shape["pool_end"] + 6
    position += 2 + 2 * int.from_bytes(before[position:position + 2], "big")

    def attributes(offset: int, count: int) -> tuple:
        rows = []
        for _ in range(count):
            name, size = struct.unpack_from(">HI", before, offset)
            end = offset + 6 + size
            rows.append((strings[name], offset, end))
            offset = end
        return offset, rows

    fields = int.from_bytes(before[position:position + 2], "big")
    position += 2
    for _ in range(fields):
        position, _ = attributes(position + 8, int.from_bytes(before[position + 6:position + 8], "big"))
    methods = int.from_bytes(before[position:position + 2], "big")
    position += 2
    selected = []
    for _ in range(methods):
        actual_flags, name, prototype, count = struct.unpack_from(">HHHH", before, position)
        position, rows = attributes(position + 8, count)
        if strings[name] != method or strings[prototype] != descriptor:
            continue
        if actual_flags != flags:
            raise RuntimeError("Owned exception declaration flags changed")
        selected += [(start, end) for key, start, end in rows if key == b"Exceptions"]
    if len(selected) != 1:
        raise RuntimeError("Missing exact owned Exceptions attribute")
    start, end = selected[0]
    count = int.from_bytes(before[start + 6:start + 8], "big")
    if count not in (1, 2) or end - start != 8 + count * 2:
        raise RuntimeError("Owned exception inventory changed")
    entries = list(struct.unpack_from(">" + "H" * count, before, start + 8))
    if action == "order":
        if count != 2:
            raise RuntimeError("Missing two-entry owned exception order")
        entries.reverse()
    elif action == "drop":
        entries = entries[1:]
    else:
        raise RuntimeError("Unknown owned exception edit")
    payload = struct.pack(">H" + "H" * len(entries), len(entries), *entries)
    changed = before[:start] + before[start:start + 2] + struct.pack(">I", len(payload)) + payload + before[end:]
    if method_code(before) != method_code(changed):
        raise RuntimeError("Owned exception edit changed Code or runtime descriptors")
    path.write_bytes(changed)
    return {"method": method.decode() + descriptor.decode(), "original_count": count,
            "new_count": len(entries), "change": action, "Code_Signatures_and_descriptors": "unchanged"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parent", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-inherited-exception-bridges-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted((FIXTURES / "inherited_exception_bridges/java").rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/InheritedExceptionBridgesHarness.java"], harness,
                             classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        records = class_identity.compiler_classes(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - {"Lfixture/Echo;"} if args.dependency_parent else all_classes
        methods = {k: v for k, v in all_methods.items() if k.split("->", 1)[0] in classes}
        dependency = work / "owned-parent.jar"

        def write_parent(root: Path, destination: Path) -> None:
            with zipfile.ZipFile(destination, "x", zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("fixture/Echo.class", (root / "fixture/Echo.class").read_bytes())

        write_parent(originals, dependency)
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "InheritedExceptionBridgesHarness"], "baseline")
        if baseline.splitlines() != ["values=31,identities=36,failures=28",
                "original-bridge-exception-order=matched,typed-no-throws-and-dispatch=matched"]:
            raise RuntimeError("Owned inherited exception bridge baseline missed expected cases: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--classpath", dependency, "--output", destination,
                        *[p for p in sorted(root.rglob("*.class"))
                          if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]], "d8-" + destination.name)
            if list(destination.glob("classes*.dex")) != [destination / "classes.dex"]:
                raise RuntimeError("D8 changed original inherited exception bridge DEX inventory")
            return destination / "classes.dex"

        owned = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", owned], "inventory")
        inventory = parse_dexdump(dump, owned.name, dex_header(owned.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the independently authored inherited exception declarations")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        classpath = ["--classpath", str(args.android_jar.resolve())]
        if args.dependency_parent:
            classpath += ["--classpath", str(dependency)]
        verify.run([args.neverd.resolve(), "mobile", owned, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        worker = "Lfixture/Worker;"
        bridge_ids = {worker + "->call()Ljava/lang/Object;",
                      worker + "->echo(Ljava/lang/Object;)Ljava/lang/Object;"}
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes) or
                coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"] or
                coverage["status"] != "partial" or coverage.get("generic_signature_projections") or
                coverage.get("class_source_bindings") or
                {k for k, r in rows.items() if r["status"] == "source-projected"} != bridge_ids or
                any(rows[k].get("projection_kind") != "generic-interface-bridge" for k in bridge_ids)):
            raise RuntimeError("Inherited exceptions lost original bridge contracts or added source projections")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_parent else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("javac changed original inventory beyond reported helpers")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            wanted = records[owner]
            after = actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != wanted.get(key):
                    raise RuntimeError("Original inherited exception class metadata changed: " + owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original inherited exception method metadata changed: " + identity)
        runtime_path = (rebuilt, harness, dependency) if args.dependency_parent else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime_path)),
                             "InheritedExceptionBridgesHarness"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original exception order, checked dispatch, identities or effects changed")

        for label in ("complete-arguments", "call-exceptions", "echo-order", "echo-exceptions", "contract-exceptions"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "complete-arguments":
                receipt = change_signature(negative / "fixture/Worker.class",
                    b"Ljava/lang/Object;Ljava/util/concurrent/Callable<Ljava/lang/String;>;Lfixture/Echo<Ljava/lang/String;>;",
                    b"Ljava/lang/Object;Ljava/util/concurrent/Callable<Ljava/lang/String;>;Lfixture/Echo<Ljava/lang/Integer;>;")
            elif label == "call-exceptions":
                receipt = change_owned_throws(negative / "fixture/Worker.class", b"call", b"()Ljava/lang/Object;", 0x1041, "drop")
            else:
                target = "Echo" if label == "contract-exceptions" else "Worker"
                flags = 0x401 if target == "Echo" else 0x1041
                receipt = change_owned_throws(negative / ("fixture/" + target + ".class"), b"echo",
                    b"(Ljava/lang/Object;)Ljava/lang/Object;", flags, "order" if label == "echo-order" else "drop")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            negative_classpath = list(classpath)
            if args.dependency_parent:
                changed_parent = work / (label + "-parent.jar")
                write_parent(negative, changed_parent)
                negative_classpath[-1] = str(changed_parent)
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *negative_classpath, "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Inherited bridge accepted missing exceptions, changed order or full contract")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or
                hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash or
                hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_parent": args.dependency_parent, "bridge_count": 2,
                    "value_cases": 31, "identity_cases": 36, "failure_cases": 28,
                    "sdk_callable_and_original_bridge_exception_order": "matched",
                    "original_typed_no_throws": "retained",
                    "complete_metadata_and_inventory": "unchanged except reported helpers",
                    "missing_exceptions_order_scope_and_contract_counterexamples": "rejected with original Code",
                    "original_input_harness_and_sdk_bytes": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
