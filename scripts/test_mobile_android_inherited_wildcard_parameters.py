#!/usr/bin/env python3
"""Verify complete bounded wildcard override source erasure and original dispatch."""
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
    from scripts.test_mobile_android_final_getter_return_slots import rename_owned_entry
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_final_getter_return_slots import rename_owned_entry
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_nested_return import change_signature

PROTO = "(Lfixture/Pair;)Z"
METHOD = "Lfixture/Core;->remove" + PROTO
CONTRACT = "Lfixture/Parent;->remove" + PROTO
SIGNATURE = "(Lfixture/Pair<TK;TV;>;)Z"
PARENT_METHODS = {CONTRACT, "Lfixture/Parent;->remove(Ljava/lang/Object;)Z"}


def hashes(root: Path) -> dict:
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(root.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parent", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-inherited-wildcard-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "inherited_wildcard_parameters/java/fixture"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        edits = []
        for owner, name in (("Parent", "typedRemove"), ("Parent", "checked"), ("Core", "specificRemove"), ("Caller", "specificRemove")):
            edits.append(rename_owned_entry(originals / ("fixture/" + owner + ".class"), name.encode(), b"remove"))
        parent = originals / "fixture/Parent.class"
        edits += [bridge_flag(parent, "remove", PROTO, original_flags=1, input_flags=0x41),
                  bridge_flag(parent, "remove", "(Ljava/lang/Object;)Z", original_flags=0x11, input_flags=0x51)]
        (work / "original-return-slot-encoding.json").write_text(json.dumps(edits, indent=2) + "\n")
        verify.compile_local([FIXTURES / "harness/InheritedWildcardParametersHarness.java"], harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        declarations = {"Lfixture/Pair;", "Lfixture/Base;", "Lfixture/Parent;"}
        classes = all_classes - (declarations if args.dependency_parent else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        dependency = work / "owned-parents.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Pair", "Base", "Parent"):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_parent:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "InheritedWildcardParametersHarness", "original"], "baseline")
        lines = baseline.splitlines()
        match = re.fullmatch(r"values=(\d+),identities=(\d+),states=(\d+),failures=(\d+)", lines[0]) if len(lines) == 2 else None
        counts = tuple(map(int, match.groups())) if match else ()
        if counts != (70, 348, 740, 38) or lines[1] != "complete-bound-and-original-guarded-dispatch=retained,source-parameter-erasure=reported":
            raise RuntimeError("Owned wildcard case matrix changed: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned wildcard fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed a bounded wildcard original runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        wanted = {METHOD} if args.dependency_parent else {METHOD, *PARENT_METHODS}
        projections = coverage.get("generic_signature_projections", [])
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != len(wanted) or projected != wanted
                or coverage["status"] != "partial" or len(projections) != 1
                or coverage.get("class_source_bindings")):
            raise RuntimeError("Wildcard source erasure changed inventory or an unrelated scope: " + str(projected))
        row = rows[METHOD]
        changes = projections[0].get("changes", [])
        if (row.get("projection_kind") != "inherited-wildcard-parameter-erasure"
                or row.get("inherited_class_contract") != CONTRACT or row.get("source_identity") != METHOD
                or row.get("original_method_signature") != SIGNATURE or row.get("source_generic_signature") != PROTO
                or not row.get("metadata_differences") or projections[0].get("declaration") != METHOD
                or projections[0].get("original_signature") != SIGNATURE or len(changes) != 1
                or changes[0].get("kind") != "inherited-wildcard-parameter-erasure"
                or changes[0].get("source_type") != PROTO or changes[0].get("scope_origin") != CONTRACT):
            raise RuntimeError("Wildcard erasure report lost its complete original bounded scope")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_parent else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {entry["class"] + "->" + entry["name"] + entry["prototype"]
                   for entry in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != methods | helpers:
            raise RuntimeError("Rebuilt wildcard erasure invented or renamed a runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original wildcard class or field metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if method == METHOD:
                    wanted["signature"] = None
                if method in PARENT_METHODS:
                    wanted["access"] &= ~0x1040
                if after["methods"][method] != wanted:
                    raise RuntimeError("Metadata changed beyond wildcard source Signature or recorded parent bridge flags: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_parent else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             "InheritedWildcardParametersHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original wildcard guard, getters, mutation, aliases or exception order changed")
        for label, old, wrong in (
            ("wrong-parameter-scope", SIGNATURE, "(Lfixture/Pair<TV;TK;>;)Z"),
            ("wrong-superclass-binding", "<K:Ljava/lang/Object;V:Ljava/lang/Object;>Lfixture/Parent<Lfixture/Pair<TK;TV;>;TK;TV;>;",
             "<K:Ljava/lang/Object;V:Ljava/lang/Object;>Lfixture/Parent<Lfixture/Pair<TV;TK;>;TK;TV;>;"),
        ):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Core.class", old.encode(), wrong.encode())
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Wildcard source erasure hid different original type scope: " + label)
        wrong_java = work / "wrong-java/fixture/Wrong.java"
        wrong_java.parent.mkdir(parents=True)
        wrong_java.write_text("package fixture; public final class Wrong<K,V> extends Parent<Pair<K,V>,K,V> {\n"
                             "  public boolean remove(Pair<K,V> entry) { return false; }\n}\n")
        ordinary_parent = wrong_java.with_name("Parent.java")
        ordinary_parent.write_text((fixture / "Parent.java").read_text().replace("typedRemove", "remove"))
        rejected = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d", str(work / "wrong-compiled"),
                                   str(fixture / "Pair.java"), str(fixture / "Base.java"), str(ordinary_parent), str(wrong_java)],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-javac.log").write_text(rejected.stdout + rejected.stderr)
        if rejected.returncode == 0:
            raise RuntimeError("Independent javac accepted distinct complete source parameters with equal erasure")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness, ancestors or SDK changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": coverage["projected_method_count"], "value_cases": counts[0],
                    "identity_cases": counts[1], "state_cases": counts[2], "failure_cases": counts[3],
                    "profile": "dependency-parent" if args.dependency_parent else "source-parent",
                    "original_class_arguments_and_runtime_bodies": "unchanged",
                    "source_parameter_signature": "erasure reported after exact bounded superclass argument proof",
                    "same_code_scope_binding_and_javac_counterexamples": "rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
