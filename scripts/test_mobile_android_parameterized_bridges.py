#!/usr/bin/env python3
"""Verify complete generic member substitution, bridge casts and erased counterexamples."""
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
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-parameterized-bridges-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "parameterized_bridges/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness_dir = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        classes, methods = compiler_inventory(verify, originals)
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        harness = FIXTURES / "harness/ParameterizedBridgeHarness.java"
        verify.compile_local([harness], harness_dir, classpath=originals)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               "ParameterizedBridgeHarness"], "baseline")
        if baseline.splitlines() != ["identities=40,failures=22,bridges=2",
                                     "full-generic-reflection=retained,mutable-effects=matched,cast-order=matched"]:
            raise RuntimeError("Owned parameterized baseline did not exercise all expected cases")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", args.android_jar.resolve())
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned parameterized bridge inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["projected_method_count"] != 2
                or coverage["unrecovered_method_count"] != 0 or coverage.get("generic_signature_projections")
                or coverage["status"] != "partial"):
            raise RuntimeError("Parameterized bridge changed inventory or generic source declarations")
        for row in coverage["methods"]:
            if row["status"] == "source-projected" and (
                    row["projection_kind"] != "generic-interface-bridge"
                    or row["bridge_target"] not in methods or row["bridge_dispatch"] not in methods):
                raise RuntimeError("Parameterized bridge lost its original checked delegation witness")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if actual_classes != classes or not set(methods).issubset(actual_methods):
            raise RuntimeError("javac did not regenerate every original bridge and member descriptor")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir))),
                             "ParameterizedBridgeHarness"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Parameterized source changed generic reflection, dispatch or ordered effects")
        negative = work / "negative-originals"
        shutil.copytree(originals, negative)
        target = negative / "fixture/Receiver.class"
        before = target.read_bytes()
        signature = b"(Lfixture/Box<-Lfixture/Value;>;Lfixture/Box<+Lfixture/Signal;>;)Ljava/lang/Object;"
        changed_signature = signature.replace(b"fixture/Value", b"fixture/Signal", 1)
        old = b"\x01" + len(signature).to_bytes(2, "big") + signature
        new = b"\x01" + len(changed_signature).to_bytes(2, "big") + changed_signature
        if before.count(old) != 1:
            raise RuntimeError("Missing exact owned target parameter Signature")
        changed = before.replace(old, new, 1)
        if method_code(before) != method_code(changed):
            raise RuntimeError("Erased counterexample changed original Code bytes or method descriptors")
        target.write_bytes(changed)
        (work / "negative-signature.json").write_text(json.dumps(
            {"original_signature": signature.decode(), "changed_signature": changed_signature.decode(),
             "code_and_descriptors": "unchanged", "changed_sha256": hashlib.sha256(changed).hexdigest()},
            indent=2) + "\n")
        negative_dex = verify.dex(sorted(negative.rglob("*.class")), work / "negative-dex",
                                  args.android_jar.resolve())
        rejected = work / "rejected"
        command = [str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                   str(args.android_jar.resolve()), "-o", str(rejected)]
        result = subprocess.run(command, text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if (result.returncode == 0 or rejected.exists()
                or "generic target parameter differs from the full inherited substitution" not in result.stderr):
            raise RuntimeError("Erasure hid a changed generic target parameter")
        negative_sources = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "Receiver.java":
                declaration = "public Object invoke(Box<? super Value> first, Box<? extends Signal> second)"
                if content.count(declaration) != 1:
                    raise RuntimeError("Missing owned target source declaration")
                content = content.replace(declaration, declaration.replace("super Value", "super Signal"), 1)
            destination = negative_sources / source.relative_to(fixture)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or "does not override" not in compiler.stderr:
            raise RuntimeError("Independent javac did not reject the mismatched inherited contract")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted(originals.rglob("*.class"))}:
            raise RuntimeError("Original owned parameterized input bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "bridge_count": 2, "identity_cases": 40, "ordered_failure_cases": 22,
                    "lower_upper_nested_and_array_types": "retained", "generic_reflection": "unchanged",
                    "mutable_reads_writes_and_virtual_override": "matched", "original_descriptors": "retained",
                    "erased_counterexample": "rejected-by-neverd-and-javac", "original_class_sha256": hashes,
                    "coverage": "partial", "jvm_output": "matched"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
