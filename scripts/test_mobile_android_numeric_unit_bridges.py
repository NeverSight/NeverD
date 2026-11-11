#!/usr/bin/env python3
"""Compare owned numeric conversions, original void targets and Unit read order."""
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
    from scripts.test_mobile_android_void_unit_bridges import encode_void_bridge
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_void_unit_bridges import encode_void_bridge


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
    parser.add_argument("--profile", choices=("float", "double", "long"), default="float")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-numeric-unit-bridges-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        wrapper, descriptor = {"float": ("Float", "F"), "double": ("Double", "D"),
                               "long": ("Long", "J")}[args.profile]
        conversion = args.profile + "Value"
        inputs = sorted((FIXTURES / (args.profile + "_unit_bridges/java")).rglob("*.java"))
        inputs.append(FIXTURES / "void_unit_bridges/java/kotlin/Unit.java")
        verify.compile_local(inputs, originals)
        harness_name = wrapper + "UnitBridgeHarness"
        verify.compile_local([FIXTURES / ("harness/" + harness_name + ".java")], harness, classpath=originals)
        interface = "Lfixture/Function<Lfixture/Marker;Ljava/lang/" + wrapper + ";Lkotlin/Unit;>;"
        signature = "Ljava/lang/Object;" + interface
        encoding = encode_void_bridge(originals / "fixture/Receiver.class", signature.encode(), rename_target=False)
        (work / "owned-input-encoding.json").write_text(json.dumps(encoding, indent=2) + "\n")
        original_records = class_identity.compiler_classes(originals)
        classes, methods = compiler_inventory(verify, originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_name, "original"], "baseline")
        if baseline.splitlines() != ["values=213,identities=94,failures=12",
                                     "numeric-conversion-and-Unit-initialization-order=matched,reflection-differences=measured"]:
            raise RuntimeError("Owned numeric Unit baseline missed cases: " + baseline)

        def make_dex(root: Path, output: Path) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", output, *sorted(root.rglob("*.class"))], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned numeric Unit changed DEX inventory")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes
                or {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original numeric Unit declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["projected_method_count"] != 1
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial"):
            raise RuntimeError("Numeric Unit recovery lost bodies or conservative coverage")
        prototype = "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"
        bridge = "Lfixture/Receiver;->invoke" + prototype
        projections = coverage.get("generic_signature_projections", [])
        if (len(projections) != 1 or projections[0].get("declaration") != "Lfixture/Receiver;"
                or projections[0].get("original_signature") != signature
                or not projections[0].get("metadata_differences") or len(projections[0]["changes"]) != 1):
            raise RuntimeError("Numeric Unit projected an unproven original contract")
        change = projections[0]["changes"][0]
        if (change["kind"] != "checked-value-carrier-interface" or change["scope_origin"] != bridge
                or change["original_type"] != interface or change["source_type"] != "Lfixture/Function;"):
            raise RuntimeError("Numeric Unit lost its complete original interface edge")
        row = next(r for r in coverage["methods"] if r["identity"] == bridge)
        target = "Lfixture/Receiver;->invoke(Lfixture/Marker;" + descriptor + ")V"
        if (row["projection_kind"] != "value-carrier-interface-bridge"
                or row["source_identity"] != bridge or not row.get("metadata_differences")
                or row["bridge_target"] != target or row["bridge_dispatch"] != "Lfixture/Function;->invoke" + prototype
                or row.get("unbox_methods") != ["Ljava/lang/Number;->" + conversion + "()" + descriptor]
                or row.get("result_field") != "Lkotlin/Unit;->INSTANCE:Lkotlin/Unit;" or "box_method" in row):
            raise RuntimeError("Numeric Unit lost its original conversion, target or singleton read")
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
                wanted = None if owner == "Lfixture/Receiver;" and key == "signature" else before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Numeric Unit changed unreported metadata: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                wanted = dict(metadata)
                if identity == bridge:
                    wanted["access"] = 0x1
                if after["methods"][identity] != wanted:
                    raise RuntimeError("Numeric Unit changed unreported method metadata: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             harness_name, "rebuilt"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Numeric Unit changed values, identities, effects or initialization order")
        negative = work / "negative-input"
        shutil.copytree(originals, negative)
        wrong = signature.replace("Lfixture/Marker;", "Ljava/lang/Object;")
        receipt = change_signature(negative / "fixture/Receiver.class", signature.encode(), wrong.encode())
        (work / "negative-parent-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = make_dex(negative, work / "negative-dex")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Equal erased parent hid a different complete reference input")
        counter = work / "compiler-counters/fixture"
        counter.mkdir(parents=True)
        shared = []
        for source in inputs:
            if source.name in ("Function.java", "Marker.java"):
                path = counter / source.name
                path.write_text(source.read_text())
                shared.append(path)
        actor = counter / "CounterActor.java"
        original = ("package fixture; public final class CounterActor implements Function<Marker," + wrapper
                    + ",Object> { public Object invoke(Marker marker, " + wrapper + " number) { return null; } }\n")
        actor.write_text(original)
        verify.compile_local([*shared, actor], work / "positive-compiler")
        actor.write_text(original.replace("Function<Marker,", "Function<Object,", 1))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *map(str, [*shared, actor])],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Independent javac accepted the changed complete reference input")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original input or immutable JVM harness bytes changed")
        evidence = {"status": "success", "profile": args.profile, "class_count": len(classes),
                    "method_count": len(methods), "value_cases": 213, "identity_cases": 94,
                    "ordered_failure_cases": 12, "Number_virtual_conversion": "called once with original effects and sentinel identity",
                    "primitive_values": "raw float/double bits or long values matched",
                    "typed_void_calls": "no conversion or Unit initialization",
                    "Unit_initialization": "after first successful target, once only",
                    "original_metadata_and_complete_inventory": "unchanged except reported interface edge, bridge flags and helpers",
                    "parent_counterexample": "same erasure and unchanged Code, rejected by NeverD and javac",
                    "original_and_harness_bytes": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
