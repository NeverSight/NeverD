#!/usr/bin/env python3
"""Compare owned inherited closed Integer carriers with recovered JVM bodies."""
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
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def inherited_edge(path: Path) -> dict:
    before = path.read_bytes()
    old, new = b"fixture/IntegerFunction", b"fixture/FixedIntegerFunction"
    token = b"\x01" + len(old).to_bytes(2, "big") + old
    replacement = b"\x01" + len(new).to_bytes(2, "big") + new
    if before.count(token) != 1:
        raise RuntimeError("Missing exact owned direct interface name")
    after = before.replace(token, replacement, 1)
    original, changed = class_records(before), class_records(after)
    wanted = class_identity.ClassFile(before).facts()
    if wanted["interfaces"] != ["Lfixture/IntegerFunction;"] or wanted["signature"] is not None:
        raise RuntimeError("Owned carrier has an unexpected original class contract")
    wanted["interfaces"] = ["Lfixture/FixedIntegerFunction;"]
    if (method_code(before) != method_code(after)
            or before[original["pool_end"]:] != after[changed["pool_end"]:]
            or class_identity.ClassFile(after).facts() != wanted):
        raise RuntimeError("Owned interface edit changed Code, flags, descriptors or other metadata")
    path.write_bytes(after)
    return {"original_interface": old.decode(), "input_interface": new.decode(),
            "code_descriptors_flags_and_signatures": "unchanged",
            "before_sha256": hashlib.sha256(before).hexdigest(),
            "after_sha256": hashlib.sha256(after).hexdigest()}


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
    parser.add_argument("--ranged", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-inherited-integer-carriers-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = "inherited_ranged_integer_carriers" if args.ranged else "inherited_integer_carriers"
        arity = 13 if args.ranged else 2
        scalar_count = 2 if args.ranged else 1
        prefix_count = arity - scalar_count - 1
        counts = (3482, 8923, 18) if args.ranged else (350, 333, 12)
        harness_name = "InheritedIntegerRangeCarriersHarness" if args.ranged else "InheritedIntegerCarriersHarness"
        inputs = sorted((FIXTURES / (fixture + "/java")).rglob("*.java"))
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / ("harness/" + harness_name + ".java")],
                             harness, classpath=originals)
        prototype = "(" + "Ljava/lang/Object;" * arity + ")Ljava/lang/Object;"
        bridge = "Lfixture/IntegerActor;->invoke" + prototype
        receipt = {"interface": inherited_edge(originals / "fixture/IntegerActor.class"),
                   "flags": bridge_flag(originals / "fixture/IntegerActor.class", "invoke", prototype,
                                        original_flags=0x1, input_flags=0x1041)}
        (work / "owned-input-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        original_records = class_identity.compiler_classes(originals)
        classes, methods = compiler_inventory(verify, originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_name, "original"], "baseline")
        expected = ["values=%d,identities=%d,failures=%d" % counts,
                    "inherited-integer-carrier-scope=measured,conversion-dispatch-identity-and-failure-order=matched"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned inherited carrier baseline missed cases: " + baseline)

        def make_dex(root: Path, output: Path) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", output, *sorted(root.rglob("*.class"))], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned inherited carrier changed DEX inventory")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes
                or {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original inherited carrier declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["projected_method_count"] != 1
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial"):
            raise RuntimeError("Inherited carrier recovery lost bodies or conservative coverage")
        interface = ("Lfixture/IntegerFunction<" + "Ljava/lang/Object;" * prefix_count
                     + "Lfixture/IntegerTag;" + "Ljava/lang/Integer;" * scalar_count + "Ljava/lang/Object;>;")
        signature = "Ljava/lang/Object;" + interface
        projections = coverage.get("generic_signature_projections", [])
        if (len(projections) != 1 or projections[0].get("declaration") != "Lfixture/FixedIntegerFunction;"
                or projections[0].get("original_signature") != signature
                or not projections[0].get("metadata_differences") or len(projections[0]["changes"]) != 1):
            raise RuntimeError("Inherited carrier projected an unproven class contract")
        change = projections[0]["changes"][0]
        if (change["kind"] != "checked-inherited-value-carrier-interface"
                or change["scope_origin"] != bridge or change["original_type"] != interface
                or change["source_type"] != "Lfixture/IntegerFunction;"):
            raise RuntimeError("Inherited carrier lost its complete original parent edge")
        row = next(r for r in coverage["methods"] if r["identity"] == bridge)
        if (row["projection_kind"] != "value-carrier-interface-bridge"
                or row["source_identity"] != bridge or not row.get("metadata_differences")
                or row["bridge_target"] != ("Lfixture/IntegerActor;->invoke(" + "Ljava/lang/Object;" * prefix_count
                                            + "Lfixture/IntegerTag;" + "I" * scalar_count + ")Ljava/lang/Object;")
                or row["bridge_dispatch"] != "Lfixture/IntegerFunction;->invoke" + prototype
                or row.get("source_interface_owner") != "Lfixture/FixedIntegerFunction;"
                or row.get("unbox_methods") != ["Ljava/lang/Number;->intValue()I"] * scalar_count or "box_method" in row):
            raise RuntimeError("Inherited carrier lost conversion, dispatch or source ownership")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("javac changed inventory beyond the reported helpers")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            before, after = original_records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                wanted = None if owner == "Lfixture/FixedIntegerFunction;" and key == "signature" else before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Inherited carrier changed unreported metadata: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                wanted = dict(metadata)
                if identity == bridge:
                    wanted["access"] = 0x1
                if after["methods"][identity] != wanted:
                    raise RuntimeError("Inherited carrier changed unreported method metadata: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             harness_name, "rebuilt"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Inherited carrier changed values, identities, effects or ordered failures")
        negative = work / "negative-input"
        shutil.copytree(originals, negative)
        wrong = signature.replace("Lfixture/IntegerTag;", "Ljava/lang/Integer;")
        receipt = change_signature(negative / "fixture/FixedIntegerFunction.class",
                                   signature.encode(), wrong.encode())
        (work / "negative-parent-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = make_dex(negative, work / "negative-dex")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Equal erased parent hid a different complete input contract")
        counter = work / "compiler-counters/fixture"
        counter.mkdir(parents=True)
        shared = []
        for source in inputs:
            if source.name in ("IntegerFunction.java", "IntegerTag.java"):
                path = counter / source.name
                path.write_text(source.read_text())
                shared.append(path)
        actor = counter / "CounterActor.java"
        counter_parameters = ["Object p%d" % i for i in range(prefix_count)] + ["IntegerTag tag"]
        counter_parameters += ["Integer number%d" % i for i in range(scalar_count)]
        actor.write_text("package fixture; public final class CounterActor implements FixedIntegerFunction {"
                         "public Object invoke(" + ", ".join(counter_parameters) + ") { return null; } }\n")
        parent = counter / "FixedIntegerFunction.java"
        original_parent = (FIXTURES / (fixture + "/java/fixture/FixedIntegerFunction.java")).read_text()
        parent.write_text(original_parent)
        verify.compile_local([*shared, parent, actor], work / "positive-compiler")
        old = "IntegerTag"
        if original_parent.count(old) != 1:
            raise RuntimeError("Missing exact independently authored parent counterexample")
        parent.write_text(original_parent.replace(old, "Integer"))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *map(str, [*shared, parent, actor])],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Independent javac accepted the changed complete parent input")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original authored input or immutable JVM harness bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": counts[0], "identity_cases": counts[1], "ordered_failure_cases": counts[2],
                    "range_call": args.ranged,
                    "original_signature_free_owner": "unchanged",
                    "original_complete_parent_scope": "retained for validation, raw source edge reported partial",
                    "Number_intValue_virtual_override": "called once with original effects and sentinel identity",
                    "converted_and_target_arguments": "matched, including null and non-Integer numbers",
                    "original_metadata_and_complete_inventory": "unchanged except reported parent edge, bridge flags and helpers",
                    "parent_counterexample": "same erasure and unchanged Code, rejected by NeverD and javac",
                    "original_and_harness_bytes": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
