#!/usr/bin/env python3
"""Verify disclosed raw source results for exact final nominal-element getters."""
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
        prefix="neverd-nominal-element-getters-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = FIXTURES / "nominal_element_getters/java"
        inputs = sorted(fixture.rglob("*.java"))
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/NominalElementGettersHarness.java"], harness,
                             classpath=originals)
        classes, methods = compiler_inventory(verify, originals)
        original_signature = b"()Ljava/util/List<+Lfixture/GetterBase;>;"
        input_signature = original_signature.replace(b"<+", b"<")
        receipt = change_signature(originals / "fixture/GetterApi.class", original_signature, input_signature)
        (work / "owned-interface-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        records = class_identity.compiler_classes(originals)

        def hashes(root: Path) -> dict:
            return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(root.rglob("*.class"))}

        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "NominalElementGettersHarness"], "baseline")
        expected = ["values=47,identities=109,failures=25",
                    "nominal-element-getter-erasure=measured,field-contract-identity-dispatch-and-element-casts=matched"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned nominal-element getter missed independent cases: " + baseline)

        def make_dex(source: Path, output: Path) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", output, *sorted(source.rglob("*.class"))], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned nominal-element getter changed DEX inventory")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original nominal-element getter declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] or coverage["status"] != "partial" or
                coverage.get("class_source_bindings")):
            raise RuntimeError("Nominal getter recovery lost an original entry or scope")
        getters = {"Lfixture/" + owner + ";->get()Ljava/util/List;"
                   for owner in ("GetterDirect", "GetterOwner")}
        projections = coverage.get("generic_signature_projections", [])
        if {r["declaration"] for r in projections} != getters or len(projections) != 2:
            raise RuntimeError("Nominal getter lost its exact result-erasure inventory")
        for row in projections:
            if (row.get("original_signature") != "()Ljava/util/List<Lfixture/GetterLeaf;>;" or
                    len(row.get("changes", [])) != 1):
                raise RuntimeError("Nominal getter changed an unrelated original Signature")
            change = row["changes"][0]
            if (change.get("kind") != "inherited-nominal-getter-result-erasure" or
                    change.get("source_type") != "()Ljava/util/List;" or
                    change.get("scope_origin") != "Lfixture/GetterApi;->get()Ljava/util/List;"):
                raise RuntimeError("Nominal getter lost its complete inherited witness")
        for row in coverage["methods"]:
            if row["identity"] in getters and (
                    row.get("generic_return_contract") != "nominal-getter-result-erasure" or
                    row.get("inherited_nominal_getter_contracts") != ["Lfixture/GetterApi;->get()Ljava/util/List;"]):
                raise RuntimeError("Nominal getter did not disclose its exact inherited result")
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
                    raise RuntimeError("Original nominal-element getter class/field metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                expected_metadata = dict(metadata)
                if identity in getters:
                    expected_metadata["signature"] = None
                if after["methods"][identity] != expected_metadata:
                    raise RuntimeError("Original nominal getter method metadata changed beyond reported result erasure: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "NominalElementGettersHarness", "rebuilt"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Original getter dispatch, list identity, mutation or element cast order changed")
        negative = work / "negative-input"
        shutil.copytree(originals, negative)
        signature = b"Ljava/util/List<Lfixture/GetterLeaf;>;"
        receipt = change_signature(negative / "fixture/GetterOwner.class", signature,
                                   signature.replace(b"GetterLeaf", b"GetterOther"))
        (work / "negative-parameter-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = make_dex(negative, work / "negative-input-dex")
        rejected = work / "rejected-input"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-input-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Equal erased parameter hid a different complete final-field contract")
        negative_sources = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "GetterOwner.java":
                original = "private final java.util.List<GetterLeaf> values;"
                changed = "private final java.util.List<GetterOther> values;"
                if content.count(original) != 1:
                    raise RuntimeError("Missing exact independently authored final-field counterexample")
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
            raise RuntimeError("Independent Java compiler accepted the changed complete final-field type")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original authored input or immutable JVM harness bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 47, "identity_cases": 109, "ordered_failure_cases": 25,
                    "original_final_field_and_constructor_contracts": "retained",
                    "direct_and_inherited_dispatch": "matched", "list_identity_mutation_and_original_element_casts": "matched",
                    "absent_original_class_signatures": "unchanged",
                    "original_metadata_and_complete_inventory": "unchanged except reported getter result erasure and helpers",
                    "field_counterexample": "same erasure and unchanged Code, rejected by NeverD and javac",
                    "original_and_harness_bytes": "unchanged", "coverage": "partial"}
        (work / "oracle-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
