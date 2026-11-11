#!/usr/bin/env python3
"""Verify fixed-scope overrides of inherited dependency interface defaults."""
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


def hashes(directory):
    return {p.relative_to(directory).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(directory.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-fixed-scope-default-override-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "fixed_scope_default_override/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        declarations = {"Lfixture/Api;", "Lfixture/Root;", "Lfixture/Trace;"}
        classes = all_classes - declarations
        methods = {k: v for k, v in all_methods.items() if k.split("->", 1)[0] in classes}
        original_records = class_identity.compiler_classes(originals)
        target = "Lfixture/Receiver;->echo(Ljava/lang/String;)Ljava/lang/String;"
        if original_records["Lfixture/Receiver;"]["methods"][target]["signature"] is not None:
            raise RuntimeError("Fixed typed override unexpectedly has a generic method Signature")
        dependency = work / "owned-declarations.jar"
        with zipfile.ZipFile(dependency, "x", zipfile.ZIP_DEFLATED) as archive:
            for descriptor in sorted(declarations):
                path = descriptor[1:-1] + ".class"
                archive.writestr(path, (originals / path).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        verify.compile_local([FIXTURES / "harness/FixedScopeDefaultOverrideHarness.java"],
                             harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "FixedScopeDefaultOverrideHarness"], "baseline")
        if baseline.splitlines() != ["values=9,identities=26,failures=12",
                "fixed-scope=retained,ancestor-default-effects=unchanged,override-effects=ordered"]:
            raise RuntimeError("Original oracle missed its independently authored results")

        def dex(source, destination):
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath",
                        args.android_jar.resolve(), "--classpath", dependency, "--output", destination,
                        *(source / (descriptor[1:-1] + ".class") for descriptor in sorted(classes))],
                       "d8-" + destination.name)
            if list(destination.glob("classes*.dex")) != [destination / "classes.dex"]:
                raise RuntimeError("Owned fixture did not produce exactly one DEX")
            return destination / "classes.dex"

        owned = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", owned], "sdk-inventory")
        inventory = parse_dexdump(dump, owned.name, dex_header(owned.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes
                or {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original runtime declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", owned, "--classpath", args.android_jar.resolve(),
                    "--classpath", dependency, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage["projected_method_count"] != 1 or coverage.get("generic_signature_projections")):
            raise RuntimeError("Plain contracts lost an original bridge or invented metadata")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency, release=11)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("Rebuilt inventory contains an unreported or missing declaration")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            original, actual = original_records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields"):
                if actual.get(key) != original.get(key):
                    raise RuntimeError("Original class metadata changed: " + owner + ":" + key)
            for identity, method in original["methods"].items():
                for key in ("access", "signature", "code", "deprecated_attribute", "runtime_visible_annotations"):
                    if actual["methods"][identity].get(key) != method.get(key):
                        raise RuntimeError("Original method metadata changed: " + identity + ":" + key)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "FixedScopeDefaultOverrideHarness"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Bridge dispatch, identity or effects before failures changed")
        for label, owner, old, wrong in (
                ("fixed-superclass", "Receiver",
                 b"Lfixture/Middle<Ljava/lang/String;>;Lfixture/Api<Ljava/lang/String;>;",
                 b"Lfixture/Middle<Ljava/lang/Integer;>;Lfixture/Api<Ljava/lang/String;>;"),
                ("fixed-contract", "Receiver",
                 b"Lfixture/Middle<Ljava/lang/String;>;Lfixture/Api<Ljava/lang/String;>;",
                 b"Lfixture/Middle<Ljava/lang/Integer;>;Lfixture/Api<Ljava/lang/Integer;>;")):
            negative = work / ("negative-" + label)
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / ("fixture/" + owner + ".class"), old, wrong)
            (work / (label + "-metadata-change.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / ("negative-" + label + "-dex"))
            rejected = work / ("rejected-" + label)
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                    str(args.android_jar.resolve()), "--classpath", str(dependency), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Same-erasure ancestor metadata drift was silently accepted")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original input or independent harness bytes changed")
        if hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash:
            raise RuntimeError("Original dependency declarations changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 9, "identity_cases": 26, "ordered_failure_cases": 12,
                    "original_bridge_count": 1, "fixed_override_signature": "absent and unchanged",
                    "complete_runtime_inventory": "original methods and reported helpers only",
                    "generic_reflection": "unchanged", "ancestor_default_effects": "unchanged through original dependency",
                    "ancestor_effects_during_override": 0,
                    "same_erasure_metadata_counterexamples": "rejected with unchanged Code",
                    "original_class_sha256": original_hashes, "coverage": "partial", "jvm_output": "matched"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
