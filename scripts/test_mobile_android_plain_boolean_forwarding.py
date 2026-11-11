#!/usr/bin/env python3
"""Compare owned boolean forwarding with complete fixed Object contracts."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag


def hashes(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


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
        prefix="neverd-plain-boolean-forwarding-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "plain_boolean_forwarding/java").rglob("*.java"))
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/PlainBooleanForwardingHarness.java"], harness, classpath=originals)
        prototype = "(Ljava/lang/Object;)Z"
        bridges = {"Lfixture/" + name + ";->add" + prototype for name in ("Counter", "FinalCounter")}
        receipts = {name: bridge_flag(originals / ("fixture/" + name + ".class"), "add", prototype,
                                     original_flags=flags, input_flags=flags | 0x1040)
                    for name, flags in (("Counter", 0x11), ("FinalCounter", 0x1))}
        (work / "owned-input-flags.json").write_text(json.dumps(receipts, indent=2) + "\n")
        original_records = class_identity.compiler_classes(originals)
        classes, methods = compiler_inventory(verify, originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "PlainBooleanForwardingHarness", "original"], "baseline")
        if baseline.splitlines() != ["values=242,identities=92,failures=15",
                                     "plain-boolean-forwarding-and-virtual-dispatch=matched,full-generic-metadata=retained"]:
            raise RuntimeError("Owned boolean forwarding baseline missed cases: " + baseline)

        def make_dex(root: Path, output: Path) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", output, *sorted(root.rglob("*.class"))], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned boolean forwarding changed DEX inventory")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes
                or {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original boolean forwarding declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["projected_method_count"] != 2 or coverage["unrecovered_method_count"]
                or coverage["status"] != "partial" or coverage.get("generic_signature_projections")):
            raise RuntimeError("Boolean forwarding recovery lost bodies, scopes or conservative coverage")
        projected = [r for r in coverage["methods"] if r["status"] == "source-projected"]
        if {r["identity"] for r in projected} != bridges:
            raise RuntimeError("Boolean forwarding projected a different declaration")
        for row in projected:
            if (row["source_identity"] != row["identity"] or not row.get("metadata_differences")
                    or row["projection_kind"] != "plain-interface-forwarding-bridge"
                    or row["bridge_target"] != row["identity"].replace("->add(", "->addImpl(")):
                raise RuntimeError("Boolean forwarding lost its exact original call receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("javac changed inventory beyond reported helpers")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            before, after = original_records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Boolean forwarding changed full metadata: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                wanted = dict(metadata)
                if identity in bridges:
                    wanted["access"] &= ~0x1040
                if after["methods"][identity] != wanted:
                    raise RuntimeError("Boolean forwarding changed unreported method metadata: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "PlainBooleanForwardingHarness", "rebuilt"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Boolean forwarding changed values, argument identity or virtual dispatch")
        negative = work / "negative-input"
        shutil.copytree(originals, negative)
        receipt = change_signature(negative / "fixture/Counter.class",
                                   b"Ljava/lang/Object;Lfixture/Api<Ljava/lang/Object;>;",
                                   b"Ljava/lang/Object;Lfixture/Api<Ljava/lang/String;>;")
        (work / "negative-parent-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = make_dex(negative, work / "negative-dex")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Equal erased parent hid a different complete boolean input contract")
        counter = work / "compiler-counters/fixture"
        counter.mkdir(parents=True)
        compiler_inputs = []
        for source in inputs:
            destination = counter / source.name
            content = source.read_text()
            if source.name == "Counter.java":
                content = content.replace("Api<Object>", "Api<String>", 1)
            destination.write_text(content)
            compiler_inputs.append(destination)
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *map(str, compiler_inputs)],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Independent javac accepted the changed complete boolean input")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original input or immutable JVM harness bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 242, "identity_cases": 92, "ordered_failure_cases": 15,
                    "same_owner_boolean_dispatch": "original call and overridden target matched",
                    "original_complete_fixed_Object_scope": "retained without Signature projection",
                    "original_metadata_and_complete_inventory": "unchanged except reported bridge flags and helpers",
                    "parent_counterexample": "same erasure and unchanged Code, rejected by NeverD and javac",
                    "original_and_harness_bytes": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
