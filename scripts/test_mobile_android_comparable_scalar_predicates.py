#!/usr/bin/env python3
"""Verify original Comparable checks and scalar dispatch across raw source interface projections."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory

OWNER = "Lfixture/Core;"
BRIDGE = OWNER + "->contains(Ljava/lang/Comparable;)Z"
GETTER = OWNER + "->get()Ljava/lang/Comparable;"
SOURCE_SIGNATURE = b"Lfixture/Parent<Ljava/lang/Object;>;Lfixture/Api;Lfixture/OtherApi;"
INPUT_SIGNATURE = SOURCE_SIGNATURE.replace(b"Lfixture/Api;", b"Lfixture/Api<Ljava/lang/Character;>;").replace(
    b"Lfixture/OtherApi;", b"Lfixture/OtherApi<Ljava/lang/Character;>;")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-root", action="store_true")
    parser.add_argument("--numeric", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-comparable-scalar-predicates-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = "comparable_numeric_predicates" if args.numeric else "comparable_scalar_predicates"
        harness_name = "ComparableNumericPredicatesHarness" if args.numeric else "ComparableScalarPredicatesHarness"
        boxed = "Double" if args.numeric else "Character"
        primitive = "D" if args.numeric else "C"
        input_signature = INPUT_SIGNATURE.replace(b"Character", boxed.encode())
        bridges = {BRIDGE: OWNER + "->contains(" + primitive + ")Z"}
        if args.numeric:
            bridges[OWNER + "->before(Ljava/lang/Comparable;Ljava/lang/Comparable;)Z"] = OWNER + "->before(DD)Z"
        projected = {*bridges, GETTER}
        verify.compile_local(sorted((FIXTURES / fixture / "java").rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness" / (harness_name + ".java")], harness, classpath=originals)
        receipts = [change_signature(originals / "fixture/Core.class", SOURCE_SIGNATURE, input_signature),
                    change_method_flags(originals / "fixture/Core.class", b"(Ljava/lang/Comparable;)Z", 0x1, 0x1041, name=b"contains")]
        if args.numeric:
            receipts.append(change_method_flags(originals / "fixture/Core.class", b"(Ljava/lang/Comparable;Ljava/lang/Comparable;)Z",
                                                0x1, 0x1041, name=b"before"))
        (work / "original-encoding.json").write_text(json.dumps(receipts, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        declarations = {"Lfixture/Api;", "Lfixture/OtherApi;", "Lfixture/Parent;"}
        dependency_names = ["Api", "OtherApi", "Parent"] + (["RootApi"] if args.numeric else [])
        declarations.update("Lfixture/" + name + ";" for name in dependency_names)
        classes = all_classes - (declarations if args.dependency_root else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        original_exceptions = exception_inventory(originals)
        dependency = work / "owned-ancestors.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in dependency_names:
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_root:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_name, "original"], "baseline")
        lines = baseline.splitlines()
        match = re.fullmatch(r"values=(\d+),identities=(\d+),states=(\d+),failures=(\d+)", lines[0]) if len(lines) == 2 else None
        counts = tuple(map(int, match.groups())) if match else ()
        if counts != ((168, 64, 376, 96) if args.numeric else (80, 64, 112, 40)) or lines[1] != "original-casts-unboxing-parent-dispatch-and-runtime-slots=retained,raw-interface-and-bridge-metadata=reported":
            raise RuntimeError("Owned Comparable predicate oracle changed: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned predicate fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original predicate runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projections = coverage.get("generic_signature_projections", [])
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != len(projected) or coverage["status"] != "partial"
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != projected
                or coverage.get("class_source_bindings") or len(projections) != 1):
            raise RuntimeError("Predicate projection changed its original runtime inventory or scope")
        projection = projections[0]
        changes = projection.get("changes", [])
        if (projection.get("declaration") != OWNER or projection.get("original_signature") != input_signature.decode()
                or len(changes) != 2 or {row.get("source_type") for row in changes} != {"Lfixture/Api;", "Lfixture/OtherApi;"}
                or any(row.get("kind") != "checked-comparable-predicate-raw-interface" or row.get("scope_origin") not in bridges for row in changes)
                or rows[BRIDGE].get("projection_kind") != "checked-comparable-predicate-bridge"
                or rows[BRIDGE].get("source_identity") != BRIDGE
                or rows[BRIDGE].get("bridge_target") != bridges[BRIDGE]
                or rows[BRIDGE].get("unbox_method") != ("Ljava/lang/Number;->doubleValue()D" if args.numeric else "Ljava/lang/Character;->charValue()C")
                or not rows[BRIDGE].get("metadata_differences")):
            raise RuntimeError("Predicate projection lost its original contract or exact body witness")
        for identity, target in bridges.items():
            if (rows[identity].get("projection_kind") != "checked-comparable-predicate-bridge"
                    or rows[identity].get("source_identity") != identity
                    or rows[identity].get("bridge_target") != target
                    or rows[identity].get("unbox_method") != rows[BRIDGE].get("unbox_method")):
                raise RuntimeError("An original scalar predicate lost its checked conversion proof")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_root else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {entry["class"] + "->" + entry["name"] + entry["prototype"]
                   for entry in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != methods | helpers:
            raise RuntimeError("Rebuilt predicate invented or renamed a runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                expected = SOURCE_SIGNATURE.decode() if identity == OWNER and key == "signature" else before.get(key)
                if after.get(key) != expected:
                    raise RuntimeError("Unreported class or field metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if method in bridges:
                    wanted["access"] &= ~0x1040
                if after["methods"][method] != wanted:
                    raise RuntimeError("Unreported original method metadata changed: " + method)
        rebuilt_exceptions = exception_inventory(rebuilt)
        if any(rebuilt_exceptions[method] != original_exceptions[method] for method in methods):
            raise RuntimeError("Original exception metadata changed")
        runtime = (rebuilt, harness, dependency) if args.dependency_root else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             harness_name, "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original checked casts, null checks, scalar calls or parent effects changed")
        for mutation in ("wrong-argument", "missing-synthetic"):
            negative = work / (mutation + "-originals")
            shutil.copytree(originals, negative)
            if mutation == "wrong-argument":
                receipt = change_signature(negative / "fixture/Core.class", input_signature,
                                           input_signature.replace(("Ljava/lang/" + boxed + ";").encode(), b"Ljava/lang/Integer;"))
            else:
                receipt = change_method_flags(negative / "fixture/Core.class", b"(Ljava/lang/Comparable;)Z", 0x1041, 0x41, name=b"contains")
            (work / (mutation + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            bad_dex = dex(negative, work / (mutation + "-dex"))
            rejected = work / (mutation + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(bad_dex), *map(str, classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (mutation + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Predicate admission accepted an unproved original conversion: " + mutation)
        wrong_java = work / "wrong-java/fixture/Wrong.java"
        wrong_java.parent.mkdir(parents=True)
        wrong_java.write_text("package fixture; public final class Wrong implements Api<" + boxed + "> {\n"
                             "  public boolean contains(Comparable value) { return false; }\n"
                             "  public " + boxed + " get() { return null; }\n"
                             "  public boolean isEmpty() { return false; }\n" +
                             ("  public boolean before(Comparable a, Comparable b) { return false; }\n" if args.numeric else "") + "}\n")
        invalid = subprocess.run([str(verify.javac), "--release", "8", "-proc:none", "-classpath", str(originals),
                                  "-d", str(work / "wrong-compiled"), str(wrong_java)],
                                 text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-javac.log").write_text(invalid.stdout + invalid.stderr)
        if invalid.returncode == 0 or "name clash" not in invalid.stderr:
            raise RuntimeError("Independent javac accepted the incompatible specialized source parameter")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness, ancestors or SDK changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": len(projected), "numeric": args.numeric, "value_cases": counts[0], "identity_cases": counts[1],
                    "state_cases": counts[2], "failure_cases": counts[3],
                    "profile": "dependency-root" if args.dependency_root else "source-root",
                    "original_runtime_slots_bodies_and_exceptions": "unchanged",
                    "source_metadata_differences": "two raw interface edges and disclosed retained predicate bridge flags",
                    "same_code_negative_controls": "rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
