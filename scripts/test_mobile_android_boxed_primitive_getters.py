#!/usr/bin/env python3
"""Verify retained primitive field getters and original static boxing bridges."""
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-boxed-primitive-getters-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = FIXTURES / "boxed_primitive_getters/java"
        inputs = sorted(fixture.rglob("*.java"))
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/BoxedPrimitiveGettersHarness.java"], harness,
                             classpath=originals)
        flags = [bridge_flag(originals / "fixture/BoxedGetterOwner.class", name,
                             "()Ljava/lang/Object;", 0x1, 0x1041)
                 for name in ("getFloat", "getDouble")]
        (work / "owned-bridge-flags.json").write_text(json.dumps(flags, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)

        def hashes(root: Path) -> dict:
            return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(root.rglob("*.class"))}

        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "BoxedPrimitiveGettersHarness"], "baseline")
        expected = ["values=242,identities=176,failures=8",
                    "primitive-width-bits-boxing-effects-and-failure-order=matched,original-bridge-flags=measured"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned boxed primitive getter missed independent cases: " + baseline)

        def make_dex(source: Path, output: Path) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", output, *sorted(source.rglob("*.class"))], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned boxed primitive getter changed DEX inventory")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original boxed primitive getter declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] or coverage["status"] != "partial" or
                coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")):
            raise RuntimeError("Boxed primitive getter recovery lost an original entry or scope")
        bridges = {"Lfixture/BoxedGetterOwner;->get" + kind + "()Ljava/lang/Object;":
                   ("Lfixture/BoxedGetterOwner;->read" + kind + "()" + primitive,
                    "Lfixture/Getter" + kind + "Box;->box(" + primitive + ")Lfixture/Getter" + kind + "Box;",
                    "Lfixture/BoxedGetterApi;->get" + kind + "()Ljava/lang/Object;")
                   for kind, primitive in (("Float", "F"), ("Double", "D"))}
        projected = [r for r in coverage["methods"] if r["status"] == "source-projected"]
        if ({r["identity"] for r in projected} != set(bridges) or
                coverage.get("projected_method_count") != 2):
            raise RuntimeError("Boxed getter recovery changed an unrelated original entry")
        for row in projected:
            target, box, inherited = bridges[row["identity"]]
            if (row.get("projection_kind") != "boxed-primitive-getter-bridge" or
                    row.get("bridge_target") != target or row.get("box_method") != box or
                    row.get("inherited_class_contract") != inherited or not row.get("metadata_differences")):
                raise RuntimeError("Boxed getter lost its target, original box call or inherited witness")
        for owner in ("BoxedGetterApi", "BoxedGetterOwner"):
            if records["Lfixture/" + owner + ";"].get("signature"):
                raise RuntimeError("Owned nongeneric boxed getter acquired a class Signature")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("javac changed inventory beyond the reported helpers")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original boxed primitive getter class/field metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                wanted = dict(metadata)
                if identity in bridges:
                    wanted["access"] &= ~0x1040
                if after["methods"][identity] != wanted:
                    raise RuntimeError("Original boxed getter metadata changed beyond measured bridge flags: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "BoxedPrimitiveGettersHarness", "rebuilt"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Original boxing dispatch, primitive bits, allocation or failure order changed")
        negative = work / "negative-input"
        shutil.copytree(originals, negative)
        signature = b"<R:Ljava/lang/Object;S:Ljava/lang/Object;>(TR;TS;)TR;"
        receipt = change_signature(negative / "fixture/BoxedGetterOwner.class", signature,
                                   signature.replace(b"(TR;TS;)", b"(TS;TS;)"))
        (work / "negative-parameter-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = make_dex(negative, work / "negative-input-dex")
        rejected = work / "rejected-input"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-input-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Equal erased parameter hid a different complete method-formal contract")
        negative_sources = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "BoxedGetterOwner.java":
                original = "R echo(R input, S tag) { return input; }"
                changed = "R echo(S input, S tag) { return (R) input; }"
                if content.count(original) != 1:
                    raise RuntimeError("Missing exact independently authored method-formal counterexample")
                content = content.replace(original, changed)
            destination = negative_sources / "fixture" / source.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Independent Java compiler accepted the changed complete method-formal scope")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original authored input or immutable JVM harness bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 242, "identity_cases": 176, "ordered_failure_cases": 8,
                    "complete_primitive_width_field_and_original_box_calls": "retained",
                    "direct_and_interface_dispatch": "matched", "raw_bits_allocation_effects_and_failure_identity": "matched",
                    "absent_original_class_signatures": "unchanged",
                    "original_metadata_and_complete_inventory": "unchanged except reported bridge flags and helpers",
                    "parameter_counterexample": "same erasure and unchanged Code, rejected by NeverD and javac",
                    "original_and_harness_bytes": "unchanged", "coverage": "partial"}
        (work / "oracle-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
