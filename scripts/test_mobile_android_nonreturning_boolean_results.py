#!/usr/bin/env python3
"""Verify reported boolean source results for proven original terminal throws."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_private_producer_return_slots import rename_owned_entry
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_private_producer_return_slots import rename_owned_entry


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
        prefix="neverd-nonreturning-boolean-results-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = FIXTURES / "nonreturning_boolean_results/java"
        inputs = sorted(fixture.rglob("*.java"))
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/NonReturningBooleanResultsHarness.java"], harness,
                             classpath=originals)

        def encode(root: Path) -> list:
            return [change_signature(root / "fixture/View.class",
                                     b"<T:Ljava/lang/Object;>Lfixture/Root;",
                                     b"<T:Ljava/lang/Object;>Lfixture/Root<Lfixture/Entry<TT;>;TT;>;"),
                    rename_owned_entry(root / "fixture/View.class", b"typedThrow", b"add"),
                    rename_owned_entry(root / "fixture/Caller.class", b"typedThrow", b"add"),
                    rename_owned_entry(root / "fixture/View.class", b"throwMany", b"addAll"),
                    rename_owned_entry(root / "fixture/Caller.class", b"throwMany", b"addAll"),
                    rename_owned_entry(root / "fixture/View.class", b"throwScalar", b"push"),
                    rename_owned_entry(root / "fixture/Caller.class", b"throwScalar", b"push"),
                    bridge_flag(root / "fixture/View.class", "add", "(Ljava/lang/Object;)Z", 0x1, 0x1041),
                    bridge_flag(root / "fixture/View.class", "addAll", "(Ljava/util/Collection;)Z", 0x1, 0x1041),
                    bridge_flag(root / "fixture/View.class", "push", "(Ljava/lang/Object;)Z", 0x1, 0x1041)]

        receipts = encode(originals)
        (work / "owned-input-edits.json").write_text(json.dumps(receipts, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)

        def hashes(root: Path) -> dict:
            return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(root.rglob("*.class"))}

        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "NonReturningBooleanResultsHarness"], "baseline")
        expected = ["values=44,identities=67,failures=38",
                    "nonreturning-prefix-casts-and-exceptions=matched,distinct-void-and-boolean-entries=matched"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned nonreturning baseline missed effect or exception cases: " + baseline)

        def make_dex(source: Path, output: Path) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", output, *sorted(source.rglob("*.class"))], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned terminal fixture changed DEX inventory")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes or
                {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original typed or erased terminal entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        typed = "Lfixture/View;->add(Lfixture/Entry;)Ljava/lang/Void;"
        bridge = "Lfixture/View;->add(Ljava/lang/Object;)Z"
        typed_all = "Lfixture/View;->addAll(Ljava/util/Collection;)Ljava/lang/Void;"
        bridge_all = "Lfixture/View;->addAll(Ljava/util/Collection;)Z"
        typed_scalar = "Lfixture/View;->push(Ljava/lang/Object;)Ljava/lang/Void;"
        bridge_scalar = "Lfixture/View;->push(Ljava/lang/Object;)Z"
        projected = {row["identity"]: row for row in coverage["methods"] if row["status"] == "source-projected"}
        if ({row["identity"] for row in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] or coverage["status"] != "partial" or
                coverage.get("generic_signature_projections") or
                set(projected) != {typed, bridge, typed_all, bridge_all, typed_scalar, bridge_scalar}):
            raise RuntimeError("Terminal recovery lost an original declaration or undisclosed scope")
        for target, adapter in ((typed, bridge), (typed_all, bridge_all), (typed_scalar, bridge_scalar)):
            row = projected[target]
            if (row["projection_kind"] != "nonreturning-boolean-result"
                    or row["source_identity"] != target.replace("->", "->__neverdNonReturning_", 1)
                    or row["source_generic_signature"] != row["generic_signature"]
                    or row["original_bridge"] != adapter
                    or projected[adapter]["projection_kind"] != "nonreturning-boolean-result-bridge"
                    or projected[adapter]["source_identity"] != adapter
                    or not row["metadata_differences"] or not projected[adapter]["metadata_differences"]):
                raise RuntimeError("Terminal target and adapter lost their distinct bodies or descriptors")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        source_ids = {row["identity"]: row.get("source_identity", row["identity"]) for row in coverage["methods"]}
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(source_ids.values()) | helpers:
            raise RuntimeError("javac changed the complete original terminal inventory")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original terminal class/field metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                wanted = dict(metadata)
                if identity in (typed, typed_all, typed_scalar):
                    wanted["name"] = projected[identity]["source_identity"].split("->")[1].split("(")[0]
                elif identity in (bridge_all, bridge_scalar):
                    target = typed_all if identity == bridge_all else typed_scalar
                    wanted["access"] &= ~0x1040
                    wanted["signature"] = projected[target]["generic_signature"].replace(")Ljava/lang/Void;", ")Z")
                if after["methods"][source_ids[identity]] != wanted:
                    raise RuntimeError("Terminal metadata changed beyond the exact reported result: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "NonReturningBooleanResultsHarness", "boolean"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Terminal prefix, parameter cast or exception identity/order changed")

        qualified = work / "qualified"
        verify.run([sys.executable, Path(__file__).with_name("verify_mobile_android_recovery.py"), output,
                    "--input", dex, "--javac", verify.javac, "--dexdump", verify.dexdump,
                    "--keep-classes", "-o", qualified], "qualify-compiled-methods")
        final = json.loads((qualified / "metadata/android-methods.json").read_text())
        if (final["status"] != "recovered" or final["compilation_verification"]["distinct_compiled_methods"] != len(methods)
                or final["compilation_verification"]["restored_method_flags"] < 2
                or len({row["compiled_identity"] for row in final["methods"]}) != len(methods)):
            raise RuntimeError("Compiled recovery did not preserve every original entry and bridge flag")
        result = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (qualified / "classes", harness))),
                             "NonReturningBooleanResultsHarness", "verified"], "verified-execution")
        if result != baseline:
            raise RuntimeError("Rebuilt bridge flags, original throw order or generic Void result changed")

        # Still-compilable source edits must not inherit the verified export.
        unit = output / "sources/fixture/View.java"
        saved = unit.read_bytes()
        bad = work / "modified-verified"
        try:
            unit.write_bytes(saved + b"\n// valid Java change\n")
            attempt = subprocess.run([sys.executable, Path(__file__).with_name("verify_mobile_android_recovery.py"),
                                      output, "--input", dex, "--javac", verify.javac, "--dexdump",
                                      verify.dexdump, "-o", bad], text=True, capture_output=True)
            if attempt.returncode == 0 or bad.exists() or "changed since decompilation" not in attempt.stderr:
                raise RuntimeError("Modified source qualified under the original recovery report")
        finally:
            unit.write_bytes(saved)

        def reject(negative: Path, label: str) -> None:
            negative_dex = make_dex(negative, work / ("negative-" + label + "-dex"))
            rejected = work / ("rejected-" + label)
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                     str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / ("negative-" + label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Nonreturning result proof ignored original return or scope drift")

        negative = work / "negative-scope"
        shutil.copytree(originals, negative)
        receipt = change_signature(negative / "fixture/View.class", b"(Lfixture/Entry<TT;>;)Ljava/lang/Void;",
                                   b"(Lfixture/Entry<Ljava/lang/String;>;)Ljava/lang/Void;")
        (work / "negative-scope-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        reject(negative, "scope")
        negative_sources = work / "negative-return-java"
        shutil.copytree(fixture, negative_sources)
        target = negative_sources / "fixture/View.java"
        content = target.read_text()
        if content.count("throw new Failure();") != 3:
            raise RuntimeError("Missing exact independently authored terminal throw")
        target.write_text(content.replace("throw new Failure();", "return null;", 1))
        negative_return = work / "negative-return"
        verify.compile_local(sorted(negative_sources.rglob("*.java")), negative_return)
        (work / "negative-return-input-edits.json").write_text(json.dumps(encode(negative_return), indent=2) + "\n")
        reject(negative_return, "return")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original independently authored input or immutable harness changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 44, "identity_cases": 67, "ordered_failure_cases": 38,
                    "full_class_field_and_parameter_metadata": "unchanged", "typed_void_result_and_signature": "preserved",
                    "distinct_original_entries": "verified", "restored_bridge_flags": "verified",
                    "scope_and_normal_return_counterexamples": "rejected", "original_and_harness_bytes": "unchanged"}
        (work / "oracle-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
