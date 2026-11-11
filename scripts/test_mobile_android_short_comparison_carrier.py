#!/usr/bin/env python3
"""Compare original short, float or long carrier conversions and scalar dispatch."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag


CLASS_PREFIX = b"Lfixture/Root<Lfixture/Carrier;>;"
SOURCE_SIGNATURE = CLASS_PREFIX + b"Ljava/lang/Comparable;"
INPUT_SIGNATURE = CLASS_PREFIX + b"Ljava/lang/Comparable<Lfixture/Carrier;>;"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    profiles = parser.add_mutually_exclusive_group()
    profiles.add_argument("--float", action="store_true")
    profiles.add_argument("--long", action="store_true", dest="wide_integer")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-short-comparison-carrier-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        profile = "long" if args.wide_integer else "float" if args.float else "short"
        primitive = "J" if args.wide_integer else "F" if args.float else "S"
        fixture = FIXTURES / (profile + "_comparison_carrier/java")
        inputs = sorted(fixture.rglob("*.java"))
        if args.float or args.wide_integer:
            inputs.append(FIXTURES / "short_comparison_carrier/java/fixture/Root.java")
        harness_name = "LongComparisonCarrierHarness" if args.wide_integer else "FloatComparisonCarrierHarness" if args.float else "ShortComparisonCarrierHarness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / ("harness/" + harness_name + ".java")], harness,
                             classpath=originals)
        target = originals / "fixture/Carrier.class"
        edits = [change_signature(target, SOURCE_SIGNATURE, INPUT_SIGNATURE),
                 bridge_flag(target, "compareTo", "(Ljava/lang/Object;)I", 0x1, 0x1041)]
        (work / "owned-input-edits.json").write_text(json.dumps(edits, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)

        def hashes(root: Path) -> dict:
            return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(root.rglob("*.class"))}

        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_name], "baseline")
        value_cases, identity_cases = (720, 1567) if args.wide_integer else (3946, 809) if args.float else (218, 75)
        failure_cases = 52 if args.wide_integer else 8
        state_cases = 5916 if args.wide_integer else None
        expected = [f"values={value_cases},identities={identity_cases},failures={failure_cases}",
                    profile + "-comparison-conversion-and-dispatch=matched,raw-source-contract-and-flags=reported"]
        if args.wide_integer:
            expected = [f"values={value_cases},identities={identity_cases},states={state_cases},failures={failure_cases}",
                        "long-comparison-both-words-aliases-and-effects=matched,raw-source-contract-and-flags=reported"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned scalar comparison baseline missed effect or identity cases: " + baseline)

        def make_dex(source: Path, output: Path) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", output, *sorted(source.rglob("*.class"))], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned scalar fixture changed DEX inventory")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original scalar declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        identity = "Lfixture/Carrier;->compareTo(Ljava/lang/Object;)I"
        projected = {r["identity"]: r for r in coverage["methods"] if r["status"] == "source-projected"}
        if ({r["identity"] for r in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] or coverage["status"] != "partial" or
                set(projected) != {identity} or coverage.get("class_source_bindings")):
            raise RuntimeError("Scalar recovery lost an original entry or widened a source scope")
        row = projected[identity]
        if (row.get("projection_kind") != "value-carrier-interface-bridge" or
                row.get("bridge_target") != "Lfixture/Carrier;->compareScalar(" + primitive + ")I" or
                row.get("unbox_methods") != ["Lfixture/Carrier;->read()" + primitive] or
                row.get("box_method") or not row.get("metadata_differences")):
            raise RuntimeError("Scalar conversion or original target lost its exact witness")
        projections = coverage.get("generic_signature_projections", [])
        if (len(projections) != 1 or projections[0].get("declaration") != "Lfixture/Carrier;" or
                projections[0].get("original_signature") != INPUT_SIGNATURE.decode() or
                projections[0].get("changes") != [{"kind": "checked-value-carrier-interface",
                    "offset": len(CLASS_PREFIX), "original_type": "Ljava/lang/Comparable<Lfixture/Carrier;>;",
                    "source_type": "Ljava/lang/Comparable;", "scope_origin": identity}]):
            raise RuntimeError("Original scalar source contract change is not explicitly reported")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("javac changed the complete scalar inventory")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                wanted = SOURCE_SIGNATURE.decode() if owner == "Lfixture/Carrier;" and key == "signature" else before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Scalar class/field metadata changed beyond raw contract: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if method == identity:
                    wanted["access"] = 0x1
                if after["methods"][method] != wanted:
                    raise RuntimeError("Scalar method metadata changed beyond the bridge flag: " + method)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             harness_name, "raw"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Scalar conversion, comparison effects or original exception order changed")
        negative = work / "negative-scope"
        shutil.copytree(originals, negative)
        receipt = change_signature(negative / "fixture/Carrier.class", INPUT_SIGNATURE,
                                   CLASS_PREFIX + b"Ljava/lang/Comparable<Ljava/lang/String;>;")
        (work / "negative-scope-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = make_dex(negative, work / "negative-scope-dex")
        rejected = work / "rejected-scope"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-scope-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Scalar proof accepted a different complete input contract")
        if args.wide_integer:
            for label, forwarding in (("changed-wide-input", "return compareScalar(unboxed + 1L);"),
                                      ("extra-conversion", "checked.read(); return compareScalar(unboxed);")):
                authored, negative = work / (label + "-java"), work / label
                shutil.copytree(fixture, authored)
                source = authored / "fixture/Carrier.java"
                text = source.read_text()
                assert text.count("return compareScalar(unboxed);") == 1
                source.write_text(text.replace("return compareScalar(unboxed);", forwarding))
                verify.compile_local([*sorted(authored.rglob("*.java")), inputs[-1]], negative)
                target = negative / "fixture/Carrier.class"
                receipts = [change_signature(target, SOURCE_SIGNATURE, INPUT_SIGNATURE),
                            bridge_flag(target, "compareTo", "(Ljava/lang/Object;)I", 0x1, 0x1041)]
                (work / (label + "-encoding.json")).write_text(json.dumps(receipts, indent=2) + "\n")
                negative_dex = make_dex(negative, work / (label + "-dex"))
                rejected = work / (label + "-rejected")
                result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                         str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                        capture_output=True, timeout=300, env=verify.environment)
                (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
                if result.returncode == 0 or rejected.exists():
                    raise RuntimeError("Wide comparison accepted changed input or an extra conversion: " + label)
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original independently authored input or immutable harness changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": value_cases, "identity_cases": identity_cases, "state_cases": state_cases, "ordered_failure_cases": failure_cases,
                    "scalar_profile": profile,
                    "scalar_extremes": "signed long extremes, distinct high/low words and aliases matched" if args.wide_integer else "NaN payloads, signed zero, subnormals and infinities matched" if args.float else "signed short extremes matched",
                    "conversion_and_target_effects": "matched",
                    "raw_source_contract_and_bridge_flags": "reported", "original_body_and_callers": "retained",
                    "complete_class_field_and_method_metadata": "checked", "scope_counterexample": "rejected",
                    "original_and_harness_bytes": "unchanged"}
        (work / "oracle-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
