#!/usr/bin/env python3
"""Verify disclosed compiler slots for original substituted abstract returns."""
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
    from scripts.test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge
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
        prefix="neverd-substituted-interface-returns-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = FIXTURES / "substituted_interface_returns/java"
        inputs = sorted(fixture.rglob("*.java"))
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/SubstitutedInterfaceReturnsHarness.java"], harness,
                             classpath=originals)
        edit = omit_owned_interface_bridge(originals / "fixture/Narrow.class", b"next()Ljava/lang/Object;")
        (work / "owned-interface-slot-edit.json").write_text(json.dumps(edit, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)

        def hashes(root: Path) -> dict:
            return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(root.rglob("*.class"))}

        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "SubstitutedInterfaceReturnsHarness"], "baseline")
        if baseline.splitlines() != ["values=10,identities=16,failures=4",
                                     "typed-and-erased-iterator-dispatch=matched,added-interface-bridge=reported"]:
            raise RuntimeError("Owned iterator baseline missed effects or identity cases: " + baseline)

        def make_dex(source: Path, output: Path) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", output, *sorted(source.rglob("*.class"))], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned iterator fixture changed DEX inventory")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original iterator declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        typed, added = "Lfixture/Narrow;->next()Lfixture/Item;", "Lfixture/Narrow;->next()Ljava/lang/Object;"
        if ({r["identity"] for r in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] or coverage["status"] != "partial" or
                coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")):
            raise RuntimeError("Iterator recovery lost an original entry or full contract")
        row = next(r for r in coverage["methods"] if r["identity"] == typed)
        if (row.get("projection_kind") != "interface-covariant-runtime-bridges" or
                row.get("generated_bridges") != [added] or not row.get("metadata_differences")):
            raise RuntimeError("Additional compiler slot is confused with an input declaration")
        bridge_helpers = [r for r in coverage.get("generated_source_helpers", [])
                          if r.get("kind") == "compiler-covariant-interface-bridge"]
        if (len(bridge_helpers) != 1 or bridge_helpers[0].get("class") != "Lfixture/Narrow;" or
                bridge_helpers[0].get("prototype") != "()Ljava/lang/Object;" or
                bridge_helpers[0].get("bridge_target") != typed or
                bridge_helpers[0].get("access_flags") != ["public", "bridge", "synthetic"]):
            raise RuntimeError("Generated iterator bridge lacks its exact original typed declaration")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("javac changed inventory beyond the reported generated slots")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original iterator class/field metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original iterator method metadata changed: " + identity)
        generated = actual_records["Lfixture/Narrow;"]["methods"][added]
        if generated["access"] != 0x1041 or generated["signature"] is not None or not generated["code"]:
            raise RuntimeError("Compiler slot has the wrong bridge/default metadata")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "SubstitutedInterfaceReturnsHarness", "generated"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Original narrow/erased dispatch, identities or ordered failures changed")
        negative = work / "negative-result"
        shutil.copytree(originals, negative)
        receipt = change_signature(negative / "fixture/Narrow.class",
                                   b"Ljava/lang/Object;Ljava/util/Iterator<Lfixture/Item;>;",
                                   b"Ljava/lang/Object;Ljava/util/Iterator<Lfixture/Other;>;")
        (work / "negative-result-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = make_dex(negative, work / "negative-result-dex")
        rejected = work / "rejected-result"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-result-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Different complete result argument authorized an incorrect generated slot")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original independently authored input or immutable harness changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 10, "identity_cases": 16, "ordered_failure_cases": 4,
                    "added_interface_slot": "reported and measured", "original_declarations_and_metadata": "unchanged",
                    "complete_rebuilt_inventory": "original slots plus reported helpers only",
                    "result_counterexample": "rejected", "original_and_harness_bytes": "unchanged"}
        (work / "oracle-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
