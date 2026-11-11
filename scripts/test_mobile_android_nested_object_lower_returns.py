#!/usr/bin/env python3
"""Verify closed nested Object lower-bound return equivalence with original metadata and dispatch."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature

METHOD = "Lfixture/View;->values()Ljava/util/Iterator;"
CONTRACT = "Lfixture/Contract;->values()Ljava/util/Iterator;"
SIGNATURE = "()Ljava/util/Iterator<Lfixture/Pair<Ljava/lang/Object;Ljava/lang/Object;>;>;"
SOURCE_SIGNATURE = "()Ljava/util/Iterator<Lfixture/Pair<-Ljava/lang/Object;-Ljava/lang/Object;>;>;"


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
    parser.add_argument("--dependency-root", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nested-object-lower-return-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "nested_object_lower_returns/java/fixture"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/NestedObjectLowerReturnsHarness.java"], harness, classpath=originals)
        receipt = change_signature(originals / "fixture/View.class", SOURCE_SIGNATURE.encode(), SIGNATURE.encode())
        (work / "original-nested-return-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        declarations = {"Lfixture/Contract;"}
        classes = all_classes - (declarations if args.dependency_root else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        dependency = work / "owned-parents.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Contract",):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_root:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "NestedObjectLowerReturnsHarness", "original"], "baseline")
        lines = baseline.splitlines()
        match = re.fullmatch(r"values=(\d+),identities=(\d+),states=(\d+),failures=(\d+)", lines[0]) if len(lines) == 2 else None
        counts = tuple(map(int, match.groups())) if match else ()
        if counts != (18, 153, 117, 27) or lines[1] != "complete-object-lower-bound=retained,original-default-and-caller-dispatch=retained,all-original-generic-metadata=unchanged":
            raise RuntimeError("Owned nested lower-bound case matrix changed: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned nested lower-bound fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed a nested lower-bound original runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        projections = coverage.get("generic_signature_projections", [])
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 0 or projected
                or coverage["status"] != "recovered" or projections
                or coverage.get("class_source_bindings")):
            raise RuntimeError("Nested lower-bound return changed inventory or an unrelated scope")
        if (rows[METHOD].get("generic_return_contract") != "closed-object-lower-bound"
                or rows[METHOD].get("inherited_closed_lower_contracts") != [CONTRACT]
                or rows[METHOD].get("generic_signature") != SIGNATURE
                or rows[METHOD].get("metadata_differences")):
            raise RuntimeError("Nested lower-bound return lost its unchanged Signature or exact inherited witness")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_root else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {entry["class"] + "->" + entry["name"] + entry["prototype"]
                   for entry in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != methods | helpers:
            raise RuntimeError("Rebuilt nested return invented or renamed a runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original nested-return class or field metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if after["methods"][method] != wanted:
                    raise RuntimeError("Original method metadata changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_root else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             "NestedObjectLowerReturnsHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original nested-return identity, iterator state, aliases or exception order changed")
        for label, old, wrong in (
            ("wrong-return-argument", SIGNATURE,
             "()Ljava/util/Iterator<Lfixture/Pair<Ljava/lang/String;Ljava/lang/Object;>;>;"),
            ("wrong-inherited-lower-bound",
             "Ljava/lang/Object;Lfixture/Contract<Lfixture/Pair<-Ljava/lang/Object;-Ljava/lang/Object;>;>;",
             "Ljava/lang/Object;Lfixture/Contract<Lfixture/Pair<-Ljava/lang/String;-Ljava/lang/Object;>;>;"),
        ):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/View.class", old.encode(), wrong.encode())
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Nested return hid a different complete lower bound: " + label)
        wrong_java = work / "wrong-java/fixture/Wrong.java"
        wrong_java.parent.mkdir(parents=True)
        wrong_java.write_text("package fixture; import java.util.Iterator;\n"
                             "public final class Wrong implements Contract<Pair<? super Object,? super Object>> {\n"
                             "  public Iterator<Pair<Object,Object>> values() { return null; }\n}\n")
        command = [verify.javac, "--release", "8", "-proc:none", "-Xlint:unchecked", "-Xlint:-options", "-Werror", "-classpath", str(originals)]
        accepted = subprocess.run([*command, "-d", str(work / "equivalent-compiled"), str(wrong_java)],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "equivalent-javac.log").write_text(accepted.stdout + accepted.stderr)
        if accepted.returncode:
            raise RuntimeError("Independent javac rejected the exact original closed Object lower return")
        equivalent = class_identity.compiler_classes(work / "equivalent-compiled")
        if equivalent["Lfixture/Wrong;"]["methods"]["Lfixture/Wrong;->values()Ljava/util/Iterator;"].get("signature") != SIGNATURE:
            raise RuntimeError("Independent javac changed the original closed-equivalent return metadata")
        wrong_java.write_text(wrong_java.read_text().replace("Iterator<Pair<Object,Object>>", "Iterator<Pair<String,Object>>"))
        rejected = subprocess.run([*command, "-d", str(work / "wrong-compiled"), str(wrong_java)],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-javac.log").write_text(rejected.stdout + rejected.stderr)
        if rejected.returncode == 0:
            raise RuntimeError("Independent javac accepted a different complete nested lower bound")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness, ancestors or SDK changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": coverage.get("projected_method_count", 0), "value_cases": counts[0],
                    "identity_cases": counts[1], "state_cases": counts[2], "failure_cases": counts[3],
                    "profile": "dependency-root" if args.dependency_root else "source-root",
                    "original_class_arguments_runtime_slots_and_bodies": "unchanged",
                    "source_return_signature": "unchanged with closed Object lower-bound proof",
                    "independent_javac_equivalence_and_unchanged_signature": "confirmed",
                    "same_code_complete_bound_and_javac_counterexamples": "rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
