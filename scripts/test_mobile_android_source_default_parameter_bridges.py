#!/usr/bin/env python3
"""Verify source interface parameter bridges against source and dependency roots."""
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
    from scripts.test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


BRIDGE = "Lfixture/Ordered;->compare(Ljava/lang/Object;)I"
TARGET = "Lfixture/Ordered;->compare(Lfixture/Ordered;)I"
DISPATCH = "Lfixture/Ordering;->compare(Ljava/lang/Object;)I"
BRIDGE_KEY = b"compare(Ljava/lang/Object;)I"
TARGET_KEY = b"compare(Lfixture/Ordered;)I"


def hashes(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


def change_bridge_load(path: Path, receiver: bool) -> dict:
    """Change exactly one load in the original interface forwarding bridge."""
    before = path.read_bytes()
    methods = method_code(before)
    code = methods[BRIDGE_KEY]
    length = int.from_bytes(code[10:14], "big")
    instructions = code[14:14 + length]
    if (length != 11 or instructions[:3] != b"\x2a\x2b\xc0"
            or instructions[5:6] != b"\xb9" or instructions[8:] != b"\x02\x00\xac"
            or before.count(code) != 1):
        raise RuntimeError("Missing exact javac interface cast and invokeinterface bridge")
    at = 14 if receiver else 15
    replacement = code[:at] + b"\x01" + code[at + 1:]
    after = before.replace(code, replacement, 1)
    wanted = dict(methods)
    wanted[BRIDGE_KEY] = replacement
    if (method_code(after) != wanted or len(before) != len(after)
            or sum(a != b for a, b in zip(before, after)) != 1):
        raise RuntimeError("Bridge counterexample changed unrelated Code or metadata")
    path.write_bytes(after)
    return {"method": BRIDGE, "change": "receiver to null" if receiver else "argument to null",
            "metadata_descriptors_and_other_Code": "unchanged",
            "before_sha256": hashlib.sha256(before).hexdigest(),
            "after_sha256": hashlib.sha256(after).hexdigest()}


def abstract_default(path: Path) -> dict:
    """Remove only the typed default Code and mark that original method abstract."""
    before = path.read_bytes()
    shape = class_records(before)
    at = shape["pool_end"] + 6
    at += 2 + 2 * int.from_bytes(before[at:at + 2], "big")

    def attributes(position: int, count: int) -> tuple:
        rows = []
        for _ in range(count):
            name, length = struct.unpack_from(">HI", before, position)
            end = position + 6 + length
            rows.append((shape["strings"][name], position, end))
            position = end
        return position, rows

    fields = int.from_bytes(before[at:at + 2], "big")
    at += 2
    for _ in range(fields):
        at, _ = attributes(at + 8, int.from_bytes(before[at + 6:at + 8], "big"))
    count = int.from_bytes(before[at:at + 2], "big")
    at += 2
    selected = []
    for _ in range(count):
        start = at
        flags, name, prototype, size = struct.unpack_from(">HHHH", before, at)
        at, rows = attributes(at + 8, size)
        if shape["strings"][name] + shape["strings"][prototype] == TARGET_KEY:
            code = [(begin, end) for key, begin, end in rows if key == b"Code"]
            if flags != 0x0001 or len(code) != 1:
                raise RuntimeError("Owned typed default flags or Code changed")
            selected.append((start, at, name, prototype, rows))
    if len(selected) != 1:
        raise RuntimeError("Missing unique owned typed interface default")
    start, stop, name, prototype, rows = selected[0]
    kept = [before[begin:end] for key, begin, end in rows if key != b"Code"]
    replacement = struct.pack(">HHHH", 0x0401, name, prototype, len(kept)) + b"".join(kept)
    after = before[:start] + replacement + before[stop:]
    wanted = dict(method_code(before))
    wanted[TARGET_KEY] = b""
    if method_code(after) != wanted or class_records(after)["strings"] != shape["strings"]:
        raise RuntimeError("Abstract default counterexample changed other Code or declarations")
    path.write_bytes(after)
    return {"method": TARGET, "change": "remove Code and set public abstract",
            "original_bridge_other_Code_descriptors_and_constant_pool": "unchanged"}


def add_interface(path: Path, name: bytes) -> dict:
    """Add one owned competing interface edge without changing original methods."""
    before = path.read_bytes()
    shape = class_records(before)
    count, end = shape["count"], shape["pool_end"]
    tail = before[end:]
    size = int.from_bytes(tail[6:8], "big")
    if size != 1 or name in shape["strings"].values():
        raise RuntimeError("Owned concrete interface edge shape changed")
    entries = b"\x01" + len(name).to_bytes(2, "big") + name + b"\x07" + count.to_bytes(2, "big")
    changed_tail = (tail[:6] + (size + 1).to_bytes(2, "big") + tail[8:8 + 2 * size]
                    + (count + 1).to_bytes(2, "big") + tail[8 + 2 * size:])
    after = before[:8] + (count + 2).to_bytes(2, "big") + before[10:end] + entries + changed_tail
    if method_code(after) != method_code(before):
        raise RuntimeError("Competing interface edge changed original Code or method descriptors")
    path.write_bytes(after)
    return {"added_interface": name.decode(), "original_Code_and_method_descriptors": "unchanged"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-root", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-source-default-parameter-bridges-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "source_default_parameter_bridges"
        inputs = sorted((fixture / "java").rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/SourceDefaultParameterBridgesHarness.java"],
                             harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        records = class_identity.compiler_classes(originals)
        if (records["Lfixture/Facade;"].get("signature") is not None
                or records["Lfixture/Receiver;"].get("signature") is not None
                or records["Lfixture/Facade;"]["methods"]):
            raise RuntimeError("Owned plain consumer path acquired a Signature or intermediate method")
        all_classes, all_methods = compiler_inventory(verify, originals)
        external = {"Lfixture/Ordering;"} if args.dependency_root else set()
        classes = all_classes - external
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        if (len(classes), len(methods)) != ((4, 6) if args.dependency_root else (5, 7)):
            raise RuntimeError("Owned source-default baseline inventory changed")
        dependency = work / "owned-ordering.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("fixture/Ordering.class", (originals / "fixture/Ordering.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_root:
            classpath += ["--classpath", dependency]

        def dex(root: Path, output: Path) -> Path:
            paths = [p for p in sorted(root.rglob("*.class"))
                     if "L" + p.relative_to(root).as_posix()[:-6] + ";" not in external]
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", output, *paths], "d8-" + output.name)
            result = output / "classes.dex"
            if not result.is_file() or list(output.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned source-default fixture did not produce exactly one DEX")
            return result

        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "SourceDefaultParameterBridgesHarness"], "baseline")
        expected = ["values=21,identities=139,states=59,failures=38",
                    "source-default-owner=retained,original-parameter-bridge=retained,virtual-order-and-cast-order=matched"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned source-default baseline missed a dispatch or failure case: " + baseline)
        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed the original source-default inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage.get("projected_method_count", 0) != 1
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != {BRIDGE}):
            raise RuntimeError("Source-default parameter bridge changed original scope or inventory")
        if (rows[BRIDGE].get("projection_kind") != "generic-interface-bridge"
                or rows[BRIDGE].get("bridge_target") != TARGET
                or rows[BRIDGE].get("bridge_dispatch") != DISPATCH):
            raise RuntimeError("Source-default bridge lacks its exact typed target and root contract: "
                               + json.dumps(rows[BRIDGE], sort_keys=True))
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_root else None,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != methods | helpers:
            raise RuntimeError("Recompile invented an implementation or unreported method")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            wanted, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != wanted.get(key):
                    raise RuntimeError("Original source-default class metadata changed: " + owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original source-default method metadata changed: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "SourceDefaultParameterBridgesHarness"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original default owner, parameter casts or virtual order effects changed")

        labels = ("bridge-argument", "bridge-receiver", "root-argument", "abstract-default",
                  "abstract-shadow", "competing-defaults")
        for label in labels:
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label in ("bridge-argument", "bridge-receiver"):
                receipt = change_bridge_load(negative / "fixture/Ordered.class", label == "bridge-receiver")
            elif label == "root-argument":
                receipt = change_signature(negative / "fixture/Ordered.class",
                    b"Ljava/lang/Object;Lfixture/Ordering<Lfixture/Ordered;>;",
                    b"Ljava/lang/Object;Lfixture/Ordering<Ljava/lang/String;>;")
            elif label == "abstract-default":
                receipt = abstract_default(negative / "fixture/Ordered.class")
            else:
                name = "Refined" if label == "abstract-shadow" else "Other"
                extra = work / (label + "-declarations")
                verify.compile_local([fixture / "negative/fixture" / (name + ".java")], extra,
                                     classpath=originals)
                target = negative / "fixture" / (name + ".class")
                shutil.copyfile(extra / "fixture" / (name + ".class"), target)
                if label == "abstract-shadow":
                    receipt = {"edge": change_signature(negative / "fixture/Receiver.class",
                                                        b"fixture/Facade", b"fixture/Refined")}
                    if BRIDGE_KEY in method_code(target.read_bytes()):
                        receipt["omitted_shadow_bridge"] = omit_owned_interface_bridge(target, BRIDGE_KEY)
                    if method_code(target.read_bytes()) != {TARGET_KEY: b""}:
                        raise RuntimeError("Owned abstract shadow must contain only its typed abstract declaration")
                    receipt["shadow_has_no_erased_bridge"] = True
                else:
                    receipt = add_interface(negative / "fixture/Receiver.class", b"fixture/Other")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if (result.returncode == 0 or rejected.exists()
                    or not any(word in result.stderr for word in ("generic", "bridge", "interface"))):
                raise RuntimeError("Source-default proof accepted invalid forwarding or hierarchy: " + label)

        for label in ("root-argument", "abstract-default", "abstract-shadow", "competing-defaults"):
            negative_java = work / (label + "-java")
            compiler_inputs = []
            source_inputs = inputs + ([fixture / "negative/fixture" / (
                "Refined.java" if label == "abstract-shadow" else "Other.java")]
                if label in ("abstract-shadow", "competing-defaults") else [])
            for source in source_inputs:
                content = source.read_text()
                if source.name == "Ordered.java" and label == "root-argument":
                    content = content.replace("Ordering<Ordered>", "Ordering<String>", 1)
                elif source.name == "Ordered.java" and label == "abstract-default":
                    prefix = content.index("    @Override\n    default int compare(")
                    content = content[:prefix] + "    @Override\n    int compare(Ordered other);\n}\n"
                elif source.name == "Receiver.java" and label in ("abstract-shadow", "competing-defaults"):
                    content = content.replace("implements Facade", "implements Refined" if label == "abstract-shadow"
                                              else "implements Facade, Other", 1)
                destination = negative_java / "fixture" / source.name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                compiler_inputs.append(str(destination))
            compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                       str(work / (label + "-compiler")), *compiler_inputs], text=True,
                                      capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-compiler.log")).write_text(compiler.stdout + compiler.stderr)
            if compiler.returncode == 0 or not any(word in compiler.stderr for word in ("clash", "override", "defaults")):
                raise RuntimeError("Independent javac accepted an invalid default hierarchy: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original classes, immutable harness or declaration bytes changed")
        evidence = {"status": "success", "dependency_root": args.dependency_root,
                    "class_count": len(classes), "method_count": len(methods), "projected_methods": 1,
                    "value_cases": 21, "identity_cases": 139, "state_cases": 59, "failure_cases": 38,
                    "original_source_default_owner_and_parameter_bridge": "retained",
                    "plain_intermediate_and_concrete_consumer_signatures": "absent and unchanged",
                    "mutable_values_and_dynamic_order_overrides": "matched",
                    "null_wrong_type_and_exception_order_and_identity": "matched",
                    "complete_original_metadata_and_inventory": "unchanged except reported helpers",
                    "same_code_full_root_argument_counterexample": "rejected",
                    "bridge_argument_and_receiver_Code_counterexamples": "rejected",
                    "abstract_default_bridge_free_shadow_and_competing_defaults": "rejected",
                    "independent_javac_invalid_hierarchies": "rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
