#!/usr/bin/env python3
"""Verify source interface fields and distinct method-local generic scopes."""
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
    parser.add_argument("--bridge-implementation", action="store_true")
    parser.add_argument("--fixed-implementations", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-method-formal-interface-fields-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "method_formal_interface_fields/java").rglob("*.java"))
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/MethodFormalInterfaceFieldsHarness.java"],
                             harness, classpath=originals)
        if args.bridge_implementation:
            receipt = bridge_flag(originals / "fixture/Receiver.class", "get",
                                  "(Lfixture/Key;)Ljava/lang/Object;", 0x1, 0x41)
            (work / "owned-method-flags.json").write_text(json.dumps(receipt, indent=2) + "\n")
        if args.fixed_implementations:
            receipts = [bridge_flag(originals / "fixture/Receiver.class", name, prototype, 0x1, 0x41)
                        for name, prototype in (("fixed", "(Lfixture/Key;)Ljava/lang/Object;"),
                                                ("plain", "(Ljava/lang/Object;)Ljava/lang/Object;"))]
            (work / "owned-fixed-method-flags.json").write_text(json.dumps(receipts, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)

        def hashes(root: Path) -> dict:
            return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(root.rglob("*.class"))}

        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "MethodFormalInterfaceFieldsHarness",
                               "bridge" if args.bridge_implementation else "ordinary",
                               "fixed" if args.fixed_implementations else "ordinary"], "baseline")
        if baseline.splitlines() != ["values=40,identities=42,failures=10",
                                    "source-interface-fields-and-method-formals=retained,virtual-effects-and-exceptions=matched"]:
            raise RuntimeError("Owned method-formal baseline missed scopes, effects or failures: " + baseline)

        def make_dex(source: Path, output: Path) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", output, *sorted(source.rglob("*.class"))], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned method-formal fixture changed DEX inventory")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes or
                {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original source interface declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        projected = [row for row in coverage["methods"] if row["status"] == "source-projected"]
        projected_count = int(args.bridge_implementation) + 2 * int(args.fixed_implementations)
        expected_status = "partial" if projected_count else "recovered"
        if ({row["identity"] for row in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] or coverage["status"] != expected_status or
                coverage.get("generic_signature_projections") or
                coverage.get("projected_method_count", 0) != projected_count):
            raise RuntimeError("Source interface recovery lost an original field or method scope")
        implementation = "Lfixture/Receiver;->get(Lfixture/Key;)Ljava/lang/Object;"
        projected_ids = {implementation: "method-formal-implementation-bridge"} if args.bridge_implementation else {}
        if args.fixed_implementations:
            projected_ids.update({"Lfixture/Receiver;->" + name + prototype: "full-contract-implementation-bridge"
                                  for name, prototype in (("fixed", "(Lfixture/Key;)Ljava/lang/Object;"),
                                                          ("plain", "(Ljava/lang/Object;)Ljava/lang/Object;"))})
        if ({row["identity"] for row in projected} != set(projected_ids) or
                any(row.get("projection_kind") != projected_ids[row["identity"]] or
                    row.get("source_identity") != row["identity"] or
                    not row.get("inherited_contract") or not row.get("metadata_differences") for row in projected)):
            raise RuntimeError("Original bridge implementation lacks a complete reported inheritance witness")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("javac changed the complete original source interface inventory")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            expected, actual = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "inner_class", "enclosing_method"):
                if actual.get(key) != expected.get(key):
                    raise RuntimeError("Source interface metadata changed: " + owner + ":" + key)
            fields = {identity: dict(field) for identity, field in expected["fields"].items()}
            if owner == "Lfixture/FormalView;":
                token = fields["Lfixture/FormalView;->TOKEN:I"]
                if (token["constant_value"] != {"tag": 3, "value": "0000000b"} or
                        owner + "-><clinit>()V" not in helpers):
                    raise RuntimeError("Interface constant initializer lacks original value and reported inventory")
                token["constant_value"] = None
            if actual["fields"] != fields:
                raise RuntimeError("Original field declarations changed beyond the reported constant initializer")
            for identity, method in expected["methods"].items():
                wanted = dict(method)
                if identity in projected_ids:
                    wanted["access"] &= ~0x40
                if actual["methods"][identity] != wanted:
                    raise RuntimeError("Original method-local generic metadata changed: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "MethodFormalInterfaceFieldsHarness"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Original source interface reflection, virtual calls or exception order changed")
        signature = b"<T:Ljava/lang/Object;>(Lfixture/Key<TT;>;)TT;"
        fixed_signature = b"(Lfixture/Key<*>;)Ljava/lang/Object;"
        for label, original, wrong in (
                ("parameter-scope", signature, signature.replace(b"Key<TT;>", b"Key<TC;>")),
                ("result-scope", signature, signature[:-3] + b"TC;"),
                ("fixed-parameter-scope", fixed_signature, fixed_signature.replace(b"Key<*>", b"Key<TC;>"))):
            negative = work / ("negative-" + label)
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Receiver.class", original, wrong)
            (work / ("negative-" + label + "-signature.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = make_dex(negative, work / ("negative-" + label + "-dex"))
            rejected = work / ("rejected-" + label)
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                     str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / ("negative-" + label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Source method formal was confused with its class formal")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Independently authored original or harness class bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 40, "identity_cases": 42, "ordered_failure_cases": 10,
                    "original_field_declarations_class_and_method_metadata": "unchanged",
                    "constant_value_difference": "original encoded value retained by reported field initializer",
                    "bridge_flag_difference": "reported" if projected_count else "none",
                    "original_and_harness_classes": "unchanged", "signature_only_scope_negatives": "rejected"}
        (work / "oracle-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
