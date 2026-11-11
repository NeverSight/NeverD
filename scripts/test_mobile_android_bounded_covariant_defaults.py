#!/usr/bin/env python3
"""Verify complete bounded generic default contracts and original bridges."""
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-bounded-covariant-defaults-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = FIXTURES / "bounded_covariant_defaults/java"
        inputs = sorted(fixture.rglob("*.java"))
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / ("harness/" + name + ".java") for name in
                              ("BoundedCovariantDefaultsHarness", "BoundedCovariantDefaultsCaller")], harness,
                             classpath=originals)
        classes, methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)

        def hashes(root: Path) -> dict:
            return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(root.rglob("*.class"))}

        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "BoundedCovariantDefaultsHarness"], "baseline")
        expected = ["values=336,identities=553,failures=20",
                    "bounded-default-scopes=retained,original-interface-bridges-and-ordered-effects=matched"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned bounded default missed independent cases: " + baseline)

        def make_dex(source: Path, output: Path) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", output, *sorted(source.rglob("*.class"))], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned bounded default changed DEX inventory")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original bounded default declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] or coverage["status"] != "partial" or
                coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")):
            raise RuntimeError("Bounded default recovery lost an original entry or scope")
        bridge = "Lfixture/Provider;->make(Lfixture/Pair;)Lfixture/ResultBase;"
        target = "Lfixture/Provider;->make(Lfixture/Pair;)Lfixture/ResultLeaf;"
        row = next(r for r in coverage["methods"] if r["identity"] == bridge)
        if (row.get("bridge_target") != target or row.get("projection_kind") != "generic-interface-bridge" or
                row.get("bridge_dispatch") != "Lfixture/Root;->make(Lfixture/Pair;)Lfixture/ResultBase;" or
                not row.get("reason")):
            raise RuntimeError("Original interface bridge lost its target or reported differences")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("javac changed inventory beyond the reported helpers")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original bounded default class/field metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original bounded default method metadata changed: " + identity)
        for helper in coverage.get("generated_source_helpers", []):
            identity = helper["class"] + "->" + helper["name"] + helper["prototype"]
            metadata = actual_records[helper["class"]]["methods"][identity]
            if (helper.get("kind") != "throw-helper" or helper.get("name") != "__neverdThrow" or
                    helper.get("static") is not True or
                    helper.get("prototype") != class_identity.THROW_HELPER_PROTOTYPE or
                    metadata["access"] != 0xA or not metadata["code"] or
                    metadata["signature"] != class_identity.THROW_HELPER_SIGNATURE):
                raise RuntimeError("Reported throw helper changed its exact metadata")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "BoundedCovariantDefaultsHarness", "reported-helper"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Original default/override dispatch, scopes, identity or effect order changed")
        negative = work / "negative-input"
        shutil.copytree(originals, negative)
        signature = b"<R:Lfixture/Bound;>(Lfixture/Pair<Ljava/lang/String;TR;>;)Lfixture/ResultLeaf<TR;>;"
        receipt = change_signature(negative / "fixture/Provider.class", signature,
                                   signature.replace(b"Ljava/lang/String;TR;", b"Ljava/lang/Object;TR;"))
        (work / "negative-parameter-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = make_dex(negative, work / "negative-input-dex")
        rejected = work / "rejected-input"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-input-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Equal erased parameter hid a different complete default contract")
        negative_sources = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "Provider.java":
                content = content.replace("Pair<String, R> input", "Pair<Object, R> input")
            destination = negative_sources / "fixture" / source.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Independent Java compiler accepted the changed complete default input")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original authored input or immutable JVM harness bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 336, "identity_cases": 553, "ordered_failure_cases": 20,
                    "complete_method_bounds_parameters_and_results": "retained",
                    "default_and_subclass_virtual_dispatch": "matched", "ordered_effects_and_bound_casts": "matched",
                    "original_metadata_and_complete_inventory": "unchanged except reported helpers",
                    "parameter_counterexample": "unchanged Code, rejected by NeverD and javac",
                    "original_and_harness_bytes": "unchanged", "coverage": "partial"}
        (work / "oracle-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
