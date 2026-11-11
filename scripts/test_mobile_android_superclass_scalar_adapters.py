#!/usr/bin/env python3
"""Verify original guarded bridges targeting original scalar super adapters."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_plain_integer_bridge import bridge_flag


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
        prefix="neverd-superclass-scalar-adapters-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "superclass_scalar_adapters/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        declarations = {"Lfixture/CountView;"}
        if args.dependency_root:
            declarations.add("Lfixture/Root;")
        classes = all_classes - declarations
        methods = {k: v for k, v in all_methods.items() if k.split("->", 1)[0] in classes}
        verify.compile_local([FIXTURES / "harness/SuperclassScalarAdapterHarness.java"],
                             harness, classpath=originals)
        outer = "Lfixture/Counter;->contains(Ljava/lang/Object;)Z"
        typed = "Lfixture/Counter;->contains(Lfixture/Element;)Z"

        def mark_bridges(directory: Path) -> list:
            path = directory / "fixture/Counter.class"
            return [bridge_flag(path, "contains", "(Ljava/lang/Object;)Z", 0x11, 0x51),
                    bridge_flag(path, "contains", "(Lfixture/Element;)Z", 0x1, 0x41)]

        receipts = mark_bridges(originals)
        (work / "input-flag-changes.json").write_text(json.dumps(receipts, indent=2) + "\n")
        original_records = class_identity.compiler_classes(originals)
        if original_records["Lfixture/Root;"]["methods"][
                "Lfixture/Root;->contains(Ljava/lang/Object;)Z"]["signature"] is not None:
            raise RuntimeError("Plain superclass declaration acquired a generic Signature")
        dependency = work / "owned-declarations.jar"
        with zipfile.ZipFile(dependency, "x", zipfile.ZIP_DEFLATED) as archive:
            for descriptor in sorted(declarations):
                path = descriptor[1:-1] + ".class"
                archive.writestr(path, (originals / path).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "SuperclassScalarAdapterHarness", "original"], "baseline")
        if baseline.splitlines() != ["values=37,identities=30,failures=9",
                "scalar-superclass-adapters=matched,virtual-overrides-and-super-effects=retained,flags=reported"]:
            raise RuntimeError("Original oracle missed its independently authored cases")

        def dex(source: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath",
                        args.android_jar.resolve(), "--classpath", dependency, "--output", destination,
                        *(source / (descriptor[1:-1] + ".class") for descriptor in sorted(classes))],
                       "d8-" + destination.name)
            if list(destination.glob("classes*.dex")) != [destination / "classes.dex"]:
                raise RuntimeError("Owned scalar fixture did not produce exactly one DEX")
            return destination / "classes.dex"

        owned = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", owned], "sdk-inventory")
        inventory = parse_dexdump(dump, owned.name, dex_header(owned.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes
                or {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original scalar declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", owned, "--classpath", args.android_jar.resolve(),
                    "--classpath", dependency, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        projected = {r["identity"]: r for r in coverage["methods"] if r["status"] == "source-projected"}
        if ({r["identity"] for r in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage["projected_method_count"] != 2 or coverage.get("generic_signature_projections")
                or set(projected) != {outer, typed}):
            raise RuntimeError("Original scalar bridge inventory or metadata changed")
        for identity, kind, target in (
                (outer, "guarded-class-forwarding-bridge", typed),
                (typed, "scalar-superclass-forwarding-bridge",
                 "Lfixture/Middle;->contains(Ljava/lang/Object;)Z")):
            row = projected[identity]
            if (row.get("source_identity") != identity or row.get("projection_kind") != kind
                    or row.get("bridge_target") != target or not row.get("metadata_differences")):
                raise RuntimeError("Scalar adapter lost its original call or bridge flag receipt")
        if projected[outer].get("inherited_class_contract") != "Lfixture/Root;->contains(Ljava/lang/Object;)Z":
            raise RuntimeError("Guarded adapter lost its nearest original class contract")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("Rebuilt inventory contains an unreported or missing declaration")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            original, actual = original_records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                if actual.get(key) != original.get(key):
                    raise RuntimeError("Original scalar class metadata changed: " + owner + ":" + key)
            for identity, method in original["methods"].items():
                expected = dict(method)
                if identity in projected:
                    expected["access"] &= ~0x40
                if actual["methods"][identity] != expected:
                    raise RuntimeError("Method metadata changed beyond reported bridge flag: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "SuperclassScalarAdapterHarness", "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Superclass dispatch, identity or effects before failures changed")
        negative_sources = work / "negative-java/fixture"
        negative_sources.mkdir(parents=True)
        for source in inputs:
            content = source.read_text()
            if source.name == "Counter.java":
                original = "return super.contains(value);"
                if content.count(original) != 1:
                    raise RuntimeError("Missing independent scalar superclass forwarding body")
                content = content.replace(original, "calls++; return super.contains(value);")
            (negative_sources / source.name).write_text(content)
        negative = work / "negative-originals"
        verify.compile_local(sorted(negative_sources.glob("*.java")), negative)
        (work / "negative-flag-changes.json").write_text(json.dumps(mark_bridges(negative), indent=2) + "\n")
        negative_dex = dex(negative, work / "negative-dex")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                str(args.android_jar.resolve()), "--classpath", str(dependency), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic inheritance" not in result.stderr:
            raise RuntimeError("Unproven scalar effects were accepted as original forwarding")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original input or independent harness bytes changed")
        if hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash:
            raise RuntimeError("Original dependency declarations changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 37, "identity_cases": 30, "ordered_failure_cases": 9,
                    "original_bridge_count": 2, "rebuilt_bridge_count": 0,
                    "complete_runtime_inventory": "original methods and reported helpers only",
                    "generic_reflection": "unchanged", "superclass_invocation": "retained",
                    "virtual_override": "retained", "effects_before_failure": "matched",
                    "dependency_root": args.dependency_root, "bridge_flag_differences": "reported",
                    "forwarding_effect_counterexample": "rejected", "original_class_sha256": original_hashes,
                    "harness_class_sha256": harness_hashes, "dependency_sha256": dependency_hash,
                    "coverage": "partial", "jvm_output": "matched"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
