#!/usr/bin/env python3
"""Run recovered source-owned default, static and private interface bodies."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_nested_return import change_signature


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--raw-parent", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-source-interface-defaults-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness_dir = work / "originals", work / "harness"
        fixture = "source_raw_interface_parent" if args.raw_parent else "source_interface_defaults"
        inputs = sorted((FIXTURES / (fixture + "/java")).rglob("*.java"))
        inputs.append(FIXTURES / "value_carrier_bridges/java/fixture/Trace.java")
        if args.raw_parent:
            inputs.append(FIXTURES / "generic_result_value_carrier_bridges/java/fixture/Box.java")
        verify.compile_local(inputs, originals, release=9)
        harness = "SourceRawInterfaceParentHarness" if args.raw_parent else "SourceInterfaceDefaultHarness"
        verify.compile_local([FIXTURES / ("harness/" + harness + ".java")], harness_dir,
                             classpath=originals, release=9)
        classes, methods = compiler_inventory(verify, originals)
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               harness], "baseline")
        expected = (["identities=27,failures=6",
                     "raw-interface-and-source-parent=matched,full-generic-reflection=unchanged"]
                    if args.raw_parent else ["values=3,identities=27,failures=6",
                                            "default-static-private-dispatch=matched,generic-reflection=unchanged"])
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned source-default baseline missed expected cases")

        def make_dex(directory: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "28", "--classpath",
                        args.android_jar.resolve(), "--output", destination,
                        *sorted(directory.rglob("*.class"))], "d8-" + destination.name)
            dex = destination / "classes.dex"
            if not dex.is_file() or list(destination.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Source default fixture did not retain one original DEX")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed source-default class or method identities")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage.get("projected_method_count", 0) != 0 or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "recovered" or coverage.get("generic_signature_projections")
                or coverage["minimum_java_release"] != 9):
            raise RuntimeError("Source default recovery lost bodies, metadata or required Java release")
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        helper_owner = "Leaf" if args.raw_parent else "Root"
        if not any(key.startswith("Lfixture/" + helper_owner + ";->__neverdThrow") for key in helpers):
            raise RuntimeError("Private interface exception helper was not disclosed")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage["minimum_java_release"])
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if (actual_classes != classes or not set(methods).issubset(actual_methods)
                or set(actual_methods) - set(methods) != helpers):
            raise RuntimeError("Source default recompile changed ownership or added an unreported method")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir))),
                             harness], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Source interface bodies changed effects, failures, dispatch or reflection")
        negative = work / "negative-originals"
        for source in originals.rglob("*.class"):
            destination = negative / source.relative_to(originals)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(source.read_bytes())
        if args.raw_parent:
            receipt = change_signature(negative / "fixture/Leaf.class",
                                       b"<R:Ljava/lang/Object;>(TR;)TR;",
                                       b"<R:Ljava/lang/Object;>(Ljava/lang/Object;)Ljava/lang/Object;")
        else:
            receipt = change_signature(negative / "fixture/Root.class", b"(TT;)TT;",
                                       b"(Ljava/lang/String;)Ljava/lang/String;")
        (work / "negative-metadata-change.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = make_dex(negative, work / "negative-dex")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Default Signature with wrong runtime erasure was accepted")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted(originals.rglob("*.class"))}:
            raise RuntimeError("Owned source-default input changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 3, "identity_cases": 27, "ordered_failure_cases": 6,
                    "original_method_ownership": "unchanged", "generic_reflection": "unchanged",
                    "default_and_static_dispatch": "matched", "private_interface_dispatch": "matched",
                    "generated_helpers": "reported private methods only", "minimum_java_release": 9,
                    "metadata_counterexample": "rejected", "coverage": "recovered", "jvm_output": "matched"}
        if args.raw_parent:
            evidence.pop("value_cases")
            evidence.pop("private_interface_dispatch")
            evidence["method_formals_and_parameterized_result"] = "complete scopes and arguments retained"
            evidence["abstract_generic_parent_and_class_precedence"] = "matched"
            evidence["same_erasure_metadata_counterexample"] = "rejected"
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
