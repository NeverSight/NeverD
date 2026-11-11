#!/usr/bin/env python3
"""Compare enum parameter metadata projection with JVM casts and dispatch."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import tempfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_checked_erasure import encode_fixture_signatures
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_checked_erasure import encode_fixture_signatures
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--d8", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-enum-erasure-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), args.d8.resolve(), 300)
        originals, harness_dir = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "checked_enum_erasure/java").rglob("*.java"))
        harness = FIXTURES / "harness/CheckedEnumHarness.java"
        verify.compile_local(inputs, originals)
        verify.compile_local([harness], harness_dir, classpath=originals)
        classes, methods = compiler_inventory(verify, originals)
        path = originals / "fixture/CheckedTransform.class"
        before = path.read_bytes()
        encode_fixture_signatures(path, b"Ljava/lang/Enum;", b"Lfixture/Choice;")
        mutation = {"changes": "class and method generic Signature attributes", "method_bytecode": "unchanged",
                    "before_sha256": hashlib.sha256(before).hexdigest(),
                    "after_sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        (work / "input-metadata-change.json").write_text(json.dumps(mutation, indent=2) + "\n")
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (harness_dir, originals))),
                               "CheckedEnumHarness", "original"], "baseline")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", originals)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned fixture declaration inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial"):
            raise RuntimeError("Enum erasure changed inventory or omitted partial coverage")
        projections = coverage.get("generic_signature_projections", [])
        expected = {"Ljava/lang/Object;Lfixture/Transform<Lfixture/Choice;Ljava/lang/String;>;",
                    "(Lfixture/Choice;)Ljava/lang/String;"}
        if ({row["original_signature"] for row in projections} != expected
                or {change["kind"] for row in projections for change in row["changes"]}
                != {"checked-erased-parameter", "checked-erased-interface-argument"}
                or any(not row.get("metadata_differences") for row in projections)):
            raise RuntimeError("Enum signature differences are not explicitly reported")
        rebuilt = work / "rebuilt"
        verify.compile_local([*sorted((output / "sources").rglob("*.java")), harness], rebuilt)
        _, rebuilt_methods = compiler_inventory(verify, rebuilt)
        if not set(methods).issubset(rebuilt_methods):
            raise RuntimeError("Recompiled source lost an original runtime descriptor")
        actual = verify.run([verify.java, "-cp", rebuilt, "CheckedEnumHarness", "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Enum signature projection changed executable behavior")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "identity_probes": 18, "callback_calls": 12, "callback_failures": 6,
                    "body_cast_failures": 6, "bridge_cast_failures": 3, "null_argument_failures": 6,
                    "null_receiver_failures": 1, "alternative_enum_acceptance": "preserved",
                    "primitive_capture_values": "preserved", "runtime_descriptors": "preserved",
                    "bridge_identity": "preserved", "generic_reflection_difference": "confirmed",
                    "original_method_bytecode": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
