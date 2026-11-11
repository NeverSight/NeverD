#!/usr/bin/env python3
"""Verify original integer static receiver implementations and erased comparison entries."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge

OWNER = "Lfixture/Core;"
BRIDGES = {OWNER + "->compareTo(Lfixture/Api;)I"}
ERASED = {owner + "->compareTo(Ljava/lang/Object;)I": owner + "->compareTo(Lfixture/Api;)I"
          for owner in (OWNER, "Lfixture/Api;")}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-helper", action="store_true")
    parser.add_argument("--reference-result", action="store_true")
    parser.add_argument("--omit-interface-bridge", action="store_true")
    parser.add_argument("--copied-receiver", action="store_true")
    parser.add_argument("--inherited-predicates", action="store_true")
    parser.add_argument("--dependency-parent", action="store_true")
    args = parser.parse_args()
    if args.dependency_parent and not (args.reference_result or args.inherited_predicates):
        parser.error("The parent dependency profile requires a reference or inherited-predicate profile")
    if (args.reference_result or args.inherited_predicates) and args.dependency_helper:
        parser.error("Reference helpers remain original input; use --dependency-parent")
    dependency_mode = args.dependency_helper or args.dependency_parent
    bridges, erased_entries = set(BRIDGES), dict(ERASED)
    added_bridge = "Lfixture/Api;->compareTo(Ljava/lang/Object;)I"
    added_target = "Lfixture/Api;->compareTo(Lfixture/Api;)I"
    projected_declarations = {added_target} if args.omit_interface_bridge else set()
    if args.omit_interface_bridge:
        del erased_entries[added_bridge]
    if args.reference_result:
        bridges.add(OWNER + "->shift(J)Lfixture/Api;")
        erased_entries.update({owner + "->shift(J)Lfixture/ShiftRoot;": owner + "->shift(J)Lfixture/Api;"
                               for owner in (OWNER, "Lfixture/Api;")})
    if args.inherited_predicates:
        bridges.update(OWNER + "->" + name + "()Z" for name in ("ready", "notReady"))
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-integer-static-comparisons-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = FIXTURES / "integer_static_comparisons/java"
        harness_sources = [FIXTURES / "harness/IntegerStaticComparisonHarness.java"]
        flag_entries = [(b"compareTo", b"(Lfixture/Api;)I")]
        if args.omit_interface_bridge:
            harness_sources.append(FIXTURES / "harness/AbsentInterfaceBridgeHarness.java")
        if args.reference_result:
            authored = work / "reference-java"
            shutil.copytree(fixture, authored)
            replacements = {
                "Api.java": [("extends Comparable<Api>", "extends Comparable<Api>, ShiftRoot"),
                             ("int observe(Api other);", "int observe(Api other); Api shift(long input); Api result();")],
                "Core.java": [("public int tag, score, calls;", "public int tag, score, calls; public Api output;"),
                              ("public int compareTo(Api other)", "public Api result() { return output; } public Api shift(long input) { return Dispatch.shift(this, input); } public int compareTo(Api other)")],
                "Dispatch.java": [("public static int compare", "public static Api shift(Api receiver, long input) { Trace.wide = input; receiver.observe(receiver); return receiver.result(); } public static int compare")],
                "Trace.java": [("public static int events;", "public static int events; public static long wide;")],
                "Caller.java": [("public static int typed", "public static ShiftRoot shift(ShiftRoot receiver, long input) { return receiver.shift(input); } public static int typed")],
            }
            for name, changes in replacements.items():
                path = authored / "fixture" / name
                text = path.read_text()
                for before, after in changes:
                    assert text.count(before) == 1
                    text = text.replace(before, after)
                path.write_text(text)
            (authored / "fixture/ShiftRoot.java").write_text("package fixture; public interface ShiftRoot { ShiftRoot shift(long input); }\n")
            fixture = authored
            harness_sources.append(FIXTURES / "harness/FixedReferenceStaticHarness.java")
            flag_entries.append((b"shift", b"(J)Lfixture/Api;"))
        if args.inherited_predicates:
            authored = work / "predicate-java"
            shutil.copytree(fixture, authored)
            replacements = {
                "Api.java": [("extends Comparable<Api>", "extends Comparable<Api>, Predicate")],
                "Core.java": [("public int compareTo(Api other)", "public boolean ready() { return Dispatch.ready(this); } public boolean notReady() { return Dispatch.notReady(this); } public int compareTo(Api other)")],
                "Dispatch.java": [("public static int compare", "public static boolean ready(Api receiver) { return receiver.observe(receiver) >= 0; } public static boolean notReady(Api receiver) { return !ready(receiver); } public static int compare")],
                "Caller.java": [("public static int typed", "public static boolean ready(Api receiver, boolean negative) { return negative ? receiver.notReady() : receiver.ready(); } public static int typed")],
            }
            for name, changes in replacements.items():
                path = authored / "fixture" / name
                text = path.read_text()
                for before, after in changes:
                    assert text.count(before) == 1
                    text = text.replace(before, after)
                path.write_text(text)
            (authored / "fixture/PredicateRoot.java").write_text("package fixture; public interface PredicateRoot { boolean ready(); boolean notReady(); }\n")
            (authored / "fixture/Predicate.java").write_text("package fixture; public interface Predicate extends PredicateRoot {}\n")
            fixture = authored
            harness_sources.append(FIXTURES / "harness/InheritedStaticPredicateHarness.java")
            flag_entries.extend((name, b"()Z") for name in (b"ready", b"notReady"))
        if args.copied_receiver:
            authored = work / "copied-java"
            shutil.copytree(fixture, authored)
            source = authored / "fixture/Core.java"
            text = source.read_text()
            for entry in ("compare", "shift", "ready", "notReady"):
                before = "Dispatch." + entry + "(this"
                if before in text:
                    assert text.count(before) == 1
                    text = text.replace(before, "Dispatch." + entry + "((Api) (Object) this")
            source.write_text(text)
            fixture = authored
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local(harness_sources, harness, classpath=originals)
        def encode(root):
            edits = [change_method_flags(root / "fixture/Core.class", prototype, 0x1, 0x41, name=name)
                     for name, prototype in flag_entries]
            if args.omit_interface_bridge:
                edits.append(omit_owned_interface_bridge(root / "fixture/Api.class", b"compareTo(Ljava/lang/Object;)I"))
            return edits
        receipts = encode(originals)
        (work / "original-encoding.json").write_text(json.dumps(receipts, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        dependency_names = (["ShiftRoot"] if args.reference_result else []) + (["Predicate", "PredicateRoot"] if args.inherited_predicates else []) if args.dependency_parent else ["Dispatch", "Trace"]
        declarations = {"Lfixture/" + name + ";" for name in dependency_names}
        classes = all_classes - (declarations if dependency_mode else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        original_exceptions = exception_inventory(originals)
        dependency = work / "owned-helper.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in dependency_names:
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if dependency_mode:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "IntegerStaticComparisonHarness", "original"], "baseline")
        lines = baseline.splitlines()
        match = re.fullmatch(r"values=(\d+),identities=(\d+),states=(\d+),failures=(\d+)", lines[0]) if len(lines) == 2 else None
        counts = tuple(map(int, match.groups())) if match else ()
        if counts != (80, 144, 237, 37) or lines[1] != "original-static-helper-arguments-callbacks-and-integer-results=retained,typed-bridge-flag=reported":
            raise RuntimeError("Owned integer static oracle changed: " + baseline)

        reference_baseline = None
        if args.reference_result:
            reference_baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                                             "FixedReferenceStaticHarness", "original"], "reference-baseline")
            if reference_baseline.splitlines() != ["values=152,identities=296,states=456,failures=16",
                    "original-wide-input-and-covariant-reference-identity=retained,typed-bridge-flag=reported"]:
                raise RuntimeError("Independent reference oracle missed original outcomes: " + reference_baseline)

        predicate_baseline = None
        if args.inherited_predicates:
            predicate_baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                                             "InheritedStaticPredicateHarness", "original"], "predicate-baseline")
            if predicate_baseline.splitlines() != ["values=80,identities=96,states=288,failures=32",
                    "inherited-predicate-declarations-original-helpers-and-receiver-effects=retained,typed-bridge-flags=reported"]:
                raise RuntimeError("Independent predicate oracle missed original outcomes: " + predicate_baseline)

        absent_baseline = None
        if args.omit_interface_bridge:
            absent_baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                                          "AbsentInterfaceBridgeHarness", "original"], "absent-baseline")
            if absent_baseline.splitlines() != ["values=10,identities=22,states=34,failures=4",
                    "inherited-erased-dispatch=matched,added-interface-default-and-reflection=reported"]:
                raise RuntimeError("Independent interface declaration oracle missed outcomes: " + absent_baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned integer static fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original integer static runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != len(bridges) + len(erased_entries) + len(projected_declarations) or coverage["status"] != "partial"
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != bridges | set(erased_entries) | projected_declarations
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Integer static implementation changed its original runtime inventory or scope")
        for identity in bridges:
            row = rows[identity]
            if (row.get("projection_kind") != "full-contract-implementation-bridge"
                    or row.get("source_identity") != identity or not row.get("inherited_contract")
                    or row.get("bridge_target") or not row.get("metadata_differences")):
                raise RuntimeError("Original integer static bridge lacks its complete original contract")
        for identity, target in erased_entries.items():
            if (rows[identity].get("projection_kind") != "generic-interface-bridge"
                    or rows[identity].get("bridge_target") != target
                    or rows[identity].get("bridge_dispatch") != ("Lfixture/ShiftRoot;->shift(J)Lfixture/ShiftRoot;" if "->shift(" in identity else "Ljava/lang/Comparable;->compareTo(Ljava/lang/Object;)I")):
                raise RuntimeError("Original erased comparison bridge lost its complete forwarding proof")
        if args.omit_interface_bridge:
            row = rows[added_target]
            generated = [item for item in coverage.get("generated_source_helpers", [])
                         if item.get("kind") == "compiler-parameter-interface-bridge"]
            if (row.get("projection_kind") != "interface-parameter-runtime-bridges"
                    or row.get("generated_bridges") != [added_bridge] or not row.get("metadata_differences")
                    or added_bridge in rows or len(generated) != 1
                    or generated[0].get("bridge_target") != added_target
                    or generated[0].get("bridge_dispatch") != "Ljava/lang/Comparable;->compareTo(Ljava/lang/Object;)I"
                    or generated[0].get("prototype") != "(Ljava/lang/Object;)I"
                    or generated[0].get("access_flags") != ["public", "bridge", "synthetic"]):
                raise RuntimeError("Added interface bridge is not distinguished from original runtime inventory")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if dependency_mode else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {entry["class"] + "->" + entry["name"] + entry["prototype"]
                   for entry in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != methods | helpers:
            raise RuntimeError("Rebuilt integer static implementation invented or renamed a runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original class or field metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if method in bridges:
                    wanted["access"] &= ~0x40
                if after["methods"][method] != wanted:
                    raise RuntimeError("Original method metadata changed beyond disclosed bridge flags: " + method)
        if args.omit_interface_bridge:
            generated = actual_records["Lfixture/Api;"]["methods"][added_bridge]
            if generated["access"] != 0x1041 or generated["signature"] is not None or not generated["code"]:
                raise RuntimeError("Added interface declaration has unreported bridge/default metadata")
        rebuilt_exceptions = exception_inventory(rebuilt)
        if any(rebuilt_exceptions[method] != original_exceptions[method] for method in methods):
            raise RuntimeError("Original exception metadata changed")
        runtime = (rebuilt, harness, dependency) if dependency_mode else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             "IntegerStaticComparisonHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original helper scopes, virtual callbacks, aliases or failure order changed")
        if reference_baseline is not None:
            actual_reference = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                           "FixedReferenceStaticHarness", "rebuilt"], "reference-rebuilt")
            if actual_reference != reference_baseline:
                raise RuntimeError("Original wide inputs, returned references or callback effects changed")
        if predicate_baseline is not None:
            actual_predicate = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                           "InheritedStaticPredicateHarness", "rebuilt"], "predicate-rebuilt")
            if actual_predicate != predicate_baseline:
                raise RuntimeError("Original inherited predicates, receiver effects or failures changed")
        if absent_baseline is not None:
            actual_absent = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                        "AbsentInterfaceBridgeHarness", "rebuilt"], "absent-rebuilt")
            if actual_absent != absent_baseline:
                raise RuntimeError("Original erased dispatch or disclosed interface reflection changed")
        labels = ["synthetic-implementation", "reversed-arguments", "extra-effect"]
        if args.reference_result:
            labels += ["reference-synthetic", "changed-wide-input"]
        if args.inherited_predicates:
            labels += ["predicate-synthetic", "changed-predicate-receiver"]
        if args.omit_interface_bridge:
            labels += ["wrong-interface-contract", "missing-class-bridge"]
        for label in labels:
            negative = work / label
            if label in ("wrong-interface-contract", "missing-class-bridge"):
                shutil.copytree(originals, negative)
                if label == "wrong-interface-contract":
                    receipt = change_signature(negative / "fixture/Api.class", records["Lfixture/Api;"]["signature"].encode(),
                                               records["Lfixture/Api;"]["signature"].replace("Comparable<Lfixture/Api;>", "Comparable<Lfixture/Core;>").encode())
                else:
                    receipt = omit_owned_interface_bridge(negative / "fixture/Core.class", b"compareTo(Ljava/lang/Object;)I")
            elif label in ("synthetic-implementation", "reference-synthetic", "predicate-synthetic"):
                shutil.copytree(originals, negative)
                prototype = b"()Z" if label == "predicate-synthetic" else b"(J)Lfixture/Api;" if label == "reference-synthetic" else b"(Lfixture/Api;)I"
                name = b"ready" if label == "predicate-synthetic" else b"shift" if label == "reference-synthetic" else b"compareTo"
                receipt = change_method_flags(negative / "fixture/Core.class", prototype, 0x41, 0x1041, name=name)
            else:
                authored = work / (label + "-java")
                shutil.copytree(fixture, authored)
                path = authored / "fixture/Core.java"
                text = path.read_text()
                old = ("return Dispatch.ready(this);" if label == "changed-predicate-receiver" else
                       "return Dispatch.shift(this, input);" if label == "changed-wide-input" else
                       "return Dispatch.compare(this, other);")
                if args.copied_receiver:
                    old = old.replace("(this", "((Api) (Object) this")
                assert text.count(old) == 1
                new = ("return Dispatch.ready(null);" if label == "changed-predicate-receiver" else
                       "return Dispatch.shift(this, input + 1);" if label == "changed-wide-input" else
                       "return Dispatch.compare(other, this);" if label == "reversed-arguments" else
                       "observe(other); return Dispatch.compare(this, other);")
                if args.copied_receiver:
                    new = new.replace("(this", "((Api) (Object) this")
                path.write_text(text.replace(old, new))
                verify.compile_local(sorted(authored.rglob("*.java")), negative)
                receipt = encode(negative)
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            bad_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(bad_dex), *map(str, classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Unproved integer implementation accepted: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness, helper or SDK changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": len(bridges) + len(erased_entries) + len(projected_declarations), "value_cases": counts[0], "identity_cases": counts[1],
                    "state_cases": counts[2], "failure_cases": counts[3],
                    "profile": "dependency-parent" if args.dependency_parent else "dependency-helper" if args.dependency_helper else "source-root",
                    "original_runtime_slots_scopes_bodies_and_exceptions": "unchanged",
                    "source_metadata_differences": "original typed ACC_BRIDGE flags" + (" and one reported compiler interface bridge" if args.omit_interface_bridge else ""),
                    "absent_interface_jvm_output": absent_baseline.splitlines() if absent_baseline else None,
                    "added_interface_bridge": added_bridge if args.omit_interface_bridge else None,
                    "reference_jvm_output": reference_baseline.splitlines() if reference_baseline else None,
                    "reference_result": args.reference_result,
                    "copied_receiver": args.copied_receiver,
                    "predicate_jvm_output": predicate_baseline.splitlines() if predicate_baseline else None,
                    "inherited_predicates": args.inherited_predicates,
                    "wrong_synthetic_flag_reversed_arguments_and_extra_effect": "rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
