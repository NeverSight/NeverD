#!/usr/bin/env python3
"""Verify Boolean result boxing with distinct original JVM return entries."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
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
    from scripts.test_mobile_android_final_getter_return_slots import rename_owned_entry
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_final_getter_return_slots import rename_owned_entry
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag


RAW_SIGNATURE = b"<S:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Function;"
INPUT_SIGNATURE = (b"<S:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;"
                   b"Lfixture/Function<Ljava/lang/Boolean;>;")
BRIDGE = "Lfixture/Receiver;->invoke()Ljava/lang/Object;"
TARGET = "Lfixture/Receiver;->invoke()Z"
DISPATCH = "Lfixture/Function;->invoke()Ljava/lang/Object;"


def hashes(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-interface", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-boolean-result-bridges-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "boolean_result_bridges/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        # The harness is compiled before the input names, Signature and flags are
        # encoded, and is never rewritten or rebuilt against recovered source.
        verify.compile_local([FIXTURES / "harness/BooleanResultBridgesHarness.java"],
                             harness, classpath=originals)
        harness_hashes = hashes(harness)
        edits = {
            "signature": change_signature(originals / "fixture/Receiver.class",
                                          RAW_SIGNATURE, INPUT_SIGNATURE),
            "flags": bridge_flag(originals / "fixture/Receiver.class", "invoke",
                                 "()Ljava/lang/Object;", original_flags=0x1, input_flags=0x1041),
            "names": {owner: rename_owned_entry(originals / ("fixture/" + owner + ".class"),
                                                  b"primitiveInvoke", b"invoke")
                      for owner in ("Receiver", "Caller")}}
        (work / "owned-input-encoding.json").write_text(json.dumps(edits, indent=2) + "\n")
        original_hashes = hashes(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        classes = all_classes - ({"Lfixture/Function;"} if args.dependency_interface else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        dependency = work / "owned-interface.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("fixture/Function.class", (originals / "fixture/Function.class").read_bytes())
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
                raise RuntimeError("Owned Boolean callback did not produce one DEX")
            return result

        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "BooleanResultBridgesHarness", "original", "invoke"], "baseline")
        expected_counts = "values=81,identities=53,states=90,failures=10"
        if baseline.splitlines() != [expected_counts, "reflection=original"]:
            raise RuntimeError("Owned Boolean baseline missed a dispatch, identity or ordered effect")
        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed the original Boolean callback inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["projected_method_count"] != 2
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"):
            raise RuntimeError("Boolean callback recovery lost original entries or conservative coverage")
        projections = coverage.get("generic_signature_projections", [])
        if (len(projections) != 1 or not projections[0].get("metadata_differences")
                or projections[0].get("original_signature") != INPUT_SIGNATURE.decode()
                or len(projections[0]["changes"]) != 1):
            raise RuntimeError("Boolean callback projection lost the original full class Signature")
        change = projections[0]["changes"][0]
        if (change["kind"] != "checked-value-carrier-interface" or change["scope_origin"] != BRIDGE
                or change["original_type"] != "Lfixture/Function<Ljava/lang/Boolean;>;"
                or change["source_type"] != "Lfixture/Function;"):
            raise RuntimeError("Boolean callback raw interface lacks the original boxing witness")
        bridge, target = rows[BRIDGE], rows[TARGET]
        if (bridge.get("projection_kind") != "value-carrier-interface-bridge"
                or bridge.get("source_identity") != BRIDGE or not bridge.get("metadata_differences")
                or bridge.get("bridge_target") != TARGET or bridge.get("bridge_dispatch") != DISPATCH
                or bridge.get("box_method") != "Ljava/lang/Boolean;->valueOf(Z)Ljava/lang/Boolean;"
                or bridge.get("unbox_methods") != []):
            raise RuntimeError("Boolean bridge report lost exact target, dispatch or canonical boxing")
        alias = target.get("source_name")
        if (target.get("projection_kind") != "incompatible-inherited-return-slot"
                or not alias or alias in {"invoke", "primitiveInvoke", "__neverdReturnSlot"}
                or target.get("source_identity") != "Lfixture/Receiver;->" + alias + "()Z"
                or target.get("source_name_declaration") != TARGET
                or target.get("conflicting_inherited_contracts") != [DISPATCH]
                or not target.get("metadata_differences")):
            raise RuntimeError("Primitive entry alias lost its collision or original body receipt")
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        if projected != {BRIDGE, TARGET}:
            raise RuntimeError("Boolean result proof added an unrelated source projection")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_interface else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        source_ids = {identity: row.get("source_identity", identity) for identity, row in rows.items()}
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(source_ids.values()) | helpers:
            raise RuntimeError("Recompile changed inventory beyond disclosed names and helpers")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            wanted, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                expected = RAW_SIGNATURE.decode() if owner == "Lfixture/Receiver;" and key == "signature" else wanted.get(key)
                if after.get(key) != expected:
                    raise RuntimeError("Original Boolean callback class metadata changed: " + owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                expected = dict(metadata)
                if identity == TARGET:
                    expected["name"] = alias
                if identity == BRIDGE:
                    expected["access"] &= ~0x1040
                if after["methods"][source_ids[identity]] != expected:
                    raise RuntimeError("Original method metadata changed beyond disclosed projections: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "BooleanResultBridgesHarness", "rebuilt", alias], "rebuilt-execution")
        if actual.splitlines() != [expected_counts, "reflection=reported-raw-interface-and-primitive-name"]:
            raise RuntimeError("Boolean state, cached result, dispatch or failure identity changed")

        for label, result_type in (("wrong-closed-result", b"Ljava/lang/Integer;"),
                                   ("open-result-scope", b"TS;")):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Receiver.class", INPUT_SIGNATURE,
                                       INPUT_SIGNATURE.replace(b"Ljava/lang/Boolean;", result_type))
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Boolean boxing bypassed a different complete result contract")
        negative_java = work / "negative-java/fixture"
        negative_java.mkdir(parents=True)
        wrong = negative_java / "Wrong.java"
        wrong.write_text("package fixture; public final class Wrong implements Function<Integer> {\n"
                         "  public Boolean invoke() { return Boolean.TRUE; }\n}\n")
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"),
                                   str(fixture / "fixture/Function.java"), str(wrong)], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or "invoke(" not in compiler.stderr or "compatible" not in compiler.stderr:
            raise RuntimeError("Independent javac did not reject a different closed result")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original classes, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "dependency_interface": args.dependency_interface,
                    "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 81, "identity_cases": 53, "state_cases": 90, "failure_cases": 10,
                    "primitive_direct_erased_and_reflective_dispatch": "matched",
                    "canonical_boolean_cache_identity_and_target_effects": "matched",
                    "source_alias_collision": "avoided",
                    "complete_metadata_and_inventory": "only reported names, bridge flags, raw interface and helpers",
                    "same_code_closed_and_open_result_counterexamples": "rejected",
                    "independent_javac_result_counterexample": "rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
