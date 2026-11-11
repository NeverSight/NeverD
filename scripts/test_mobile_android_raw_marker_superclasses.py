#!/usr/bin/env python3
"""Verify an erasure-neutral raw superclass marker argument source projection."""
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
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature


def hashes(directory: Path) -> dict:
    return {p.relative_to(directory).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(directory.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-root", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-raw-marker-superclasses-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "raw_marker_superclasses/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        declarations = {"Lfixture/Marker;", "Lfixture/ArityView;", "Lfixture/Factory;"}
        if args.dependency_root:
            declarations.add("Lfixture/Root;")
        classes = all_classes - declarations
        methods = {k: v for k, v in all_methods.items() if k.split("->", 1)[0] in classes}
        verify.compile_local([FIXTURES / "harness/RawMarkerSuperclassHarness.java"],
                             harness, classpath=originals)
        authored = (b"Lfixture/Root<Ljava/lang/String;>;"
                    b"Lfixture/Factory<Ljava/lang/Integer;Ljava/lang/Long;Ljava/lang/String;>;")
        original = (b"Lfixture/Root;"
                    b"Lfixture/Factory<Ljava/lang/Integer;Ljava/lang/Long;Ljava/lang/String;>;")
        receipt = change_signature(originals / "fixture/Consumer.class", authored, original)
        (work / "input-metadata-change.json").write_text(json.dumps(receipt, indent=2) + "\n")
        original_records = class_identity.compiler_classes(originals)
        dependency = work / "owned-declarations.jar"
        with zipfile.ZipFile(dependency, "x", zipfile.ZIP_DEFLATED) as archive:
            for descriptor in sorted(declarations):
                path = descriptor[1:-1] + ".class"
                archive.writestr(path, (originals / path).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "RawMarkerSuperclassHarness", "original"], "baseline")
        if baseline.splitlines() != ["values=12,identities=14,failures=6",
                "marker-argument=reported,runtime-descriptors-and-dispatch=retained,ordered-effects=matched"]:
            raise RuntimeError("Original marker oracle missed its independently authored cases")

        def dex(source: Path, destination: Path, classpath: Path = dependency) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath",
                        args.android_jar.resolve(), "--classpath", classpath, "--output", destination,
                        *(source / (descriptor[1:-1] + ".class") for descriptor in sorted(classes))],
                       "d8-" + destination.name)
            if list(destination.glob("classes*.dex")) != [destination / "classes.dex"]:
                raise RuntimeError("Owned marker fixture did not produce exactly one DEX")
            return destination / "classes.dex"

        owned = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", owned], "sdk-inventory")
        inventory = parse_dexdump(dump, owned.name, dex_header(owned.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes
                or {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original marker declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", owned, "--classpath", args.android_jar.resolve(),
                    "--classpath", dependency, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        projections = coverage.get("generic_signature_projections", [])
        if ({r["identity"] for r in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage["projected_method_count"] != 1 or len(projections) != 1
                or projections[0]["declaration"] != "Lfixture/Consumer;"
                or projections[0]["original_signature"] != original.decode()
                or len(projections[0]["changes"]) != 1
                or projections[0]["changes"][0]["kind"] != "raw-marker-superclass-argument"
                or projections[0]["changes"][0]["original_type"] != "Lfixture/Root;"
                or projections[0]["changes"][0]["source_type"] != "Lfixture/Root<Ljava/lang/String;>;"
                or not projections[0].get("metadata_differences")):
            raise RuntimeError("Marker source argument or original inventory lost its receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("Rebuilt inventory contains an unreported or missing declaration")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            expected, actual = original_records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                expected_value = authored.decode() if owner == "Lfixture/Consumer;" and key == "signature" else expected.get(key)
                if actual.get(key) != expected_value:
                    raise RuntimeError("Class metadata changed beyond reported marker argument: " + owner + ":" + key)
            for identity, method in expected["methods"].items():
                if actual["methods"][identity] != method:
                    raise RuntimeError("Original marker method metadata changed: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "RawMarkerSuperclassHarness", "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Marker projection changed original dispatch, identity or effects")
        for label, owner, before, wrong in (
                ("conflicting-arguments", "Consumer", original,
                 original.replace(b"Ljava/lang/Long;Ljava/lang/String;",
                                  b"Ljava/lang/Long;Ljava/lang/Integer;")),
                ("parent-marker", "ArityView",
                 b"<A:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Marker<TA;>;",
                 b"<A:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Marker<Ljava/lang/String;>;")):
            negative = work / ("negative-" + label)
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / ("fixture/" + owner + ".class"), before, wrong)
            (work / (label + "-metadata-change.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dependency = dependency
            if "Lfixture/" + owner + ";" in declarations:
                negative_dependency = work / ("negative-" + label + ".jar")
                with zipfile.ZipFile(negative_dependency, "x", zipfile.ZIP_DEFLATED) as archive:
                    for descriptor in sorted(declarations):
                        path = descriptor[1:-1] + ".class"
                        archive.writestr(path, (negative / path).read_bytes())
            negative_dex = dex(negative, work / ("negative-" + label + "-dex"), negative_dependency)
            rejected = work / ("rejected-" + label)
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                    str(args.android_jar.resolve()), "--classpath", str(negative_dependency), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Same-erasure unproven marker arguments were accepted")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original input or independent harness bytes changed")
        if hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash:
            raise RuntimeError("Original dependency declarations changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 12, "identity_cases": 14, "ordered_failure_cases": 6,
                    "original_bridge_count": 1, "complete_runtime_inventory": "original methods and reported helpers only",
                    "source_superclass_argument": "reported", "original_code_and_descriptors": "unchanged",
                    "dependency_root": args.dependency_root, "generic_method_metadata": "unchanged",
                    "counterexamples": "rejected with unchanged Code and descriptors",
                    "original_class_sha256": original_hashes, "harness_class_sha256": harness_hashes,
                    "dependency_sha256": dependency_hash, "coverage": "partial", "jvm_output": "matched"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
