#!/usr/bin/env python3
"""Verify proven final-class parameter wildcard projections with a JVM oracle."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-final-parameter-variance-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "final_parameter_variance/java").rglob("*.java"))
        inputs += [FIXTURES / "standard_float_reference_bridges/java/fixture/Function.java",
                   FIXTURES / "generic_result_value_carrier_bridges/java/fixture/Box.java",
                   FIXTURES / "value_carrier_bridges/java/fixture/Trace.java"]
        verify.compile_local(inputs, originals)
        harness_name = "FinalParameterVarianceHarness"
        verify.compile_local([FIXTURES / ("harness/" + harness_name + ".java")], harness,
                             classpath=originals)
        receiver = originals / "fixture/Receiver.class"
        old = b"<S:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Function;"
        new = (b"<S:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Function<"
               b"Lfixture/Mapper<-Lfixture/Input;+Lfixture/Value;>;"
               b"Lfixture/Box<-Ljava/lang/Boolean;>;Ljava/lang/Object;>;")
        edits = {"signature": change_signature(receiver, old, new),
                 "flags": bridge_flag(receiver, "invoke", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
                                      original_flags=0x1, input_flags=0x1041)}
        (work / "owned-input-encoding.json").write_text(json.dumps(edits, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_name, "original"], "baseline")
        if baseline.splitlines() != ["identities=99,failures=6", "reflection=original"]:
            raise RuntimeError("Owned final parameter baseline missed expected cases")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", args.android_jar.resolve())
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed original final parameter identities")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        target = "Lfixture/Receiver;->invoke(Lfixture/Mapper;Lfixture/Box;)Ljava/lang/Object;"
        rows = coverage.get("generic_signature_projections", [])
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["projected_method_count"] != 1 or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial" or len(rows) != 1
                or rows[0].get("declaration") != target or not rows[0].get("metadata_differences")):
            raise RuntimeError("Final parameter recovery lost original inventory or metadata disclosure")
        changes = rows[0]["changes"]
        if (len(changes) != 1 or changes[0]["kind"] != "inherited-final-parameter-variance"
                or changes[0]["scope_origin"] !=
                    "Lfixture/Function;->invoke(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"
                or "+Lfixture/Value;" not in changes[0]["source_type"]):
            raise RuntimeError("Final parameter projection lost its complete original witness")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if (rebuilt_classes != classes or not set(methods).issubset(rebuilt_methods)
                or set(rebuilt_methods) - set(methods) != helpers):
            raise RuntimeError("Recompiled final parameter changed method ownership")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             harness_name, "recovered"], "recovered-execution")
        if actual.splitlines() != [baseline.splitlines()[0], "reflection=reported-final-parameter"]:
            raise RuntimeError("Final parameter source changed identity, dispatch, effects or failures")
        negative = work / "negative-originals"
        for path in originals.rglob("*.class"):
            destination = negative / path.relative_to(originals)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(path.read_bytes())
        signature = (b"(Lfixture/Mapper<-Lfixture/Input;Lfixture/Value;>;"
                     b"Lfixture/Box<-Ljava/lang/Boolean;>;)Ljava/lang/Object;")
        receipt = change_signature(negative / "fixture/Receiver.class", signature,
                                   signature.replace(b"Lfixture/Value;", b"Lfixture/Input;"))
        (work / "negative-metadata-change.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = verify.dex(sorted(negative.rglob("*.class")), work / "negative-dex",
                                  args.android_jar.resolve())
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic inheritance" not in result.stderr:
            raise RuntimeError("Different final bound with identical erasure and Code was accepted")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted(originals.rglob("*.class"))}:
            raise RuntimeError("Owned final parameter originals changed during verification")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "identity_cases": 99, "ordered_failure_cases": 6,
                    "original_code_and_descriptors": "unchanged", "source_variance": "reported",
                    "bridge_flags_and_dispatch": "retained", "metadata_counterexample": "rejected",
                    "input_field_result_identity_and_effects": "matched", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
