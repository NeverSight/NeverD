#!/usr/bin/env python3
"""Verify complete projected captured scopes against unchanged SDK inheritance."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path
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
        prefix="neverd-captured-sdk-inheritance-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted((FIXTURES / "captured_sdk_inheritance/java").rglob("*.java")), originals)
        classes, methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        verify.compile_local([FIXTURES / "harness/CapturedSdkInheritanceHarness.java"], harness, classpath=originals, release=11)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "CapturedSdkInheritanceHarness", "original"], "baseline")
        if baseline.splitlines() != ["values=103,identities=44,failures=21",
                "sdk-bodies=inherited,captured-outer=mutable,method-formal=unchanged"]:
            raise RuntimeError("Independent SDK oracle missed an authored expected outcome")
        (work / "baseline.txt").write_text(baseline)
        dex_dir = work / "dex"
        dex_dir.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                    "--output", dex_dir, *sorted(originals.rglob("*.class"))], "d8")
        dex = dex_dir / "classes.dex"
        if list(dex_dir.glob("classes*.dex")) != [dex]:
            raise RuntimeError("Owned fixture did not produce exactly one DEX")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes
                or {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the original declaration inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage.get("minimum_java_release") != 25):
            raise RuntimeError("Captured SDK inheritance lost original entries or projection disclosure")
        changes = [c for r in coverage.get("generic_signature_projections", []) for c in r["changes"]]
        captured = [c for c in changes if c["kind"] == "static-inner-type-variable"]
        qualifiers = [c for c in changes if c["kind"] == "static-inner-owner-arguments"]
        if (len(captured) != 2 or len(qualifiers) != 1 or len(changes) != 3
                or any(c["source_type"] != "Ljava/lang/Object;"
                       or c["original_type"] != "Lfixture/Owner;::T" for c in captured)):
            raise RuntimeError("Captured scope changed beyond the independently expected Object projection")
        bindings = coverage.get("class_source_bindings", [])
        if len(bindings) != 1 or bindings[0]["binding_kind"] != "inner-to-static-member":
            raise RuntimeError("Missing captured ownership disclosure")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=25)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("Rebuilt inventory lost or invented an unreported method")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            expected, actual = records[owner], actual_records[owner]
            expected_fields = json.loads(json.dumps(expected["fields"]))
            if owner == "Lfixture/Owner$View;":
                capture = bindings[0].get("outer_capture_field")
                if (capture != "Lfixture/Owner$View;->this$0:Lfixture/Owner;"
                        or expected_fields[capture]["access"] != 0x1010
                        or not any("synthetic capture-field" in text
                                   for text in bindings[0].get("metadata_differences", []))):
                    raise RuntimeError("Missing precise synthetic capture-field reflection disclosure")
                expected_fields[capture]["access"] &= ~0x1000
            if expected_fields != actual["fields"]:
                raise RuntimeError("Original fields changed beyond the disclosed synthetic capture flag")
            for key in ("access", "superclass", "interfaces"):
                if expected[key] != actual[key]:
                    raise RuntimeError("Original runtime class metadata changed: " + owner + ":" + key)
            expected_signature = ("Ljava/util/AbstractCollection<Ljava/lang/Object;>;"
                                  if owner == "Lfixture/Owner$View;" else expected["signature"])
            if actual["signature"] != expected_signature:
                raise RuntimeError("Original class Signature changed beyond the reported captured projection")
            expected_inner = dict(expected["inner_class"]) if expected["inner_class"] else None
            if expected_inner:
                expected_inner["access"] |= 8
            if actual["inner_class"] != expected_inner or actual["enclosing_method"] != expected["enclosing_method"]:
                raise RuntimeError("Captured lexical ownership changed beyond the reported static flag")
            for identity, method in expected["methods"].items():
                expected_signature = method["signature"]
                if identity == "Lfixture/Owner$View;->iterator()Ljava/util/Iterator;":
                    expected_signature = "()Ljava/util/Iterator<Ljava/lang/Object;>;"
                elif identity == "Lfixture/Owner;->create()Lfixture/Owner$View;":
                    expected_signature = None
                if actual["methods"][identity]["signature"] != expected_signature:
                    raise RuntimeError("Original method Signature changed beyond the reported captured projection")
                for key in ("access", "code", "deprecated_attribute", "runtime_visible_annotations"):
                    if method[key] != actual["methods"][identity][key]:
                        raise RuntimeError("Original method metadata changed: " + identity + ":" + key)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "CapturedSdkInheritanceHarness", "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Inherited SDK behavior, capture identities or ordered effects changed")
        negative = work / "negative"
        shutil.copytree(originals, negative)
        receipt = change_signature(negative / "fixture/Owner.class",
            b"<T:Ljava/lang/Object;>Ljava/lang/Object;",
            b"<T:Ljava/lang/Object;:Ljava/lang/CharSequence;>Ljava/lang/Object;")
        (work / "negative-metadata-change.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = work / "negative-dex"
        negative_dex.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                    "--output", negative_dex, *sorted(negative.rglob("*.class"))], "negative-d8")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex / "classes.dex"),
                                "--classpath", str(args.android_jar.resolve()), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Unproved same-erasure captured bound drift was silently accepted")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original input or independent harness changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 103, "identity_cases": 44, "ordered_failure_cases": 21,
                    "same_erasure_bound_counterexample": "rejected with unchanged Code",
                    "jvm_output": baseline.splitlines(), "original_class_sha256": original_hashes,
                    "runtime_inventory": "original entries and reported helpers only",
                    "inherited_sdk_bodies": "unchanged", "scope_projection": "reported Object only",
                    "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
