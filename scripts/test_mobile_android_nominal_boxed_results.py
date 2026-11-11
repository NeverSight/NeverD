#!/usr/bin/env python3
"""Verify original wide scalar and nominal boxed return slots with disclosed aliases."""
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory

CORE = "Lfixture/Core;"
SCALAR = CORE + "->shift(J)J"
NARROW = CORE + "->shift(J)Lfixture/Api;"
BROAD = CORE + "->shift(J)Lfixture/Parent;"


def encode(root: Path) -> list:
    edits = [omit_owned_interface_bridge(root / "fixture/Core.class", b"shift(J)Lfixture/Parent;")]
    for path in sorted(root.rglob("*.class")):
        before = path.read_bytes()
        after = before
        names = {}
        for old in (b"scalarShift", b"rootShift"):
            encoded = b"\x01" + len(old).to_bytes(2, "big") + old
            if encoded not in after:
                continue
            if after.count(encoded) != 1:
                raise RuntimeError("Ambiguous owned return slot name")
            after = after.replace(encoded, b"\x01\x00\x05shift", 1)
            names[old] = b"shift"
        expected = {}
        for identity, code in method_code(before).items():
            name, prototype = identity.split(b"(", 1)
            expected[names.get(name, name) + b"(" + prototype] = code
        if (expected != method_code(after)
                or before[class_records(before)["pool_end"]:] != after[class_records(after)["pool_end"]:]):
            raise RuntimeError("Owned return naming changed Code, descriptors or attributes")
        if names:
            path.write_bytes(after)
            edits.append({"class": path.relative_to(root).as_posix(), "names": {k.decode(): v.decode() for k, v in names.items()},
                          "code_descriptors_and_attributes": "unchanged"})
    for prototype in (b"(J)Lfixture/Api;", b"(J)Lfixture/Parent;"):
        edits.append(change_method_flags(root / "fixture/Core.class", prototype, 0x1, 0x1041, name=b"shift"))
    return edits


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parent", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nominal-boxed-results-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "nominal_boxed_results/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/NominalBoxedResultsHarness.java"], harness, classpath=originals)
        (work / "owned-encoding.json").write_text(json.dumps(encode(originals), indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Parent;"} if args.dependency_parent else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "original-parent.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("fixture/Parent.class", (originals / "fixture/Parent.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_parent:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "NominalBoxedResultsHarness", "original", "shift"], "baseline")
        if baseline.splitlines() != ["values=665,identities=1304,states=4640,failures=30",
                "original-scalar-boxing-null-allocation-and-failures=matched,alias-and-bridge-flags=reported"]:
            raise RuntimeError("Independent boxed result oracle missed original outcomes: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [p for p in sorted(root.rglob("*.class"))
                     if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned nominal fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes
                or {r["identity"] for r in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original nominal return slot")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        expected = {SCALAR, NARROW, BROAD, CORE + "->compareTo(Ljava/lang/Object;)I",
                    "Lfixture/Api;->compareTo(Ljava/lang/Object;)I", "Lfixture/Api;->shift(J)Lfixture/Parent;"}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial"
                or coverage.get("projected_method_count") != len(expected)
                or {i for i, r in rows.items() if r["status"] == "source-projected"} != expected
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")):
            raise RuntimeError("Nominal boxing changed original runtime inventory or scope")
        if (rows[SCALAR].get("projection_kind") != "incompatible-inherited-return-slot"
                or rows[NARROW].get("projection_kind") != "boxed-nominal-interface-result-bridge"
                or rows[NARROW].get("bridge_target") != SCALAR
                or rows[NARROW].get("box_method") != CORE + "->box(J)Lfixture/Core;"
                or rows[BROAD].get("projection_kind") != "boxed-covariant-interface-result-bridge"
                or rows[BROAD].get("bridge_target") != NARROW
                or rows[BROAD].get("original_scalar_target") != SCALAR
                or any(not rows[i].get("metadata_differences") for i in (SCALAR, NARROW, BROAD))):
            raise RuntimeError("Original scalar target, factory or return covariance lacks its shared proof")
        aliases = {i: r.get("source_identity", i) for i, r in rows.items()}
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_parent else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(aliases.values()) | helpers:
            raise RuntimeError("Rebuilt nominal result changed inventory beyond its scalar alias")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original nominal class/field metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                wanted = dict(metadata)
                if identity == SCALAR:
                    wanted["name"] = rows[SCALAR]["source_name"]
                if identity == NARROW:
                    wanted["access"] &= ~0x1040
                if after["methods"][aliases[identity]] != wanted:
                    raise RuntimeError("Original nominal method metadata changed beyond report: " + identity)
        actual_exceptions = exception_inventory(rebuilt)
        if any(actual_exceptions[aliases[i]] != exceptions[i] for i in methods):
            raise RuntimeError("Original exception metadata changed")
        runtime = (rebuilt, harness, dependency) if args.dependency_parent else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), "NominalBoxedResultsHarness",
                             "rebuilt", rows[SCALAR]["source_name"]], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original scalar, boxing, allocation or exception order changed")
        for label in ("changed-wide-input", "different-factory", "missing-synthetic", "inherited-scalar"):
            negative = work / label
            if label == "missing-synthetic":
                shutil.copytree(originals, negative)
                receipt = change_method_flags(negative / "fixture/Core.class", b"(J)Lfixture/Api;", 0x1041, 0x41, name=b"shift")
            else:
                authored = work / (label + "-java")
                shutil.copytree(fixture, authored)
                path = authored / "fixture/Core.java"
                text = path.read_text()
                if label == "changed-wide-input":
                    text = text.replace("public Api shift(long delta) { return box(scalarShift(delta)); }", "public Api shift(long delta) { return box(scalarShift(delta + 1L)); }")
                elif label == "different-factory":
                    text = text.replace("public Parent rootShift(long delta) { return box(scalarShift(delta)); }", "public static Core otherBox(long input) { return box(input); } public Parent rootShift(long delta) { return otherBox(scalarShift(delta)); }")
                else:
                    parent = authored / "fixture/Parent.java"
                    parent.write_text("package fixture; public interface Parent { Parent shift(long delta); long scalarShift(long delta); }\n")
                path.write_text(text)
                verify.compile_local(sorted(authored.rglob("*.java")), negative)
                receipt = encode(negative)
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            negative_classpath = classpath
            if label == "inherited-scalar" and args.dependency_parent:
                negative_jar = work / "changed-parent.jar"
                with zipfile.ZipFile(negative_jar, "x") as archive:
                    archive.writestr("fixture/Parent.class", (negative / "fixture/Parent.class").read_bytes())
                negative_classpath = ["--classpath", args.android_jar.resolve(), "--classpath", negative_jar]
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, negative_classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Unproved nominal boxing accepted: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original fixture, immutable harness, parent or SDK changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods), "projected_methods": len(expected),
                    "values": 665, "identities": 1304, "states": 4640, "failures": 30,
                    "profile": "parent-dependency" if args.dependency_parent else "source-parent",
                    "original_scalar_body_and_boxing_factory": "retained", "scalar_alias_and_bridge_metadata": "reported",
                    "full_inventory_metadata_and_exceptions": "checked", "independent_counterexamples": "rejected",
                    "original_input_harness_parent_and_sdk_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
