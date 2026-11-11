#!/usr/bin/env python3
"""Verify protected array/result and void bridges with original captured state."""
from __future__ import annotations

import argparse
import copy
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


def change_method_flags(path: Path, descriptor: bytes, before_flags: int, after_flags: int,
                        *, name: bytes = b"run") -> dict:
    before = path.read_bytes()
    shape = class_records(before)
    strings = shape["strings"]
    position = shape["pool_end"] + 6
    position += 2 + 2 * int.from_bytes(before[position:position + 2], "big")

    def skip_attributes(offset: int, count: int) -> int:
        for _ in range(count):
            offset += 6 + int.from_bytes(before[offset + 2:offset + 6], "big")
        return offset

    fields = int.from_bytes(before[position:position + 2], "big")
    position += 2
    for _ in range(fields):
        position = skip_attributes(position + 8, int.from_bytes(before[position + 6:position + 8], "big"))
    methods = int.from_bytes(before[position:position + 2], "big")
    position += 2
    selected = []
    for _ in range(methods):
        flags, method_name, signature, count = struct.unpack_from(">HHHH", before, position)
        if strings[method_name] == name and strings[signature] == descriptor:
            if flags != before_flags:
                raise RuntimeError("Owned protected run flags changed")
            selected.append(position)
        position = skip_attributes(position + 8, count)
    if len(selected) != 1:
        raise RuntimeError("Missing exact owned run declaration")
    offset = selected[0]
    changed = before[:offset] + after_flags.to_bytes(2, "big") + before[offset + 2:]
    if method_code(before) != method_code(changed):
        raise RuntimeError("Owned visibility edit changed Code or descriptors")
    path.write_bytes(changed)
    return {"method": name.decode() + descriptor.decode(), "original_flags": before_flags,
            "changed_flags": after_flags, "Code_Signatures_and_descriptors": "unchanged"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parent", action="store_true")
    parser.add_argument("--package-private", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-protected-array-callbacks-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "protected_array_callbacks/java").rglob("*.java"))
        harness_source = FIXTURES / "harness/ProtectedArrayCallbacksHarness.java"
        if args.package_private:
            changed_inputs = []
            for source in inputs:
                text = source.read_text()
                if text.count("protected ") != 3:
                    raise RuntimeError("Owned protected declarations changed")
                target = work / "package-java/fixture" / source.name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text.replace("protected ", ""))
                changed_inputs.append(target)
            inputs = changed_inputs
            text = harness_source.read_text()
            visibility = "!Modifier.isPublic(%s) && !Modifier.isProtected(%s) && !Modifier.isPrivate(%s)"
            before = "Modifier.isProtected(bridge.getModifiers()) && !Modifier.isPublic(bridge.getModifiers())"
            if text.count(before) != 1:
                raise RuntimeError("Owned bridge visibility oracle changed")
            text = text.replace(before, visibility % (("bridge.getModifiers()",) * 3), 1)
            before = "typed.isVarArgs() && Modifier.isProtected(typed.getModifiers())"
            if text.count(before) != 1:
                raise RuntimeError("Owned typed visibility oracle changed")
            text = text.replace(before, "typed.isVarArgs() && (" + visibility % (("typed.getModifiers()",) * 3) + ")", 1)
            text = text.replace("protected-array-and-void-dispatch=matched", "package-array-and-void-dispatch=matched", 1)
            harness_source = work / "ProtectedArrayCallbacksHarness.java"
            harness_source.write_text(text)
        verify.compile_local(inputs, originals)
        verify.compile_local([harness_source], harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        records = class_identity.compiler_classes(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - {"Lfixture/Base;"} if args.dependency_parent else all_classes
        methods = {k: v for k, v in all_methods.items() if k.split("->", 1)[0] in classes}
        dependency = work / "owned-parent.jar"

        def write_parent(root: Path, destination: Path) -> None:
            with zipfile.ZipFile(destination, "x", zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("fixture/Base.class", (root / "fixture/Base.class").read_bytes())

        write_parent(originals, dependency)
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "ProtectedArrayCallbacksHarness"], "baseline")
        summary = ("package" if args.package_private else "protected") + "-array-and-void-dispatch=matched,mutable-capture-and-constructor-order=matched"
        if baseline.splitlines() != ["values=66,identities=45,failures=24", summary]:
            raise RuntimeError("Owned protected bridge baseline missed expected cases: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--classpath", dependency, "--output", destination,
                        *[p for p in sorted(root.rglob("*.class"))
                          if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]], "d8-" + destination.name)
            if list(destination.glob("classes*.dex")) != [destination / "classes.dex"]:
                raise RuntimeError("D8 changed original protected bridge DEX inventory")
            return destination / "classes.dex"

        owned = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", owned], "inventory")
        inventory = parse_dexdump(dump, owned.name, dex_header(owned.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the independently authored protected declarations")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        classpath = ["--classpath", str(args.android_jar.resolve())]
        if args.dependency_parent:
            classpath += ["--classpath", str(dependency)]
        verify.run([args.neverd.resolve(), "mobile", owned, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        worker = "Lfixture/Owner$Worker;"
        bridge_ids = {worker + "->run([Ljava/lang/Object;)Ljava/lang/Object;",
                      worker + "->cancel(Ljava/lang/Object;)V", worker + "->finish(Ljava/lang/Object;)V"}
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes) or
                coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"] or
                coverage["status"] != "partial" or coverage["minimum_java_release"] != 25 or
                coverage.get("generic_signature_projections") or
                {k for k, r in rows.items() if r.get("projection_kind") == "generic-superclass-bridge"} != bridge_ids or
                any(r.get("projection_kind") == "public-erased-protected-getter-target" for r in rows.values())):
            raise RuntimeError("Protected bridges lost full inherited substitutions or changed target visibility")
        bindings = coverage.get("class_source_bindings", [])
        if (len(bindings) != 1 or bindings[0].get("binding_kind") != "inner-to-static-member" or
                bindings[0].get("class") != worker or not bindings[0].get("metadata_differences")):
            raise RuntimeError("Protected captured member lost its explicit source binding")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_parent else None, release=25)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("javac changed original inventory beyond reported helpers")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            wanted = copy.deepcopy(records[owner])
            if wanted["inner_class"]:
                wanted["inner_class"]["access"] |= 8
            if owner == worker:
                capture = worker + "->this$0:Lfixture/Owner;"
                if bindings[0].get("outer_capture_field") != capture:
                    raise RuntimeError("Captured owner changed")
                wanted["fields"][capture]["access"] &= ~0x1000
            after = actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != wanted.get(key):
                    raise RuntimeError("Original protected class metadata changed beyond capture flags: " + owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original protected method metadata changed: " + identity)
        runtime_path = (rebuilt, harness, dependency) if args.dependency_parent else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime_path)),
                             "ProtectedArrayCallbacksHarness"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Protected array/result dispatch, capture order or exceptions changed")

        labels = ["complete-arguments", "formal-scope", "public-contract", "bridge-flags"]
        if args.package_private:
            labels.append("private-target")
        for label in labels:
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "complete-arguments":
                receipt = change_signature(negative / "fixture/Owner$Worker.class",
                    b"Lfixture/Base<Ljava/lang/String;Ljava/lang/Integer;Ljava/lang/String;>;",
                    b"Lfixture/Base<Ljava/lang/Integer;Ljava/lang/String;Ljava/lang/String;>;")
            elif label == "formal-scope":
                receipt = change_signature(negative / "fixture/Base.class", b"([TP;)TR;", b"([TQ;)TR;")
            elif label == "public-contract":
                receipt = change_method_flags(negative / "fixture/Base.class",
                    b"([Ljava/lang/Object;)Ljava/lang/Object;", 0x480 if args.package_private else 0x484, 0x481)
            elif label == "private-target":
                receipt = change_method_flags(negative / "fixture/Owner$Worker.class",
                    b"([Ljava/lang/String;)Ljava/lang/String;", 0x80, 0x82)
            else:
                receipt = change_method_flags(negative / "fixture/Owner$Worker.class",
                    b"([Ljava/lang/Object;)Ljava/lang/Object;",
                    0x1040 if args.package_private else 0x1044,
                    0x40 if args.package_private else 0x44)
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
                raise RuntimeError("Protected bridge accepted changed visibility, scope or forwarding flags")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or
                hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash or
                hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_parent": args.dependency_parent, "package_private": args.package_private, "bridge_count": 3,
                    "value_cases": 66, "identity_cases": 45, "failure_cases": 24,
                    "bridge_visibility": "package" if args.package_private else "protected",
                    "protected_array_and_void_bridge_visibility": "not applicable" if args.package_private else "retained",
                    "constructor_order_and_mutable_capture": "matched",
                    "complete_metadata_and_inventory": "unchanged except reported capture flags and helpers",
                    "visibility_scope_and_bridge_flag_counterexamples": "rejected with original Code",
                    "original_input_harness_and_sdk_bytes": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
