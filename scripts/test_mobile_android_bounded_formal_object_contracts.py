#!/usr/bin/env python3
"""Verify bounded class-formal getters with inherited Object and fixed default contracts."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_final_getter_return_slots import rename_owned_entry
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature


BRIDGE = "Lfixture/OrderNode;->compare(Ljava/lang/Object;)I"
TARGET = "Lfixture/OrderNode;->compare(Lfixture/OrderNode;)I"
DISPATCH = "Lfixture/Ordering;->compare(Ljava/lang/Object;)I"


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
    parser.add_argument("--dependency-root", action="store_true")
    parser.add_argument("--default-type", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-bounded-formal-object-contracts-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "bounded_formal_object_contracts/java"
        inputs = sorted(fixture.rglob("*.java"))
        if args.default_type:
            authored = work / "default-type-java"
            parent_type = ("    @Override\n    public Class<? extends Bound> type() {\n"
                           "        return Token.class;\n    }\n\n")
            adjusted = []
            for source in inputs:
                content = source.read_text()
                if source.name == "Source.java":
                    content = (FIXTURES / "bounded_formal_object_contracts/default_type/java/fixture/Source.java").read_text()
                elif source.name == "Parent.java":
                    if content.count(parent_type) != 1:
                        raise RuntimeError("Missing exact owned class-token implementation for default profile")
                    content = content.replace(parent_type, "", 1)
                destination = authored / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                adjusted.append(destination)
            fixture, inputs = authored, adjusted
        type_owner = "Source" if args.default_type else "Parent"
        profile = "default" if args.default_type else "class"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/BoundedFormalObjectContractsHarness.java"],
                             harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        all_classes, all_methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        external = {"Lfixture/Ordering;", "Lfixture/TypeView;"} if args.dependency_root else set()
        classes = all_classes - external
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        if (len(classes), len(methods)) != ((9, 18) if args.dependency_root else (11, 20)):
            raise RuntimeError("Owned bounded-formal baseline inventory changed")
        dependency = work / "owned-declarations.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Ordering", "TypeView"):
                archive.writestr("fixture/" + name + ".class",
                                 (originals / "fixture" / (name + ".class")).read_bytes())
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
                raise RuntimeError("Owned bounded-formal fixture did not produce exactly one DEX")
            return result

        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "BoundedFormalObjectContractsHarness", profile], "baseline")
        expected = ["values=100,identities=596,states=194,failures=94",
                    "bounded-return-formals=retained,object-declaring-owner=retained,default-bridge-owner=retained"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned bounded-formal baseline missed a dispatch or identity case: " + baseline)
        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed the original bounded-formal inventory")
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
            raise RuntimeError("Bounded class admission changed original scopes, methods or source bindings")
        if (rows[BRIDGE].get("projection_kind") != "generic-interface-bridge"
                or rows[BRIDGE].get("bridge_target") != TARGET
                or rows[BRIDGE].get("bridge_dispatch") != DISPATCH):
            raise RuntimeError("Inherited default bridge lacks its complete target and root: "
                               + json.dumps(rows[BRIDGE], sort_keys=True))
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_root else None,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != methods | helpers:
            raise RuntimeError("Recompile invented a getter, Object implementation or unreported helper")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            wanted, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != wanted.get(key):
                    raise RuntimeError("Original bounded-formal class metadata changed: " + owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original bounded-formal method metadata changed: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "BoundedFormalObjectContractsHarness", profile], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Bounded return, Object inheritance or original default behavior changed")

        def reject(root: Path, label: str, receipt: dict) -> None:
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(root, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if (result.returncode == 0 or rejected.exists()
                    or not any(word in result.stderr for word in ("generic", "interface", "inheritance"))):
                raise RuntimeError("Bounded class proof accepted incompatible scope or Object shadow: " + label)

        for label in ("method-formal-shadow", "callback-arguments", "root-argument", "closed-result"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "method-formal-shadow":
                receipt = change_signature(negative / "fixture/Parent.class",
                    b"()TT;", b"<T::Lfixture/Bound;>()TT;")
            elif label == "callback-arguments":
                receipt = change_signature(negative / "fixture/Parent.class",
                    b"(Lfixture/Callback<Ljava/lang/String;[B>;)TT;",
                    b"(Lfixture/Callback<[BLjava/lang/String;>;)TT;")
            elif label == "root-argument":
                receipt = change_signature(negative / "fixture/OrderNode.class",
                    b"Ljava/lang/Object;Lfixture/Ordering<Lfixture/OrderNode;>;",
                    b"Ljava/lang/Object;Lfixture/Ordering<Lfixture/Token;>;")
            else:
                receipt = change_signature(negative / "fixture" / (type_owner + ".class"),
                    b"()Ljava/lang/Class<+Lfixture/Bound;>;",
                    b"()Ljava/lang/Class<+Ljava/lang/String;>;")
            reject(negative, label, receipt)

        def variant_sources(label: str, *, actual_name: bool) -> list:
            directory = work / (label + ("-invalid-java" if actual_name else "-owned-java"))
            paths = []
            for source in inputs:
                content = source.read_text()
                if label == "method-formal-shadow" and source.name == "Parent.java":
                    content = content.replace("public T get()", "public <T extends Bound> T get()", 1)
                elif label == "callback-arguments" and source.name == "Parent.java":
                    content = content.replace("public T echo(Callback<String, byte[]> callback)",
                                              "public T echo(Callback<byte[], String> callback)", 1)
                elif label == "root-argument" and source.name == "OrderNode.java":
                    content = content.replace("Ordering<OrderNode>", "Ordering<Token>", 1)
                elif label == "closed-result" and source.name == type_owner + ".java":
                    declaration = ("default" if args.default_type else "public") + " Class<? extends Bound> type()"
                    if content.count(declaration) != 1:
                        raise RuntimeError("Missing exact original closed-result compiler declaration")
                    content = content.replace(declaration, declaration.replace("Bound", "String"), 1)
                elif label in ("private-object-shadow", "static-object-shadow", "abstract-object-shadow"):
                    addition = None
                    name = "equals" if actual_name else "same"
                    if source.name == "Parent.java":
                        if label == "private-object-shadow":
                            addition = "    private boolean " + name + "(Object other) { return other != null; }\n"
                        elif label == "static-object-shadow":
                            addition = "    public static boolean " + name + "(Object other) { return other != null; }\n"
                        else:
                            addition = "    public abstract boolean " + name + "(Object other);\n"
                    elif source.name == "Child.java" and label == "abstract-object-shadow":
                        addition = "    public boolean same(Object other) { return other != null; }\n"
                    if addition is not None:
                        end = content.rfind("}")
                        content = content[:end] + addition + content[end:]
                destination = directory / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                paths.append(destination)
            return paths

        for label in ("private-object-shadow", "static-object-shadow", "abstract-object-shadow"):
            negative = work / (label + "-originals")
            verify.compile_local(variant_sources(label, actual_name=False), negative)
            before = hashes(negative)
            receipt = rename_owned_entry(negative / "fixture/Parent.class", b"same", b"equals")
            after = hashes(negative)
            if {name for name in before if before[name] != after[name]} != {"fixture/Parent.class"}:
                raise RuntimeError("Object slot counterexample changed an unrelated owned class")
            receipt["legal_original_named_method"] = "same(Object)"
            receipt["own_Object_slot_kind"] = label.removesuffix("-object-shadow")
            if label == "abstract-object-shadow":
                receipt["child_same_entry"] = "unchanged; does not implement renamed abstract equals"
            reject(negative, label, receipt)

        compiler_diagnostics = {
            "method-formal-shadow": ("name clash", "does not override"),
            "callback-arguments": ("name clash", "does not override"),
            "root-argument": ("name clash", "does not override"),
            "closed-result": ("return type", "does not override", "incompatible types"),
            "private-object-shadow": ("attempting to assign weaker access privileges",),
            "static-object-shadow": ("overriding method is static", "cannot implement"),
            "abstract-object-shadow": ("does not override abstract method",),
        }
        for label in ("method-formal-shadow", "callback-arguments", "root-argument", "closed-result",
                      "private-object-shadow", "static-object-shadow", "abstract-object-shadow"):
            compiler_inputs = variant_sources(label, actual_name=True)
            compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                       str(work / (label + "-compiler")), *map(str, compiler_inputs)], text=True,
                                      capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-compiler.log")).write_text(compiler.stdout + compiler.stderr)
            expected_diagnostic = any(fragment in compiler.stderr for fragment in compiler_diagnostics[label])
            if label.endswith("-object-shadow"):
                expected_diagnostic &= "equals(Object)" in compiler.stderr
            if label == "static-object-shadow":
                expected_diagnostic &= "static" in compiler.stderr
            if label == "closed-result":
                expected_diagnostic &= "type()" in compiler.stderr
            if compiler.returncode == 0 or not expected_diagnostic:
                raise RuntimeError("Independent javac did not produce the expected contract rejection: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original classes, immutable harness, dependency or SDK bytes changed")
        evidence = {"status": "success", "dependency_root": args.dependency_root,
                    "default_type": args.default_type, "class_token_declaring_owner": "fixture." + type_owner,
                    "class_count": len(classes), "method_count": len(methods), "projected_methods": 1,
                    "value_cases": 100, "identity_cases": 596, "state_cases": 194, "failure_cases": 94,
                    "class_owned_bound_return_and_field_signatures": "unchanged",
                    "complete_fixed_callback_parameters": "unchanged",
                    "complete_closed_class_token_result_and_identity": "unchanged",
                    "Object_contract_declaring_owner": "java.lang.Object",
                    "fixed_default_original_owner_and_bridge": "fixture.OrderNode",
                    "getter_Object_and_default_dispatch_state_and_exceptions": "matched",
                    "invented_getters_Object_implementations_and_child_members": 0,
                    "complete_original_metadata_and_inventory": "unchanged except reported helpers",
                    "same_Code_method_scope_callback_root_and_closed_result_counterexamples": "rejected",
                    "same_Code_private_static_and_abstract_Object_slot_counterexamples": "rejected",
                    "independent_javac_illegal_contract_counterexamples": "rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
