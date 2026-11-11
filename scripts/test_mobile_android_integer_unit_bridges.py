#!/usr/bin/env python3
"""Keep exact integer/boolean bridges, original void bodies and Unit read order."""
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
    from scripts.test_mobile_android_void_unit_bridges import encode_void_bridge
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_void_unit_bridges import encode_void_bridge
    from test_mobile_android_nested_return import change_signature


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--boolean", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-integer-unit-bridges-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        profile = "boolean" if args.boolean else "integer"
        inputs = sorted((FIXTURES / (profile + "_unit_bridges/java")).rglob("*.java"))
        inputs.append(FIXTURES / "void_unit_bridges/java/kotlin/Unit.java")
        originals, harness_dir = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        harness_name = "BooleanUnitBridgeHarness" if args.boolean else "IntegerUnitBridgeHarness"
        harness = FIXTURES / ("harness/" + harness_name + ".java")
        verify.compile_local([harness], harness_dir, classpath=originals)
        if args.boolean:
            original_signature = (b"<K:Ljava/lang/Object;V:Ljava/lang/Object;>"
                                  b"Ljava/lang/Object;Lfixture/Function;")
            input_signature = original_signature[:-1] + b"<Ljava/lang/Boolean;TK;TV;TV;Lkotlin/Unit;>;"
            encoding = encode_void_bridge(originals / "fixture/Receiver.class", input_signature,
                                          rename_target=False, original_signature=original_signature)
        else:
            encoding = encode_void_bridge(
                originals / "fixture/Receiver.class",
                b"Ljava/lang/Object;Lfixture/Function<Lfixture/Marker;Ljava/lang/Integer;Lkotlin/Unit;>;",
                rename_target=False)
        (work / "owned-input-encoding.json").write_text(json.dumps(encoding, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               harness_name, "original"], "baseline")
        expected = "identities=20,failures=7,boolean-dispatch=matched" if args.boolean else "identities=23,failures=10,number-dispatch=matched"
        if baseline.splitlines() != [expected, "reflection=original"]:
            raise RuntimeError("Owned integer bridge baseline did not exercise all expected cases")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", args.android_jar.resolve())
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed original integer bridge class or method identities")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["projected_method_count"] != 1
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"):
            raise RuntimeError("Integer bridge dropped bodies or changed conservative coverage")
        bridge = "Lfixture/Receiver;->invoke(" + "Ljava/lang/Object;" * (4 if args.boolean else 2) + ")Ljava/lang/Object;"
        projections = coverage.get("generic_signature_projections", [])
        if (len(projections) != 1 or not projections[0].get("metadata_differences")
                or len(projections[0]["changes"]) != 1
                or projections[0]["changes"][0]["kind"] != "checked-" + profile + "-unit-interface"
                or projections[0]["changes"][0]["scope_origin"] != bridge
                or projections[0]["changes"][0]["source_type"] != "Lfixture/Function;"):
            raise RuntimeError("Raw integer interface projection has no original bridge witness")
        if args.boolean and (projections[0].get("original_signature") != input_signature.decode()
                             or projections[0]["changes"][0].get("original_type") !=
                             "Lfixture/Function<Ljava/lang/Boolean;TK;TV;TV;Lkotlin/Unit;>;"):
            raise RuntimeError("Original JVM metadata was replaced with an internal type comparison key")
        row = next(row for row in coverage["methods"] if row["identity"] == bridge)
        if (row["projection_kind"] != profile + "-unit-interface-bridge" or row["source_identity"] != bridge
                or not row.get("metadata_differences") or row["bridge_target"] not in methods
                or row["bridge_dispatch"] not in methods):
            raise RuntimeError("Integer bridge did not retain its exact original runtime identities")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if actual_classes != classes or not set(methods).issubset(actual_methods):
            raise RuntimeError("Rebuilt integer source lost an original class, name or method descriptor")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir))),
                             harness_name, "recovered"], "recovered-execution")
        before, after = baseline.splitlines(), actual.splitlines()
        if (before[:-1] != after[:-1] or before[-1] != "reflection=original"
                or after[-1] != "reflection=reported-raw-interface"):
            raise RuntimeError("Integer source changed unboxing, effects, Unit initialization or reported reflection")
        if args.boolean:
            negative = work / "negative-originals"
            for source in originals.rglob("*.class"):
                destination = negative / source.relative_to(originals)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(source.read_bytes())
            receipt = change_signature(negative / "fixture/Receiver.class", b"(ZTK;TV;TV;)V", b"(ZTK;TV;TK;)V")
            (work / "negative-metadata-change.json").write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = verify.dex(sorted(negative.rglob("*.class")), work / "negative-dex", args.android_jar.resolve())
            rejected = work / "rejected"
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                     str(args.android_jar.resolve()), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic inheritance" not in result.stderr:
                raise RuntimeError("Primitive target same-erasure formal drift was accepted")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted(originals.rglob("*.class"))}:
            raise RuntimeError("Original owned integer bridge input bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "identity_cases": 23, "ordered_failure_cases": 10, "original_code_and_descriptors": "unchanged",
                    "typed_void_calls": "no unboxing or Unit read", "number_intValue_virtual_dispatch": "matched once",
                    "non_Integer_numbers_and_integer_edges": "matched", "Unit_initialization_after_success": "matched",
                    "raw_interface_and_bridge_flag_reflection": "confirmed", "original_names": "retained",
                    "coverage": "partial", "jvm_output": "matched except reported reflection"}
        if args.boolean:
            evidence.update(identity_cases=20, ordered_failure_cases=7,
                            boolean_booleanValue_virtual_dispatch="matched once",
                            true_false_and_null_edges="matched", typed_generic_parameter_reflection="unchanged",
                            same_erasure_counterexample="rejected")
            evidence.pop("number_intValue_virtual_dispatch")
            evidence.pop("non_Integer_numbers_and_integer_edges")
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
