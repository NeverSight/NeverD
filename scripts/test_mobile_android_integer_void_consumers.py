#!/usr/bin/env python3
"""Verify original Number-to-int void consumers and captured outer effects."""
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
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag


OWNER = "Lfixture/Owner$Receiver;"
BRIDGE = OWNER + "->accept(Ljava/lang/Object;)V"
TARGET = OWNER + "->accept(I)V"
DISPATCH = "Lfixture/Action;->accept(Ljava/lang/Object;)V"
CAPTURE = OWNER + "->this$0:Lfixture/Owner;"
RAW_SIGNATURE = b"<S:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Action;"
INPUT_SIGNATURE = (b"<S:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;"
                   b"Lfixture/Action<Ljava/lang/Integer;>;")


def hashes(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path, help="JDK supporting Java release 25")
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-interface", action="store_true")
    parser.add_argument("--inherited-interface", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-integer-void-consumers-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "integer_void_consumers/java"
        originals, harness = work / "originals", work / "harness"
        inputs = sorted(fixture.rglob("*.java"))
        if args.inherited_interface:
            source = fixture / "fixture/Action.java"
            content = source.read_text()
            declaration = "public interface Action<T> {"
            if content.count(declaration) != 1:
                raise RuntimeError("Missing exact owned direct interface declaration")
            owned = work / "inherited-java/fixture/Action.java"
            owned.parent.mkdir(parents=True)
            owned.write_text(content.replace(declaration,
                "public interface Action<T> extends RootAction<T> {", 1))
            inputs = [owned if path == source else path for path in inputs]
            inputs.append(FIXTURES / "integer_void_consumers/inherited/java/fixture/RootAction.java")
        verify.compile_local(inputs, originals)
        harness_name = ("InheritedIntegerVoidConsumersHarness" if args.inherited_interface
                        else "IntegerVoidConsumersHarness")
        harness_sources = [FIXTURES / "harness/IntegerVoidConsumersHarness.java"]
        if args.inherited_interface:
            harness_sources.append(FIXTURES / ("harness/" + harness_name + ".java"))
        verify.compile_local(harness_sources, harness, classpath=originals)
        harness_hashes = hashes(harness)
        encoded = originals / "fixture/Owner$Receiver.class"
        edits = {"signature": change_signature(encoded, RAW_SIGNATURE, INPUT_SIGNATURE),
                 "flags": bridge_flag(encoded, "accept", "(Ljava/lang/Object;)V",
                                      original_flags=0x1, input_flags=0x1041)}
        (work / "owned-input-encoding.json").write_text(json.dumps(edits, indent=2) + "\n")
        original_hashes = hashes(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        declarations = {"Lfixture/Action;"} if args.dependency_interface else set()
        if args.inherited_interface:
            declarations.add("Lfixture/RootAction;")
        classes = all_classes - declarations
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        dependency = work / "owned-action.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for declaration in sorted(declarations or {"Lfixture/Action;"}):
                relative = declaration[1:-1] + ".class"
                archive.writestr(relative, (originals / relative).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if declarations:
            classpath += ["--classpath", dependency]

        def dex(root: Path, output: Path, dependency_arguments=None) -> Path:
            paths = [p for p in sorted(root.rglob("*.class"))
                     if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            output.mkdir()
            arguments = classpath if dependency_arguments is None else dependency_arguments
            verify.run([*verify.d8, "--debug", "--min-api", "24", *arguments,
                        "--output", output, *paths], "d8-" + output.name)
            result = output / "classes.dex"
            if not result.is_file() or list(output.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned Integer consumer did not produce one DEX")
            return result

        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_name, "original"], "baseline")
        counts = "values=55,identities=84,states=75,failures=25"
        inherited_expected = (["inherited-values=12,inherited-identities=24,inherited-states=20,inherited-failures=10",
                               "root-interface-dispatch=matched,parent-generic-scopes=unchanged,root-declaration=unchanged"]
                              if args.inherited_interface else [])
        if baseline.splitlines() != [counts, "reflection=original", *inherited_expected]:
            raise RuntimeError("Owned Integer consumer baseline missed a conversion or effect case")
        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed the original captured Integer consumer inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        captured_methods = {identity for identity in methods if identity.startswith(OWNER + "->")}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods)
                or coverage["projected_method_count"] != len(captured_methods)
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage.get("minimum_java_release") != 25):
            raise RuntimeError("Integer consumer recovery lost original entries or projection disclosure")
        if ({identity for identity, row in rows.items() if row["status"] == "source-projected"}
                != captured_methods or any(row.get("source_name") for row in rows.values())):
            raise RuntimeError("Integer consumer invented an alias or unrelated method projection")
        projections = coverage.get("generic_signature_projections", [])
        if (len(projections) != 1 or not projections[0].get("metadata_differences")
                or projections[0].get("original_signature") != INPUT_SIGNATURE.decode()
                or len(projections[0]["changes"]) != 1):
            raise RuntimeError("Integer consumer projection lost the original full class Signature")
        change = projections[0]["changes"][0]
        if (change["kind"] != "checked-value-carrier-interface" or change["scope_origin"] != BRIDGE
                or change["original_type"] != "Lfixture/Action<Ljava/lang/Integer;>;"
                or change["source_type"] != "Lfixture/Action;"):
            raise RuntimeError("Integer consumer raw interface lacks its original conversion witness")
        bridge = rows[BRIDGE]
        if (bridge.get("projection_kind") != "value-carrier-interface-bridge"
                or bridge.get("source_identity") != BRIDGE or not bridge.get("metadata_differences")
                or bridge.get("bridge_target") != TARGET or bridge.get("bridge_dispatch") != DISPATCH
                or bridge.get("unbox_methods") != ["Ljava/lang/Number;->intValue()I"]
                or "box_method" in bridge):
            raise RuntimeError("Integer bridge lost its exact void target or Number conversion ABI")
        bindings = coverage.get("class_source_bindings", [])
        if (len(bindings) != 1 or bindings[0].get("binding_kind") != "inner-to-static-member"
                or bindings[0].get("outer_capture_field") != CAPTURE
                or not bindings[0].get("metadata_differences")
                or not any("synthetic capture-field" in text for text in bindings[0]["metadata_differences"])):
            raise RuntimeError("Captured consumer lost its original outer field and precise modifier disclosure")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if declarations else None, release=25)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != methods | helpers:
            raise RuntimeError("Recompile changed original consumer identities or added unreported helpers")
        expected_records = copy.deepcopy(records)
        expected_records[OWNER]["signature"] = RAW_SIGNATURE.decode()
        expected_records[OWNER]["inner_class"]["access"] |= 8
        expected_records[OWNER]["fields"][CAPTURE]["access"] &= ~0x1000
        expected_records[OWNER]["methods"][BRIDGE]["access"] &= ~0x1040
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            wanted, after = expected_records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != wanted.get(key):
                    raise RuntimeError("Consumer class metadata changed beyond disclosed projections: " + owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original consumer method metadata changed beyond bridge flags: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             harness_name, "rebuilt"], "rebuilt-execution")
        if actual.splitlines() != [counts, "reflection=reported-raw-interface-and-captured-member", *inherited_expected]:
            raise RuntimeError("Original conversion order, void dispatch, identities or captured effects changed")

        for label, argument in (("wrong-closed-input", b"Ljava/lang/Float;"),
                                ("open-input-scope", b"TS;")):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Owner$Receiver.class", INPUT_SIGNATURE,
                                       INPUT_SIGNATURE.replace(b"Ljava/lang/Integer;", argument))
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Integer conversion bypassed an incompatible complete interface argument")
        negative_java = work / "negative-java/fixture"
        negative_java.mkdir(parents=True)
        wrong = negative_java / "Wrong.java"
        wrong.write_text("package fixture; public final class Wrong implements Action<Float> {\n"
                         "  public void accept(Integer value) {}\n}\n")
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"),
                                   str(fixture / "fixture/Action.java"), str(wrong)], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or "does not override" not in compiler.stderr:
            raise RuntimeError("Independent javac did not reject a different closed input contract")
        if args.inherited_interface:
            negative = work / "wrong-parent-edge-originals"
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Action.class",
                b"<T:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/RootAction<TT;>;",
                b"<T:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/RootAction<Ljava/lang/String;>;")
            (work / "wrong-parent-edge-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dependency = work / "wrong-parent-edge-declarations.jar"
            with zipfile.ZipFile(negative_dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                for declaration in sorted(declarations):
                    relative = declaration[1:-1] + ".class"
                    archive.writestr(relative, (negative / relative).read_bytes())
            negative_classpath = ["--classpath", args.android_jar.resolve(),
                                  "--classpath", negative_dependency]
            negative_dex = dex(negative, work / "wrong-parent-edge-dex", negative_classpath)
            rejected = work / "wrong-parent-edge-rejected"
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, negative_classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / "wrong-parent-edge-recovery.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Integer conversion bypassed a competing inherited generic input")
            negative_source = work / "wrong-parent-edge-java/fixture/Action.java"
            negative_source.parent.mkdir(parents=True)
            content = next(path for path in inputs if path.name == "Action.java").read_text()
            if content.count("extends RootAction<T>") != 1:
                raise RuntimeError("Missing exact inherited interface compiler counterexample")
            negative_source.write_text(content.replace("extends RootAction<T>",
                                                       "extends RootAction<String>", 1))
            compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                str(work / "wrong-parent-edge-compiler"), str(negative_source),
                str(FIXTURES / "integer_void_consumers/inherited/java/fixture/RootAction.java")],
                text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "wrong-parent-edge-compiler.log").write_text(compiler.stdout + compiler.stderr)
            if compiler.returncode == 0 or not any(word in compiler.stderr for word in ("clash", "override")):
                raise RuntimeError("Independent javac accepted competing complete parent arguments")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original classes, immutable harness or declaration bytes changed")
        evidence = {"status": "success", "dependency_interface": args.dependency_interface,
                    "inherited_interface": args.inherited_interface,
                    "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 55, "identity_cases": 84, "state_cases": 75, "failure_cases": 25,
                    "primitive_object_interface_and_reflective_dispatch": "matched",
                    "non_integer_numbers_and_exact_virtual_conversion": "matched",
                    "conversion_and_target_failures_with_captured_effect_order": "matched",
                    "complete_metadata_and_inventory": "only reported raw interface, bridge flags, capture projection and helpers",
                    "primitive_runtime_identity": "unchanged",
                    "same_code_closed_and_open_input_counterexamples": "rejected",
                    "independent_javac_input_counterexample": "rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged", "coverage": "partial"}
        if args.inherited_interface:
            evidence.update(inherited_value_cases=12, inherited_identity_cases=24,
                            inherited_state_cases=20, inherited_failure_cases=10,
                            root_interface_dispatch="matched",
                            direct_and_root_interface_generic_metadata="unchanged",
                            parent_edge_counterexample="rejected with original Code and by javac",
                            root_interface_declaration_jar="immutable")
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
