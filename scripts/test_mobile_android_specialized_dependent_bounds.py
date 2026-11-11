#!/usr/bin/env python3
"""Verify concrete specialization of dependent method bounds and original runtime bridges."""
from __future__ import annotations
import argparse
import copy
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
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory

CORE_SIGNATURE = b"<A:Lfixture/Value;B:TA;>([TA;TB;Lfixture/Sink<-TA;>;)[TA;"
API_SIGNATURE = b"<R:TC;S:TR;>([TR;TS;Lfixture/Sink<-TR;>;)[TR;"
RESUME_SIGNATURE = b"<R:TC;>(TR;Lfixture/Sink<-TR;>;)V"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parents", action="store_true")
    parser.add_argument("--interface-slot", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-specialized-dependent-bounds-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "specialized_dependent_bounds/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / ("harness/" + name + ".java")
                              for name in ("SpecializedDependentBoundsCaller", "SpecializedDependentBoundsHarness")],
                             harness, classpath=originals)
        if args.interface_slot:
            receipts = [change_signature(originals / "fixture/Api.class", signature, signature.replace(b":T", b"::T"))
                        for signature in (API_SIGNATURE, RESUME_SIGNATURE)]
            (work / "owned-bound-positions.json").write_text(json.dumps(receipts, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Api;", "Lfixture/Sink;", "Lfixture/Value;"} if args.dependency_parents else set())
        methods = {i for i in all_methods if i.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "original-declarations.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Api", "Sink", "Value"):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_parents:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "SpecializedDependentBoundsHarness"], "baseline")
        if baseline.splitlines() != ["identities=936,states=700,failures=98",
                "specialized-method-bounds-original-bridges-captured-owner-and-callback-effects=retained"]:
            raise RuntimeError("Independent dependent-bound oracle missed original outcomes: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [p for p in sorted(root.rglob("*.class")) if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned dependent-bound fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original dependent declarations")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        bridges = {identity for owner in ("Lfixture/Core;", "Lfixture/Host$Worker;")
                   for identity, metadata in records[owner]["methods"].items() if metadata["access"] & 0x40}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        captured = {i for i in methods if i.startswith("Lfixture/Host$Worker;->")} - bridges
        bindings = {r["class"]: r for r in coverage.get("class_source_bindings", [])}
        if (set(bindings) != {"Lfixture/Host$Worker;"}
                or bindings["Lfixture/Host$Worker;"]["binding_kind"] != "inner-to-static-member"
                or bindings["Lfixture/Host$Worker;"].get("outer_capture_field") != "Lfixture/Host$Worker;->this$0:Lfixture/Host;"
                or not bindings["Lfixture/Host$Worker;"].get("metadata_differences")
                or any(rows[i].get("projection_kind") != "inner-to-static-member" for i in captured)):
            raise RuntimeError("Original captured owner and field projection were not disclosed exactly")
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count") != 10
                or projected != bridges | captured or coverage["status"] != "partial"
                or any(rows[i].get("projection_kind") != "generic-interface-bridge" for i in bridges)):
            raise RuntimeError("Specialized bounds changed original inventory or bridge receipts")
        projections = coverage.get("generic_signature_projections", [])
        expected = {"Lfixture/Api;->copy([Ljava/lang/Object;Ljava/lang/Object;Lfixture/Sink;)[Ljava/lang/Object;": 2,
                    "Lfixture/Api;->resume(Ljava/lang/Object;Lfixture/Sink;)V": 1} if not args.dependency_parents else {}
        if args.interface_slot:
            if ({r["declaration"] for r in projections} != set(expected) or len(projections) != len(expected)
                    or any(len(r["changes"]) != expected[r["declaration"]]
                           or any(c["kind"] != "type-variable-bound-position" for c in r["changes"]) for r in projections)):
                raise RuntimeError("Original bound encoding change was not reported exactly")
        elif projections:
            raise RuntimeError("Ordinary method scopes should remain unchanged")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8), classpath=dependency if args.dependency_parents else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != methods | helpers:
            raise RuntimeError("Dependent bounds changed complete runtime inventory")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            before, after = copy.deepcopy(records[owner]), actual_records[owner]
            if owner in bindings:
                capture = bindings[owner]["outer_capture_field"]
                if before["fields"][capture]["access"] != 0x1010:
                    raise RuntimeError("Original capture field flags changed")
                before["fields"][capture]["access"] &= ~0x1000
                before["inner_class"]["access"] |= 8
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original class/field scope changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                wanted = dict(metadata)
                if args.interface_slot and identity in expected:
                    wanted["signature"] = wanted["signature"].replace("::T", ":T")
                if after["methods"][identity] != wanted:
                    raise RuntimeError("Complete dependent method metadata changed beyond bound position: " + identity)
        actual_exceptions = exception_inventory(rebuilt)
        if any(actual_exceptions[i] != exceptions[i] for i in methods):
            raise RuntimeError("Original dependent exception declarations changed")
        runtime = (rebuilt, harness, dependency) if args.dependency_parents else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), "SpecializedDependentBoundsHarness"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original dependent scopes, values, callbacks or failures changed")
        for label, changed in (("independent-second-bound", CORE_SIGNATURE.replace(b"B:TA;", b"B:Lfixture/Value;")),
                               ("different-callback-scope", CORE_SIGNATURE.replace(b"Sink<-TA;>", b"Sink<-TB;>")),
                               ("different-variance", CORE_SIGNATURE.replace(b"Sink<-TA;>", b"Sink<+TA;>"))):
            negative = work / label
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Core.class", CORE_SIGNATURE, changed)
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Equal erasures concealed a changed method scope: " + label)
        authored = work / "invalid-java"
        shutil.copytree(fixture, authored)
        source = authored / "fixture/Core.java"
        text = source.read_text()
        assert text.count("B extends A") == 1
        source.write_text(text.replace("B extends A", "B extends Value"))
        result = subprocess.run([str(verify.javac), "--release", "8", "-proc:none", "-d", str(work / "invalid-classes"),
                                 *map(str, sorted(authored.rglob("*.java")))],
                                capture_output=True, text=True, timeout=300, env=verify.environment)
        (work / "invalid-javac.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0:
            raise RuntimeError("Independent javac accepted an unrelated method-bound chain")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original fixture, harness, declarations or SDK changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "identities": 936, "states": 700, "failures": 98, "bound_reflection_and_callbacks": "unchanged",
                    "interface_slot_encoding": args.interface_slot, "dependency_parents": args.dependency_parents,
                    "bound_position_projections": len(projections), "original_bridges": len(bridges), "source_coverage": coverage["status"],
                    "complete_inventory_metadata_and_exceptions": "checked", "same_erasure_counterexamples": "rejected",
                    "original_input_harness_dependency_and_sdk": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
