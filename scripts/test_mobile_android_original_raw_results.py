#!/usr/bin/env python3
"""Verify original raw results implementing closed parameterized interface returns."""
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
        prefix="neverd-original-raw-results-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = FIXTURES / "original_raw_results/java"
        inputs = sorted(fixture.rglob("*.java"))
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/OriginalRawResultsHarness.java"], harness,
                             classpath=originals)
        classes, methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)

        def hashes(root: Path) -> dict:
            return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(root.rglob("*.class"))}

        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "OriginalRawResultsHarness"], "baseline")
        expected = ["values=38,identities=35,failures=4",
                    "original-raw-results-and-complete-parameters=retained,allocation-dispatch-and-effect-order=matched"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned raw-result method missed independent cases: " + baseline)

        def make_dex(source: Path, output: Path) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", output, *sorted(source.rglob("*.class"))], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned raw-result method changed DEX inventory")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original raw-result method declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] or coverage["status"] != "recovered" or
                coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")):
            raise RuntimeError("Original raw-result recovery lost an original entry or scope")
        raw = "Lfixture/RawOwner;->get(Lfixture/RawTag;)Lfixture/RawBox;"
        row = next(r for r in coverage["methods"] if r["identity"] == raw)
        if (row.get("generic_return_contract") != "original-raw-result" or
                row.get("inherited_raw_result_contracts") !=
                ["Lfixture/RawApi;->get(Lfixture/RawTag;)Lfixture/RawBox;"] or
                row.get("source_compiler_diagnostics") != "unchecked return conversion" or
                row.get("generic_signature") or records["Lfixture/RawOwner;"]["methods"][raw].get("signature")):
            raise RuntimeError("Original raw result lost its exact contract receipt or acquired a Signature")
        for owner in ("RawApi", "RawOwner"):
            if records["Lfixture/" + owner + ";"].get("signature"):
                raise RuntimeError("Owned nongeneric raw-result fixture acquired a class Signature")
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
                    raise RuntimeError("Original raw-result method class/field metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original raw-result method method metadata changed: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "OriginalRawResultsHarness"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Original raw-result dispatch, allocation, identity or effect order changed")
        negative = work / "negative-input"
        shutil.copytree(originals, negative)
        signature = b"(Lfixture/RawBox<Lfixture/RawTag;>;)V"
        receipt = change_signature(negative / "fixture/RawOwner.class", signature,
                                   signature.replace(b"Lfixture/RawTag;", b"Ljava/lang/Object;"))
        (work / "negative-parameter-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = make_dex(negative, work / "negative-input-dex")
        rejected = work / "rejected-input"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-input-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Equal erased parameter hid a different complete parameter contract")
        negative_sources = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "RawOwner.java":
                original = "void accept(RawBox<RawTag> box) { accepted = box; accepts++; }"
                changed = "void accept(RawBox<Object> box) { accepts++; }"
                if content.count(original) != 1:
                    raise RuntimeError("Missing exact independently authored parameter counterexample")
                content = content.replace(original, changed)
            destination = negative_sources / "fixture" / source.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Independent Java compiler accepted the changed complete parameter type")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original authored input or immutable JVM harness bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 38, "identity_cases": 35, "ordered_failure_cases": 4,
                    "original_raw_result_and_complete_parameters": "retained",
                    "raw_result_dispatch_allocation_and_failure_identity": "matched", "input_identity_and_effect_order": "matched",
                    "absent_original_class_signatures": "unchanged",
                    "original_metadata_and_complete_inventory": "unchanged except reported helpers",
                    "parameter_counterexample": "same erasure and unchanged Code, rejected by NeverD and javac",
                    "original_and_harness_bytes": "unchanged", "coverage": "recovered"}
        (work / "oracle-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
