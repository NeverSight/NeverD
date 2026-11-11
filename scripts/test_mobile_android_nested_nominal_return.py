#!/usr/bin/env python3
"""Verify complete nominal iterator bounds and a witnessed nested Object wildcard."""
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


def hashes(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nested-nominal-return-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "nested_nominal_return/java").rglob("*.java"))
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/NestedNominalReturnHarness.java"], harness,
                             classpath=originals)
        owner = "Lfixture/View;"
        target_method = owner + "->iterator()Ljava/util/Iterator;"
        target_signature = b"()Ljava/util/Iterator<Ljava/util/Map$Entry<Lfixture/Key<*>;Ljava/lang/Object;>;>;"
        source_signature = b"()Ljava/util/Iterator<Ljava/util/Map$Entry<+Lfixture/Key<*>;*>;>;"
        edits = [change_signature(originals / "fixture/View.class", source_signature, target_signature)]
        (work / "owned-input-encoding.json").write_text(json.dumps(edits, indent=2) + "\n")
        records = class_identity.compiler_classes(originals)
        classes, methods = compiler_inventory(verify, originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "NestedNominalReturnHarness", "original"], "baseline")
        if baseline.splitlines() != ["values=132,identities=516,failures=67", "reflection=original"]:
            raise RuntimeError("Owned nested-return baseline missed identity, value or exception-order cases: " + baseline)
        def make_dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", destination, *sorted(root.rglob("*.class"))], "d8-" + destination.name)
            dex = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned nominal iterator DEX inventory changed")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed original nested-return declaration identities")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        projections = coverage.get("generic_signature_projections", [])
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes) or
                coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"] or
                coverage.get("projected_method_count", 0) != 0 or coverage["status"] != "partial" or
                len(projections) != 1 or projections[0].get("declaration") != target_method or
                projections[0].get("original_signature") != target_signature.decode() or
                not projections[0].get("metadata_differences") or len(projections[0]["changes"]) != 1):
            raise RuntimeError("Nested nominal return projection changed inventory or its original result witness")
        change = projections[0]["changes"][0]
        if (change["kind"] != "inherited-nested-return-variance" or
                change["original_type"] != target_signature.decode() or
                change["source_type"] != source_signature.decode() or
                change["scope_origin"] != "Ljava/lang/Iterable;->iterator()Ljava/util/Iterator;"):
            raise RuntimeError("Nested nominal return projection lost the complete original covariant contract")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("Nested-return source added or lost an undisclosed descriptor")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                wanted = before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Nested-return class/field metadata changed: " + identity + ":" + key)
            for member, metadata in before["methods"].items():
                wanted = dict(metadata)
                if member == target_method:
                    wanted["signature"] = source_signature.decode()
                if after["methods"][member] != wanted:
                    raise RuntimeError("Original complete nested-return method metadata changed: " + member)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "NestedNominalReturnHarness", "rebuilt"], "recovered-execution")
        if actual.splitlines() != [baseline.splitlines()[0], "reflection=reported-nested-return"]:
            raise RuntimeError("List identities, unchecked leaves, effects or exception order changed")
        invalid = work / "invalid-java/fixture/View.java"
        invalid.parent.mkdir(parents=True)
        source = (FIXTURES / "nested_nominal_return/java/fixture/View.java").read_text()
        declaration = "Iterator<Map.Entry<? extends Key<?>, ?>> iterator()"
        if source.count(declaration) != 1:
            raise RuntimeError("Owned iterator source declaration changed")
        invalid.write_text(source.replace(declaration,
            "Iterator<Map.Entry<Key<?>, Object>> iterator()", 1))
        compiler = subprocess.run([str(verify.javac), "--release", "8", "-cp", str(originals),
                                   "-d", str(work / "invalid-compiler"), str(invalid)],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "invalid-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("javac accepted the original incompatible nested invariant override")
        class_signature = records[owner]["signature"].encode()
        for label, original_contract, changed_signature in (
                ("wrong-complete-key", target_signature, target_signature.replace(b"Lfixture/Key<*>;", b"Lfixture/Key<Ljava/lang/String;>;")),
                ("nonobject-wildcard", target_signature, target_signature.replace(b"Ljava/lang/Object;", b"Ljava/lang/String;")),
                ("lower-bound", class_signature, class_signature.replace(b"+Lfixture/Key<*>;", b"-Lfixture/Key<*>;"))):
            negative = work / ("negative-" + label)
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/View.class", original_contract, changed_signature)
            (work / (label + "-signature.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = make_dex(negative, work / (label + "-dex"))
            rejected = work / ("rejected-" + label)
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                     str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Nested-return proof accepted a same-erasure generic contract counterexample")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or
                hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original input, SDK or immutable harness bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 132, "identity_cases": 516, "ordered_failure_cases": 67,
                    "original_class_and_field_signatures": "retained",
                    "nested_return_projection_and_full_metadata": "matched to report",
                    "same_erasure_generic_counterexamples": "rejected",
                    "input_sdk_and_immutable_harness": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
