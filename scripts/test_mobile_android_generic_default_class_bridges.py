#!/usr/bin/env python3
"""Verify an inherited class bridge overrides a complete generic interface default."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


ANONYMOUS = "Lfixture/Owner$1;"
LISTENER_BRIDGE = "Lfixture/Base;->listener(Lfixture/Callback;)Lfixture/Contract;"
LISTENER_TARGET = "Lfixture/Base;->listener(Lfixture/Callback;)Lfixture/Base;"
LISTENER_DISPATCH = "Lfixture/Contract;->listener(Lfixture/Callback;)Lfixture/Contract;"
REMOVED_BRIDGE = ANONYMOUS + "->removed(Ljava/lang/Object;Ljava/lang/Object;)V"


def hashes(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


def change_bridge_argument(path: Path) -> dict:
    """Replace one owned bridge argument load with null; preserve all metadata."""
    before = path.read_bytes()
    methods = method_code(before)
    identity = b"listener(Lfixture/Callback;)Lfixture/Contract;"
    code = methods[identity]
    length = int.from_bytes(code[10:14], "big")
    instructions = code[14:14 + length]
    if (length != 6 or instructions[:3] != b"\x2a\x2b\xb6"
            or instructions[-1:] != b"\xb0" or before.count(code) != 1):
        raise RuntimeError("Missing exact original javac listener forwarding bridge")
    changed_code = code[:15] + b"\x01" + code[16:]
    changed = before.replace(code, changed_code, 1)
    wanted = dict(methods)
    wanted[identity] = changed_code
    if (method_code(changed) != wanted or len(before) != len(changed)
            or sum(a != b for a, b in zip(before, changed)) != 1):
        raise RuntimeError("Bridge argument counterexample changed unrelated Code or metadata")
    path.write_bytes(changed)
    return {"method": identity.decode(), "change": "aload_1 to aconst_null",
            "metadata_descriptors_and_other_Code": "unchanged",
            "before_sha256": hashlib.sha256(before).hexdigest(),
            "after_sha256": hashlib.sha256(changed).hexdigest()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path, help="JDK supporting Java release 25")
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-interface", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-generic-default-class-bridges-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "generic_default_class_bridges/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/GenericDefaultClassBridgesHarness.java"],
                             harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        records = class_identity.compiler_classes(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Contract;"} if args.dependency_interface else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        dependency = work / "owned-default.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("fixture/Contract.class", (originals / "fixture/Contract.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_interface:
            classpath += ["--classpath", dependency]

        def dex(root: Path, output: Path) -> Path:
            paths = [p for p in sorted(root.rglob("*.class"))
                     if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", output, *paths], "d8-" + output.name)
            result = output / "classes.dex"
            if not result.is_file() or list(output.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned generic default fixture did not produce one DEX")
            return result

        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "GenericDefaultClassBridgesHarness", "original"], "baseline")
        expected = ["identities=201,states=68,failures=35",
                    "ancestor-class-bridge=retained,default-never-dispatched-for-class=matched,captured-callback-order=matched"]
        if baseline.splitlines() != [*expected, "class-reflection=original-anonymous"]:
            raise RuntimeError("Owned generic default baseline missed a dispatch or ordered failure case")
        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed the original generic default inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {identity for identity in methods if identity.startswith(ANONYMOUS + "->")}
        projected.update({LISTENER_BRIDGE, "Lfixture/Owner;->create()Lfixture/Leaf;"})
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods)
                or coverage["projected_method_count"] != len(projected)
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release") != 25
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != projected):
            raise RuntimeError("Inherited class bridge lost original inventory or changed full generic scopes")
        if (rows[LISTENER_BRIDGE].get("projection_kind") != "generic-interface-bridge"
                or rows[LISTENER_BRIDGE].get("bridge_target") != LISTENER_TARGET
                or rows[LISTENER_BRIDGE].get("bridge_dispatch") != LISTENER_DISPATCH
                or rows[REMOVED_BRIDGE].get("projection_kind") != "generic-superclass-bridge"
                or rows[REMOVED_BRIDGE].get("bridge_target") != ANONYMOUS + "->removed(Ljava/lang/String;[B)V"
                or rows[REMOVED_BRIDGE].get("bridge_dispatch") != "Lfixture/Base;->removed(Ljava/lang/Object;Ljava/lang/Object;)V"):
            raise RuntimeError("Original listener or callback bridge has the wrong complete contract witness")
        bindings = coverage.get("class_source_bindings", [])
        if (len(bindings) != 1 or bindings[0].get("class") != ANONYMOUS
                or bindings[0].get("binding_kind") != "anonymous-to-named-local"
                or bindings[0].get("binary_name_status") != "changed"
                or not bindings[0].get("metadata_differences")):
            raise RuntimeError("Anonymous capture projection lacks its exact original lexical binding")
        binding = bindings[0]
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_interface else None, release=25)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        candidates = [owner for owner in actual_classes if owner.startswith("Lfixture/Owner$")
                      and owner.endswith(binding["source_name"] + ";")]
        if len(candidates) != 1:
            raise RuntimeError("Reported named local class does not identify one rebuilt binary")
        renamed = candidates[0]

        def mapped(text: str) -> str:
            return text.replace(ANONYMOUS, renamed)

        source_ids = {identity: mapped(identity) for identity in methods}
        hidden_outer = set()
        for identity, source_id in list(source_ids.items()):
            if source_id in actual_methods or not identity.startswith(ANONYMOUS + "-><init>("):
                continue
            with_outer = source_id.replace("-><init>(", "-><init>(Lfixture/Owner;", 1)
            if with_outer in actual_methods:
                source_ids[identity] = with_outer
                hidden_outer.add(identity)
        helpers = {mapped(row["class"] + "->" + row["name"] + row["prototype"])
                   for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != {mapped(owner) for owner in classes} or set(actual_methods) != set(source_ids.values()) | helpers:
            raise RuntimeError("Recompile changed inventory beyond reported class mapping, captures and helpers")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            wanted, after = records[owner], actual_records[mapped(owner)]
            for key in ("access", "signature", "superclass", "interfaces", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != wanted.get(key):
                    raise RuntimeError("Original generic class metadata changed: " + owner + ":" + key)
            expected_inner = copy.deepcopy(wanted["inner_class"])
            if owner == ANONYMOUS:
                expected_inner["inner"] = renamed
                expected_inner["name"] = binding["source_name"]
            if after["inner_class"] != expected_inner:
                raise RuntimeError("Original lexical ownership changed beyond the reported local name")
            fields = {mapped(identity): dict(metadata) for identity, metadata in wanted["fields"].items()}
            if owner == ANONYMOUS:
                for metadata in fields.values():
                    metadata["access"] &= ~0x1000
            actual_fields = dict(after["fields"])
            extra = set(actual_fields) - set(fields)
            if extra:
                if owner != ANONYMOUS or len(extra) != 1 or not hidden_outer:
                    raise RuntimeError("Unreported field appeared in captured source class")
                field = actual_fields.pop(next(iter(extra)))
                if field["descriptor"] != "Lfixture/Owner;" or field["access"] != 0x1010:
                    raise RuntimeError("Additional named-local capture is not its disclosed implicit owner")
            if actual_fields != fields:
                raise RuntimeError("Original captured field metadata changed beyond synthetic flags")
            for identity, metadata in wanted["methods"].items():
                expected_method = dict(metadata)
                if identity in hidden_outer:
                    expected_method["prototype"] = "(Lfixture/Owner;" + expected_method["prototype"][1:]
                if after["methods"][source_ids[identity]] != expected_method:
                    raise RuntimeError("Original method metadata changed beyond the disclosed local capture: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "GenericDefaultClassBridgesHarness", "rebuilt"], "rebuilt-execution")
        if actual.splitlines() != [*expected, "class-reflection=reported-named-local"]:
            raise RuntimeError("Inherited class/default dispatch, captures, casts or failure order changed")

        for label in ("callback-arguments", "ancestor-arguments", "bridge-argument"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "callback-arguments":
                receipt = change_signature(negative / "fixture/Base.class",
                    b"(Lfixture/Callback<TK;TV;>;)Lfixture/Base<TK;TV;>;",
                    b"(Lfixture/Callback<TV;TK;>;)Lfixture/Base<TK;TV;>;")
            elif label == "ancestor-arguments":
                receipt = change_signature(negative / "fixture/Middle.class",
                    b"<A:Ljava/lang/Object;B:Ljava/lang/Object;>Lfixture/Base<TA;TB;>;",
                    b"<A:Ljava/lang/Object;B:Ljava/lang/Object;>Lfixture/Base<TB;TA;>;")
            else:
                receipt = change_bridge_argument(negative / "fixture/Base.class")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Generic default handling accepted changed scopes or original forwarding")
        negative_java = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "Base.java":
                declaration = "public Base<K, V> listener(Callback<K, V> callback)"
                if content.count(declaration) != 1:
                    raise RuntimeError("Missing exact owned callback scope compiler counterexample")
                content = content.replace(declaration,
                    "public Base<K, V> listener(Callback<V, K> callback)", 1)
            destination = negative_java / source.relative_to(fixture)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or not any(word in compiler.stderr for word in ("clash", "override")):
            raise RuntimeError("Independent javac accepted a competing full callback argument scope")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original classes, immutable harness or default declaration bytes changed")
        evidence = {"status": "success", "dependency_interface": args.dependency_interface,
                    "class_count": len(classes), "method_count": len(methods), "bridge_count": 2,
                    "identity_cases": 201, "state_cases": 68, "failure_cases": 35,
                    "ancestor_class_bridge_and_complete_generic_default": "matched",
                    "descendant_listener_declarations": "not invented",
                    "class_paths_never_execute_default": "confirmed with failing default and independent control",
                    "original_callback_casts_and_captured_effect_order": "matched",
                    "complete_original_metadata_and_inventory": "only reported local name, capture metadata and helpers",
                    "full_generic_signatures": "unchanged",
                    "scope_and_bridge_forwarding_counterexamples": "rejected",
                    "independent_javac_callback_scope_counterexample": "rejected",
                    "original_input_harness_default_and_sdk_bytes": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
