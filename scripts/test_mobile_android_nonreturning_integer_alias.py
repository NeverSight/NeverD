#!/usr/bin/env python3
"""Verify a terminal Void source alias retains integer bridge conversions."""
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
    from scripts.test_mobile_android_private_producer_return_slots import rename_owned_entry
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_private_producer_return_slots import rename_owned_entry


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
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-nonreturning-integer-alias-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = FIXTURES / "nonreturning_integer_alias/java"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/NonReturningIntegerAliasHarness.java"], harness,
                             classpath=originals)
        prototype = "(Lfixture/Scope;Ljava/util/List;I)"
        owner = "Lfixture/Policy;"
        typed, bridge = owner + "->measure" + prototype + "Ljava/lang/Void;", owner + "->measure" + prototype + "I"

        def encode(root: Path) -> list:
            target = root / "fixture/Policy.class"
            return [rename_owned_entry(target, b"terminal", b"measure"),
                    bridge_flag(target, "measure", prototype + "I", 0x1, 0x1041)]

        (work / "owned-input-edits.json").write_text(json.dumps(encode(originals), indent=2) + "\n")
        records = class_identity.compiler_classes(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes
        methods = {key: value for key, value in all_methods.items() if key.split("->", 1)[0] in classes}
        if len(classes) != 4 or len(methods) != 6:
            raise RuntimeError("Owned integer alias declaration inventory changed")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "NonReturningIntegerAliasHarness"], "baseline")
        expected = ["values=10,failures=480,fresh-exceptions=480",
                    "void-descriptor-and-full-contract=retained,integer-override=matched,source-name=reported"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned integer alias baseline missed exceptions or overrides: " + baseline)

        def make_dex(root: Path, output: Path) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", output,
                        *[p for p in sorted(root.rglob("*.class"))
                          if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned integer alias DEX inventory changed")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original integer alias declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes) or
                coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"] or
                coverage["status"] != "partial" or coverage.get("generic_signature_projections") or
                {r["identity"] for r in rows.values() if r["status"] == "source-projected"} != {typed, bridge} or
                rows[typed].get("projection_kind") != "incompatible-inherited-return-slot" or
                rows[bridge].get("projection_kind") != "aliased-return-forwarding-bridge" or
                rows[bridge].get("bridge_target") != typed or
                rows[typed].get("instruction_count") != 6 or rows[bridge].get("instruction_count") != 6 or
                not rows[typed].get("metadata_differences") or not rows[bridge].get("metadata_differences")):
            raise RuntimeError("Integer alias lost its terminal body, conversions or complete contract")
        alias = rows[typed].get("source_name")
        if (not alias or rows[typed].get("source_name_declaration") != typed or
                rows[typed].get("conflicting_inherited_contracts") != ["Lfixture/Api;->measure" + prototype + "I"]):
            raise RuntimeError("Integer alias lacks its original conflicting contract")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage["minimum_java_release"])
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        source_ids = {identity: row.get("source_identity", identity) for identity, row in rows.items()}
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(source_ids.values()) | helpers:
            raise RuntimeError("javac changed inventory beyond the disclosed alias and helpers")
        actual_records = class_identity.compiler_classes(rebuilt)
        for original_owner in classes:
            before, after = records[original_owner], actual_records[original_owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Integer alias changed original metadata: " + original_owner + ":" + key)
            for identity, metadata in before["methods"].items():
                wanted = dict(metadata)
                if identity == typed:
                    wanted["name"] = alias
                elif identity == bridge:
                    wanted["access"] = 0x1
                if after["methods"][source_ids[identity]] != wanted:
                    raise RuntimeError("Method metadata changed beyond the disclosed name and flags: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "NonReturningIntegerAliasHarness", alias], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Original exception freshness, message or integer override changed")

        def reject(root: Path, label: str) -> None:
            negative_dex = make_dex(root, work / ("negative-" + label + "-dex"))
            rejected = work / ("rejected-" + label)
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                     str(args.android_jar.resolve()), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / ("negative-" + label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Integer alias accepted a normal return or full parameter drift")

        negative = work / "negative-scope"
        shutil.copytree(originals, negative)
        receipt = change_signature(negative / "fixture/Policy.class",
                                   b"(Lfixture/Scope;Ljava/util/List<+Lfixture/Item;>;I)Ljava/lang/Void;",
                                   b"(Lfixture/Scope;Ljava/util/List<+Lfixture/Scope;>;I)Ljava/lang/Void;")
        (work / "negative-scope-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        reject(negative, "scope")
        negative_sources = work / "negative-return-java"
        shutil.copytree(fixture, negative_sources)
        source = negative_sources / "fixture/Policy.java"
        text = source.read_text()
        statement = "throw new IllegalStateException(((Object) message).toString());"
        if text.count(statement) != 1:
            raise RuntimeError("Missing exact independently authored terminal statement")
        source.write_text(text.replace(statement, "if (size == 1337) return null; " + statement, 1))
        negative_return = work / "negative-return"
        verify.compile_local(sorted(negative_sources.rglob("*.java")), negative_return)
        (work / "negative-return-input-edits.json").write_text(json.dumps(encode(negative_return), indent=2) + "\n")
        reject(negative_return, "return")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original input or immutable harness bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 10, "failure_cases": 480, "fresh_exception_cases": 480,
                    "original_void_descriptor_and_full_parameter_metadata": "retained",
                    "integer_bridge_conversion_operations": "retained including Number cast and intValue",
                    "external_integer_override": "matched without harness recompilation",
                    "source_name_and_binary_abi_differences": "reported partial",
                    "complete_rebuilt_inventory": "reported source identities and helpers only",
                    "normal_return_and_scope_counterexamples": "rejected",
                    "input_contract_and_harness_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
