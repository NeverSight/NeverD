#!/usr/bin/env python3
"""Verify declared carrier conversions and original factories under recursive Comparable interfaces."""
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
INPUT_SIGNATURE = SOURCE_SIGNATURE.replace(b"Lfixture/Api;", b"Lfixture/Api<Lfixture/Carrier;>;").replace(
    b"Lfixture/OtherApi;", b"Lfixture/OtherApi<Lfixture/Carrier;>;")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-root", action="store_true")
    parser.add_argument("--narrow", action="store_true")
    parser.add_argument("--inlined-carrier", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-comparable-carrier-predicates-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = "comparable_carrier_predicates"
        harness_name = "ComparableCarrierPredicatesHarness"
        primitive = "I" if args.narrow else "J"
        input_signature = INPUT_SIGNATURE
        bridges = {BRIDGE: OWNER + "->containsScalar(" + primitive + ")Z",
                   OWNER + "->before(Ljava/lang/Comparable;Ljava/lang/Comparable;)Z": OWNER + "->beforeScalar(" + primitive * 2 + ")Z"}
        projected = {*bridges, GETTER}
        if not args.dependency_root or args.inlined_carrier:
            projected.add("Lfixture/Carrier;->compareTo(Ljava/lang/Object;)I")
        authored = work / "authored"
        sources = []
        for path in sorted((FIXTURES / fixture / "java").rglob("*.java")) + [FIXTURES / "harness" / (harness_name + ".java")]:
            destination = authored / (path.name if path.name == harness_name + ".java" else "fixture/" + path.name)
            destination.parent.mkdir(parents=True, exist_ok=True)
            text = path.read_text()
            if args.inlined_carrier and path.name == "Carrier.java":
                text = text.replace("implements Comparable<Carrier>", "implements Comparable<Object>")
                text = text.replace("compareTo(Carrier other) {", "compareTo(Object argument) { Carrier other = (Carrier) argument;")
            if args.narrow:
                text = re.sub(r"\blong\b", "int", text).replace("Long.", "Integer.")
            destination.write_text(text)
            sources.append(destination)
        verify.compile_local(sources[:-1], originals)
        verify.compile_local(sources[-1:], harness, classpath=originals)
        receipts = [change_signature(originals / "fixture/Core.class", SOURCE_SIGNATURE, input_signature)]
        for name, prototype in ((b"contains", b"(Ljava/lang/Comparable;)Z"),
                                (b"before", b"(Ljava/lang/Comparable;Ljava/lang/Comparable;)Z"),
                                (b"get", b"()Ljava/lang/Comparable;")):
            receipts.append(change_method_flags(originals / "fixture/Core.class", prototype, 0x1, 0x1041, name=name))
        if args.inlined_carrier:
            receipts += [change_signature(originals / "fixture/Carrier.class",
                                          b"Ljava/lang/Object;Ljava/lang/Comparable<Ljava/lang/Object;>;",
                                          b"Ljava/lang/Object;Ljava/lang/Comparable<Lfixture/Carrier;>;"),
                         change_method_flags(originals / "fixture/Carrier.class", b"(Ljava/lang/Object;)I", 0x1, 0x1041, name=b"compareTo")]
        (work / "original-encoding.json").write_text(json.dumps(receipts, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        declarations = {"Lfixture/Api;", "Lfixture/OtherApi;", "Lfixture/Parent;"}
        dependency_names = ["Api", "OtherApi", "Parent", "RootApi"] + ([] if args.inlined_carrier else ["Carrier"])
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
                               harness_name, "original", "inlined" if args.inlined_carrier else "plain"], "baseline")
        lines = baseline.splitlines()
        match = re.fullmatch(r"values=(\d+),identities=(\d+),states=(\d+),failures=(\d+)", lines[0]) if len(lines) == 2 else None
        counts = tuple(map(int, match.groups())) if match else ()
        if counts != ((105, 347, 630, 94) if args.inlined_carrier else (96, 346, 624, 88)) or lines[1] != "original-conversions-factories-effects-and-runtime-slots=retained,raw-interface-and-bridge-metadata=reported":
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
                or coverage.get("class_source_bindings") or len(projections) != (2 if args.inlined_carrier else 1)):
            raise RuntimeError("Predicate projection changed its original runtime inventory or scope")
        projection = next(item for item in projections if item["declaration"] == OWNER)
        changes = projection.get("changes", [])
        if (projection.get("declaration") != OWNER or projection.get("original_signature") != input_signature.decode()
                or len(changes) != 2 or {row.get("source_type") for row in changes} != {"Lfixture/Api;", "Lfixture/OtherApi;"}
                or any(row.get("kind") != "checked-comparable-predicate-raw-interface" or row.get("scope_origin") not in bridges for row in changes)
                or rows[BRIDGE].get("projection_kind") != "checked-comparable-predicate-bridge"
                or rows[BRIDGE].get("source_identity") != BRIDGE
                or rows[BRIDGE].get("bridge_target") != bridges[BRIDGE]
                or rows[BRIDGE].get("unbox_method") != ("Lfixture/Carrier;->read()" + primitive)
                or not rows[BRIDGE].get("metadata_differences")):
            raise RuntimeError("Predicate projection lost its original contract or exact body witness")
        for identity, target in bridges.items():
            if (rows[identity].get("projection_kind") != "checked-comparable-predicate-bridge"
                    or rows[identity].get("source_identity") != identity
                    or rows[identity].get("bridge_target") != target
                    or rows[identity].get("unbox_method") != rows[BRIDGE].get("unbox_method")):
                raise RuntimeError("An original scalar predicate lost its checked conversion proof")
        if (rows[GETTER].get("projection_kind") != "boxed-generic-interface-result-bridge"
                or rows[GETTER].get("bridge_target") != OWNER + "->readScalar()" + primitive
                or rows[GETTER].get("box_method") != "Lfixture/Carrier;->make(" + primitive + ")Lfixture/Carrier;"):
            raise RuntimeError("Original factory call lacks its shared boxing proof")
        if args.inlined_carrier:
            carrier_projection = next(item for item in projections if item["declaration"] == "Lfixture/Carrier;")
            changes = carrier_projection.get("changes", [])
            if (len(changes) != 1 or changes[0].get("kind") != "checked-inlined-interface"
                    or changes[0].get("source_type") != "Ljava/lang/Comparable<Ljava/lang/Object;>;"
                    or rows["Lfixture/Carrier;->compareTo(Ljava/lang/Object;)I"].get("projection_kind") != "checked-inlined-interface-bridge"):
                raise RuntimeError("Original carrier comparison proof was not preserved")
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
                if args.inlined_carrier and identity == "Lfixture/Carrier;" and key == "signature":
                    expected = "Ljava/lang/Object;Ljava/lang/Comparable<Ljava/lang/Object;>;"
                if after.get(key) != expected:
                    raise RuntimeError("Unreported class or field metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if method in {*bridges, GETTER} or (args.inlined_carrier and method == "Lfixture/Carrier;->compareTo(Ljava/lang/Object;)I"):
                    wanted["access"] &= ~0x1040
                if after["methods"][method] != wanted:
                    raise RuntimeError("Unreported original method metadata changed: " + method)
        rebuilt_exceptions = exception_inventory(rebuilt)
        if any(rebuilt_exceptions[method] != original_exceptions[method] for method in methods):
            raise RuntimeError("Original exception metadata changed")
        runtime = (rebuilt, harness, dependency) if args.dependency_root else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             harness_name, "rebuilt", "inlined" if args.inlined_carrier else "plain"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original checked casts, null checks, scalar calls or parent effects changed")
        for mutation in ("wrong-argument", "missing-synthetic"):
            negative = work / (mutation + "-originals")
            shutil.copytree(originals, negative)
            if mutation == "wrong-argument":
                receipt = change_signature(negative / "fixture/Core.class", input_signature,
                                           input_signature.replace(b"Lfixture/Carrier;", b"Ljava/lang/Integer;"))
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
        wrong_java.write_text("package fixture; public final class Wrong implements Api<Carrier> {\n"
                             "  public boolean contains(Comparable value) { return false; }\n"
                             "  public Carrier get() { return null; }\n"
                             "  public boolean isEmpty() { return false; }\n"
                             "  public boolean before(Comparable a, Comparable b) { return false; }\n}\n")
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
                    "projected_methods": len(projected), "primitive": primitive, "inlined_carrier": args.inlined_carrier, "value_cases": counts[0], "identity_cases": counts[1],
                    "state_cases": counts[2], "failure_cases": counts[3],
                    "profile": "dependency-root" if args.dependency_root else "source-root",
                    "original_runtime_slots_bodies_and_exceptions": "unchanged",
                    "source_metadata_differences": "two raw interface edges and three retained bridge flag differences" + ("; carrier comparison uses its existing checked-interface projection" if args.inlined_carrier else ""),
                    "same_code_negative_controls": "rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
