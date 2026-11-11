#!/usr/bin/env python3
"""Verify generic void targets behind checked Unit bridges and explicit erased wrappers."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
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
    from scripts.test_mobile_android_void_unit_bridges import encode_void_bridge
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_static_defaults import method_code
    from test_mobile_android_void_unit_bridges import encode_void_bridge


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
        prefix="neverd-parameterized-unit-bridges-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness_dir = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "parameterized_unit_bridges/java").rglob("*.java"))
        inputs.extend([FIXTURES / "void_unit_bridges/java/fixture/Trace.java",
                       FIXTURES / "void_unit_bridges/java/kotlin/Unit.java"])
        verify.compile_local(inputs, originals)
        encoding = encode_void_bridge(originals / "fixture/Worker.class",
                                      b"Ljava/lang/Object;Lfixture/Action<"
                                      b"Lfixture/Callback<+Ljava/lang/Boolean;>;Lkotlin/Unit;>;")
        (work / "owned-input-encoding.json").write_text(json.dumps(encoding, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        verify.compile_local([FIXTURES / "harness/ParameterizedUnitBridgeHarness.java"], harness_dir,
                             classpath=originals)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               "ParameterizedUnitBridgeHarness", "invoke"], "baseline")
        if baseline.splitlines() != ["identities=20,failures=12,bridge=1,ordered-initialization=matched",
                                     "generic-void-target=retained,reflection=original"]:
            raise RuntimeError("Owned generic Unit baseline missed an expected identity or ordered failure")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", args.android_jar.resolve())
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the independently authored generic Unit inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["projected_method_count"] != 2
                or coverage["unrecovered_method_count"] != 0 or coverage.get("generic_signature_projections")
                or coverage["status"] != "partial"):
            raise RuntimeError("Generic Unit bridge lost inventory or original generic declarations")
        source_name = None
        expected = set(methods)
        for row in coverage["methods"]:
            if row["status"] != "source-projected":
                continue
            if (row["projection_kind"] not in ("void-unit-target", "void-unit-interface-bridge")
                    or not row.get("metadata_differences") or not row.get("source_identity")):
                raise RuntimeError("Generic Unit source mapping omitted its metadata differences")
            if row["projection_kind"] == "void-unit-target":
                source_name = row["bridge_source_name"]
                expected.remove(row["identity"])
                expected.add(row["source_identity"])
                if row.get("generic_signature") != "(Lfixture/Callback<Ljava/lang/Boolean;>;)V":
                    raise RuntimeError("Original generic void parameter Signature was lost")
            else:
                expected.add(row["source_identity"])
                if (row["bridge_target"] not in methods or row["bridge_dispatch"] not in methods
                        or row["result_field"] != "Lkotlin/Unit;->INSTANCE:Lkotlin/Unit;"
                        or not any("runtime erasures" in item for item in row["metadata_differences"])):
                    raise RuntimeError("Generic Unit wrapper did not report its exact witness and parameter erasure")
        if not source_name:
            raise RuntimeError("Missing shared generic void target name")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if actual_classes != classes or not expected.issubset(actual_methods):
            raise RuntimeError("Recompiled Unit source lost original descriptors after the reported rename")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir))),
                             "ParameterizedUnitBridgeHarness", source_name], "recovered-execution")
        if actual.splitlines() != [baseline.splitlines()[0],
                                   "generic-void-target=retained,reflection=reported-erased-wrapper"]:
            raise RuntimeError("Unit wrapper changed casts, callback effects, exception order or reported reflection")
        negative = work / "negative-originals"
        shutil.copytree(originals, negative)
        target = negative / "fixture/Worker.class"
        before = target.read_bytes()
        signature = b"(Lfixture/Callback<Ljava/lang/Boolean;>;)V"
        changed_signature = signature.replace(b"java/lang/Boolean", b"java/lang/Integer")
        old = b"\x01" + len(signature).to_bytes(2, "big") + signature
        new = b"\x01" + len(changed_signature).to_bytes(2, "big") + changed_signature
        if before.count(old) != 1:
            raise RuntimeError("Missing unique owned generic void Signature")
        changed = before.replace(old, new, 1)
        if method_code(before) != method_code(changed):
            raise RuntimeError("Generic Unit counterexample changed method Code bytes or descriptors")
        target.write_bytes(changed)
        (work / "negative-signature.json").write_text(json.dumps(
            {"original_signature": signature.decode(), "changed_signature": changed_signature.decode(),
             "code_and_descriptors": "unchanged"}, indent=2) + "\n")
        negative_dex = verify.dex(sorted(negative.rglob("*.class")), work / "negative-dex",
                                  args.android_jar.resolve())
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if (result.returncode == 0 or rejected.exists()
                or "generic target parameter differs from the full inherited substitution" not in result.stderr):
            raise RuntimeError("Runtime erasure hid an incompatible generic void parameter")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "bridge_count": 1, "identity_cases": 20, "ordered_failure_cases": 12,
                    "code_and_input_descriptors": "unchanged owned encoding", "generic_void_target": "retained",
                    "wrapper_parameter_erasure_and_method_rename": "reported-and-confirmed",
                    "casts_callback_effects_exception_identity_and_unit_initialization": "matched",
                    "incompatible_generic_counterexample": "rejected", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
