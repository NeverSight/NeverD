#!/usr/bin/env python3
"""Verify owned nominal interface result covariance and disclosed compiler slots."""
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
    from scripts.test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nominal-interface-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "nominal_interface_returns/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/NominalInterfaceReturnHarness.java"], harness, classpath=originals)
        receipt = omit_owned_interface_bridge(originals / "fixture/Refined.class", b"create()Lfixture/Result;")
        (work / "owned-interface-method-edit.json").write_text(json.dumps(receipt, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in originals.rglob("*.class")}
        harness_hashes = {p.relative_to(harness).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in harness.rglob("*.class")}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "NominalInterfaceReturnHarness", "original"], "baseline")
        if baseline.splitlines() != ["identities=24,failures=3",
                "full-nominal-return=retained,original-class-dispatch=matched,added-interface-slot=reported"]:
            raise RuntimeError("Owned baseline missed ancestor dispatch or identity: " + baseline)
        dex = work / "dex"
        dex.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                    "--output", dex, *sorted(originals.rglob("*.class"))], "d8")
        original_dex = dex / "classes.dex"
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", original_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, original_dex.name, dex_header(original_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes or
                {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the independently authored original inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", original_dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        bridge = "Lfixture/Refined;->create()Lfixture/Result;"
        typed = "Lfixture/Refined;->create()Lfixture/Fixed;"
        row = next(row for row in coverage["methods"] if row["identity"] == typed)
        helpers = coverage.get("generated_source_helpers", [])
        added = [r for r in helpers if r["kind"] == "compiler-covariant-interface-bridge"]
        if ({r["identity"] for r in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] != 0 or coverage["projected_method_count"] != 1 or
                coverage["status"] != "partial" or coverage.get("generic_signature_projections") or
                row["projection_kind"] != "interface-covariant-runtime-bridges" or
                row["generated_bridges"] != [bridge] or len(added) != 1 or
                added[0]["bridge_target"] != typed or not row.get("metadata_differences")):
            raise RuntimeError("Nominal result covariance lost its full contract or compiler slot disclosure")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helper_ids = {r["class"] + "->" + r["name"] + r["prototype"] for r in helpers}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(methods) | helper_ids:
            raise RuntimeError("javac added an unreported slot or changed the original inventory")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "NominalInterfaceReturnHarness", "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Added interface slot changed known original class dispatch, identity or exceptions")
        negative = work / "negative"
        shutil.copytree(originals, negative)
        target = negative / "fixture/Fixed.class"
        before = target.read_bytes()
        receipt = change_signature(target, b"Ljava/lang/Object;Lfixture/Result<Lfixture/Tag;>;",
                                   b"Ljava/lang/Object;Lfixture/Result<Lfixture/OtherTag;>;")
        if method_code(before) != method_code(target.read_bytes()):
            raise RuntimeError("Nominal result counterexample changed original Code or descriptors")
        (work / "negative-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = work / "negative-dex"
        negative_dex.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                    "--output", negative_dex, *sorted(negative.rglob("*.class"))], "negative-d8")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex / "classes.dex"),
                                 "--classpath", str(args.android_jar.resolve()), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic inheritance" not in result.stderr:
            raise RuntimeError("Equal result erasure hid different nominal type arguments")
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "Fixed.java":
                content = content.replace("Result<Tag>", "Result<OtherTag>")
            destination = work / "negative-java" / "fixture" / source.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Independent javac accepted incompatible full nominal return arguments")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in originals.rglob("*.class")} or harness_hashes != {
                      p.relative_to(harness).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in harness.rglob("*.class")}:
            raise RuntimeError("Owned original interfaces or concrete implementation Code changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "identity_cases": 24, "ordered_failure_cases": 3, "added_interface_slots": [bridge],
                    "original_inventory_and_concrete_implementation_code": "unchanged",
                    "full_nominal_return_reflection": "matched", "known_class_dispatch": "matched",
                    "nominal_argument_counterexample": "rejected-by-neverd-and-javac",
                    "coverage": "partial", "jvm_output": "matched except reported added interface slot"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
