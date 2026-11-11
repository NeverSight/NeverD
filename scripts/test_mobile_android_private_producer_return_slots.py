#!/usr/bin/env python3
"""Verify private production stays distinct from aliased virtual return entries."""
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
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def rename_owned_entry(path: Path, old: bytes = b"specificItems", new: bytes = b"items") -> dict:
    before = path.read_bytes()
    token = b"\x01" + len(old).to_bytes(2, "big") + old
    replacement = b"\x01" + len(new).to_bytes(2, "big") + new
    if before.count(token) != 1:
        raise RuntimeError("Missing exact owned narrow entry name")
    after = before.replace(token, replacement, 1)
    expected = {identity.replace(old, new): code for identity, code in method_code(before).items()}
    original, changed = class_records(before), class_records(after)
    if (method_code(after) != expected or original["classes"] != changed["classes"] or
            {i: value.replace(old, new) for i, value in original["strings"].items()} != changed["strings"] or
            before[original["pool_end"]:] != after[changed["pool_end"]:]):
        raise RuntimeError("Owned name edit changed Code, flags, descriptors or metadata")
    path.write_bytes(after)
    return {"original_name": old.decode(), "input_name": new.decode(),
            "code_descriptors_flags_and_signatures": "unchanged",
            "before_sha256": hashlib.sha256(before).hexdigest(),
            "after_sha256": hashlib.sha256(after).hexdigest()}


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-private-producer-return-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "private_producer_return_slots/java").rglob("*.java"))
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/PrivateProducerReturnSlotsHarness.java"], harness, classpath=originals)
        receipts = []
        for owner in ("Api", "Parent", "Child", "Grandchild", "Caller"):
            receipts.append(rename_owned_entry(originals / ("fixture/" + owner + ".class"), b"narrowItems", b"items"))
        (work / "owned-entry-name-edits.json").write_text(json.dumps(receipts, indent=2) + "\n")
        original_records = class_identity.compiler_classes(originals)
        classes, methods = compiler_inventory(verify, originals)
        def hashes(root: Path) -> dict:
            return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(root.rglob("*.class"))}
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "PrivateProducerReturnSlotsHarness", "items"], "baseline")
        if baseline.splitlines() != ["values=144,identities=132,failures=6",
                                    "final-private-producer-and-narrow-virtual-dispatch=matched,source-names=reported"]:
            raise RuntimeError("Owned final-return baseline missed cases: " + baseline)
        def make_dex(source: Path, output: Path) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", output, *sorted(source.rglob("*.class"))], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned final-return fixture changed DEX inventory")
            return dex
        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes or
                {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original class or distinct final entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        named = [row for row in coverage["methods"] if row.get("source_name")]
        if ({row["identity"] for row in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] or coverage["status"] != "partial" or
                coverage.get("generic_signature_projections") or len(named) != 4):
            raise RuntimeError("Final-return recovery lost an original declaration or full scope")
        identities = {"Lfixture/" + owner + ";->items()Lfixture/Leaf;" for owner in ("Api", "Parent", "Child", "Grandchild")}
        if ({r["identity"] for r in named} != identities or len({r["source_name"] for r in named}) != 1
                or any(r.get("private_producer") != "Lfixture/Parent;->produce()Lfixture/Leaf;"
                    or r.get("source_name_declaration") != "Lfixture/Parent;->items()Lfixture/Leaf;"
                    or r.get("conflicting_inherited_contracts") != ["Lfixture/Parent;->items()Lfixture/Base;"]
                    or not r.get("metadata_differences") for r in named)):
            raise RuntimeError("Original final private production or virtual alias family lost its witness")
        alias = named[0]["source_name"]
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        expected_methods = {r.get("source_identity", r["identity"]) for r in coverage["methods"]}
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != expected_methods | helpers:
            raise RuntimeError("javac added an unreported entry or changed original ownership")
        actual_records = class_identity.compiler_classes(rebuilt)
        source_ids = {r["identity"]: r.get("source_identity", r["identity"]) for r in coverage["methods"]}
        for owner in classes:
            expected, actual = original_records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                if actual.get(key) != expected.get(key):
                    raise RuntimeError("Original producer class metadata changed: " + owner + ":" + key)
            for identity, method in expected["methods"].items():
                wanted = dict(method)
                if identity in identities:
                    wanted["name"] = alias
                if actual["methods"][source_ids[identity]] != wanted:
                    raise RuntimeError("Method metadata changed beyond disclosed runtime name: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "PrivateProducerReturnSlotsHarness", alias], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Original private production or child/grandchild virtual dispatch changed")
        for label, owner, old, wrong in (
                ("broad-result", "Parent", b"()Lfixture/Base<TT;>;", b"()Lfixture/Base<Ljava/lang/String;>;"),
                ("producer-result", "Parent", b"()Lfixture/Leaf<TT;>;", b"()Lfixture/Leaf<Ljava/lang/String;>;"),
                ("child-result", "Child", b"()Lfixture/Leaf<TC;>;", b"()Lfixture/Leaf<Ljava/lang/String;>;")):
            negative = work / ("negative-" + label)
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / ("fixture/" + owner + ".class"), old, wrong)
            (work / ("negative-" + label + "-signature.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = make_dex(negative, work / ("negative-" + label + "-dex"))
            rejected = work / ("rejected-" + label)
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                    str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / ("negative-" + label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Distinct final return slot ignored complete generic drift")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Owned input or concrete harness changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 144, "identity_cases": 132, "ordered_failure_cases": 6,
                    "distinct_original_final_return_entries": "private production stays separate from virtual child/grandchild overrides",
                    "source_name_and_binary_abi_differences": "reported partial",
                    "original_code_and_full_generic_metadata": "unchanged",
                    "wrong_result_and_ancestor_arguments": "rejected",
                    "complete_rebuilt_inventory": "reported source identities and helpers only"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
