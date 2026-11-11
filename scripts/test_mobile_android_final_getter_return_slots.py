#!/usr/bin/env python3
"""Compare owned distinct JVM return entries beside an inherited final getter."""
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-final-getter-return-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "final_getter_return_slots/java").rglob("*.java"))
        inputs += [FIXTURES / "indexed_covariant_returns/java/fixture" / name
                   for name in ("Base.java", "Leaf.java", "Trace.java")]
        verify.compile_local(inputs, originals)
        receipt = rename_owned_entry(originals / "fixture/Receiver.class")
        (work / "owned-entry-name-edit.json").write_text(json.dumps(receipt, indent=2) + "\n")
        for owner, result in (("Parent", "Base"), ("Receiver", "Leaf")):
            receipt = bridge_flag(originals / f"fixture/{owner}.class", "items", f"()Lfixture/{result};")
            (work / f"owned-{owner.lower()}-flag-edit.json").write_text(json.dumps(receipt, indent=2) + "\n")
        verify.compile_local([FIXTURES / "harness/FinalGetterReturnSlotsHarness.java"], harness, classpath=originals)
        classes, methods = compiler_inventory(verify, originals)
        def hashes(root: Path) -> dict:
            return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(root.rglob("*.class"))}
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "FinalGetterReturnSlotsHarness", "items", ""], "baseline")
        if baseline.splitlines() != ["values=100,identities=150,failures=11",
                                    "distinct-final-entries-and-getter-dispatch=matched,source-name-and-bridge-flags=reported"]:
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
                coverage.get("generic_signature_projections") or len(named) != 1):
            raise RuntimeError("Final-return recovery lost an original declaration or full scope")
        row = named[0]
        if (row["identity"] != "Lfixture/Receiver;->items()Lfixture/Leaf;" or
                row["conflicting_inherited_contracts"] != ["Lfixture/Parent;->items()Lfixture/Base;"] or
                row.get("bridge_target") != "Lfixture/Receiver;->getItems()Lfixture/Leaf;" or
                row.get("body_projection_kind") != "generic-reference-interface-forwarding-bridge" or
                not row.get("metadata_differences")):
            raise RuntimeError("Distinct final return entry lacks its complete original body receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        expected_methods = {r.get("source_identity", r["identity"]) for r in coverage["methods"]}
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != expected_methods | helpers:
            raise RuntimeError("javac added an unreported entry or changed original ownership")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "FinalGetterReturnSlotsHarness", row["source_name"],
                             ",".join(sorted({r["class"][1:-1].replace("/", ".") + "->" + r["name"]
                                              for r in coverage.get("generated_source_helpers", [])}))], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Distinct final entry changed identity, dispatch or ordered failure effects")
        for label, old, wrong in (
                ("result", b"()Lfixture/Leaf<TA;>;", b"()Lfixture/Leaf<TB;>;"),
                ("ancestor", b"<A:Ljava/lang/Object;B:Ljava/lang/Object;>Lfixture/Parent<TA;>;",
                 b"<A:Ljava/lang/Object;B:Ljava/lang/Object;>Lfixture/Parent<TB;>;")):
            negative = work / ("negative-" + label)
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Receiver.class", old, wrong)
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
                    "value_cases": 100, "identity_cases": 150, "ordered_failure_cases": 11,
                    "distinct_original_final_return_entries": "retained bodies and getter dispatch",
                    "source_name_and_reflection_differences": "reported partial",
                    "original_code_and_full_generic_metadata": "unchanged",
                    "wrong_result_and_ancestor_arguments": "rejected",
                    "complete_rebuilt_inventory": "reported source identities and helpers only"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
