#!/usr/bin/env python3
"""Compare original superclass getters with authenticated broad result metadata."""
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
    from scripts.test_mobile_android_final_getter_return_slots import rename_owned_entry
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code
    from test_mobile_android_final_getter_return_slots import rename_owned_entry



def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--integer-getter", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-super-getter-result-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "superclass_getter_results/java").rglob("*.java"))
        inputs += [FIXTURES / "indexed_covariant_returns/java/fixture" / name
                   for name in ("Trace.java",)]
        if args.integer_getter:
            projected = []
            for source in inputs:
                if source.name in ("Adapter.java", "View.java"):
                    replacement = work / "integer-java/fixture" / source.name
                    replacement.parent.mkdir(parents=True, exist_ok=True)
                    content = source.read_text()
                    addition = ("    public int getSize() { return super.size(); }\n"
                                "    public final int size() { return getSize(); }\n" if source.name == "Adapter.java" else
                                "    public int scalar;\n"
                                "    public int getTypedSize() {\n"
                                "        Trace.events = Trace.events * 10 + 2;\n"
                                "        if (Trace.failure != null) throw Trace.failure;\n"
                                "        return scalar;\n    }\n"
                                "    public int originalSuperSize() { return super.getSize(); }\n"
                                "    public static int selectSize(View<?,?> value) { return value.getTypedSize(); }\n")
                    content = content.rstrip()
                    if not content.endswith("}"):
                        raise RuntimeError("Owned fixture lost its class declaration boundary")
                    replacement.write_text(content[:-1] + addition + "}\n")
                    source = replacement
                projected.append(source)
            inputs = projected
        verify.compile_local(inputs, originals)
        receipt = rename_owned_entry(originals / "fixture/View.class", b"getTypedKeys", b"getKeys")
        (work / "owned-entry-name-edit.json").write_text(json.dumps(receipt, indent=2) + "\n")
        if args.integer_getter:
            receipt = rename_owned_entry(originals / "fixture/View.class", b"getTypedSize", b"getSize")
            (work / "owned-integer-entry-name-edit.json").write_text(json.dumps(receipt, indent=2) + "\n")
        entries = [("getKeys", 0x01), ("keySet", 0x11)]
        if args.integer_getter:
            entries += [("getSize", 0x01), ("size", 0x11)]
        for name, flags in entries:
            prototype = "()I" if name in ("getSize", "size") else "()Ljava/util/Set;"
            receipt = bridge_flag(originals / "fixture/Adapter.class", name, prototype,
                                  original_flags=flags, input_flags=flags | 0x40)
            (work / f"owned-{name}-flag-edit.json").write_text(json.dumps(receipt, indent=2) + "\n")
        verify.compile_local([FIXTURES / "harness/SuperclassGetterResultHarness.java"], harness, classpath=originals)
        classes, methods = compiler_inventory(verify, originals)
        def hashes(root: Path) -> dict:
            return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(root.rglob("*.class"))}
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "SuperclassGetterResultHarness", "original",
                               "integer" if args.integer_getter else "reference"], "baseline")
        counts = "values=70,identities=36,failures=13" if args.integer_getter else "values=35,identities=36,failures=6"
        if baseline.splitlines() != [counts,
                                    "original-super-body-and-virtual-getter=matched,broad-result-projection-and-flags=reported"]:
            raise RuntimeError("Owned superclass-result baseline missed cases: " + baseline)
        def make_dex(source: Path, output: Path) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", output, *sorted(source.rglob("*.class"))], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned superclass-result fixture changed DEX inventory")
            return dex
        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes or
                {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original class or distinct final entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        projections = coverage.get("generic_signature_projections", [])
        if ({row["identity"] for row in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] or coverage["status"] != "partial" or
                len(projections) != 1 or any(row.get("source_name") for row in coverage["methods"])):
            raise RuntimeError("Superclass getter recovery lost a declaration or projection receipt")
        row = next(row for row in coverage["methods"] if row["identity"] == "Lfixture/Adapter;->getKeys()Ljava/util/Set;")
        if (row.get("bridge_target") != "Ljava/util/AbstractMap;->keySet()Ljava/util/Set;" or
                row.get("projection_kind") != "generic-superclass-result-forwarding-bridge" or
                row.get("generic_signature") != "()Ljava/util/Set<Ljava/lang/Object;>;" or
                not row.get("metadata_differences")):
            raise RuntimeError("Superclass getter lacks its original full metadata and body receipt")
        if args.integer_getter:
            scalar = next(row for row in coverage["methods"] if row["identity"] == "Lfixture/Adapter;->getSize()I")
            if (scalar.get("projection_kind") != "integer-superclass-forwarding-bridge" or
                    scalar.get("bridge_target") != "Ljava/util/AbstractMap;->size()I" or
                    scalar.get("generic_signature")):
                raise RuntimeError("Integer superclass bridge changed its original plain contract")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        expected_methods = {r.get("source_identity", r["identity"]) for r in coverage["methods"]}
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != expected_methods | helpers:
            raise RuntimeError("javac added an unreported entry or changed original ownership")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "SuperclassGetterResultHarness", "recovered",
                             "integer" if args.integer_getter else "reference"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Distinct final entry changed identity, dispatch or ordered failure effects")
        for label, old, wrong in (
                ("metadata", b"()Ljava/util/Set<Ljava/lang/Object;>;", b"()Ljava/util/Set<Ljava/lang/String;>;"),
                ("witness", b"()Ljava/util/Set<TT;>;", b"()Ljava/util/Set<TU;>;"),
                ("ancestor", b"<T:Ljava/lang/Object;U:Ljava/lang/Object;>Ljava/util/AbstractMap<TT;TU;>;",
                 b"<T:Ljava/lang/Object;U:Ljava/lang/Object;>Ljava/util/AbstractMap<TU;TT;>;")):
            negative = work / ("negative-" + label)
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Adapter.class", old, wrong)
            (work / ("negative-" + label + "-signature.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = make_dex(negative, work / ("negative-" + label + "-dex"))
            rejected = work / ("rejected-" + label)
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                    str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / ("negative-" + label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Superclass getter slot ignored complete generic drift")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Owned input or concrete harness changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 70 if args.integer_getter else 35, "identity_cases": 36,
                    "ordered_failure_cases": 13 if args.integer_getter else 6,
                    "original_integer_super_getter": args.integer_getter,
                    "original_super_call_and_virtual_getter_dispatch": "matched",
                    "broad_result_projection_and_bridge_flags": "reported partial",
                    "original_code_and_input_generic_metadata": "unchanged",
                    "wrong_metadata_witness_and_ancestor_arguments": "rejected",
                    "complete_rebuilt_inventory": "reported source identities and helpers only"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
