#!/usr/bin/env python3
"""Verify original final-array data payloads and nonexecuted alignment NOPs."""
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-final-array-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "final_array_payload/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness/FinalArrayPayloadHarness.java"], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        classes, methods = compiler_inventory(verify, originals)
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        if len(classes) != 1 or len(methods) != 3:
            raise RuntimeError("Owned final-array inventory changed")
        def execute(root, label):
            output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (root, harness))), "FinalArrayPayloadHarness"], label)
            if output.splitlines() != ["calls=30,failures=12,final-array-identity-and-payload=matched"]:
                raise RuntimeError("Owned final-array observer changed")
            return output
        baseline = execute(originals, "baseline")
        dex = work / "dex"
        dex.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                    "--output", dex, *sorted(originals.rglob("*.class"))], "d8")
        encoded = dex / "classes.dex"
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", encoded], "sdk-inventory")
        inventory = parse_dexdump(dump, encoded.name, dex_header(encoded.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed original final-array inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", encoded], "payload-witness")
        if "fill-array-data" not in disassembly or "nop" not in disassembly:
            raise RuntimeError("Original D8 input lacks data payload and alignment NOP")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", encoded, "--classpath", args.android_jar.resolve(), "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != 1 or coverage["method_count"] != 3
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count", 0)):
            raise RuntimeError("Final-array recovery lost original entries")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helper = {"Lfixture/Core;->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"}
        if actual_classes != classes or set(actual_methods) != set(methods) | expected_helper or helpers != expected_helper:
            raise RuntimeError("Final-array recovery changed runtime inventory")
        actual, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original final-array class metadata changed: " + key)
            for identity, row in before["methods"].items():
                if after["methods"].get(identity) != row or actual_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original final-array method metadata changed")
        if execute(rebuilt, "rebuilt") != baseline:
            raise RuntimeError("Final-array data, aliases or original failures changed")
        wrong = work / "wrong/fixture/Core.java"
        wrong.parent.mkdir(parents=True)
        source = (fixture / "fixture/Core.java").read_text()
        if source.count("{19, -3, 0, 42, 7, 2}") != 1:
            raise RuntimeError("Missing exact owned payload anchor")
        wrong.write_text(source.replace("{19, -3, 0, 42, 7, 2}", "{20, -3, 0, 42, 7, 2}"))
        wrong_classes = work / "wrong-classes"
        verify.compile_local([wrong], wrong_classes, release=8)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong_classes, harness))),
                                 "FinalArrayPayloadHarness"], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "payload-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "original final array payload" not in result.stderr:
            raise RuntimeError("Frozen observer missed changed original payload")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original inputs, frozen observer or SDK changed")
        evidence = {"status": "success", "class_count": 1, "method_count": 3, "runtime_calls": 30,
                    "expected_failures": 12, "metadata_and_runtime_inventory": "matched",
                    "payload_alignment_array_identity_and_aliases": "matched", "payload_counterexample": "detected",
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0
if __name__ == "__main__":
    raise SystemExit(main())
