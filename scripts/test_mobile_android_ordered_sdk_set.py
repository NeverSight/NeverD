#!/usr/bin/env python3
"""Verify external ordered-set defaults across every covariant return bridge."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-ordered-sdk-set-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = FIXTURES / "ordered_sdk_set/java"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        # The original program stays Java 8. This immutable caller exercises
        # the JDK 21 covariant default slots also present in Android API 35.
        verify.compile_local([FIXTURES / "harness/OrderedSdkSetHarness.java"], harness,
                             classpath=originals, release=21)
        owner = "Lfixture/OrderedBag;"
        bridges = {owner + "->getSize()I", owner + "->size()I"}

        def encode(root: Path) -> list:
            target = root / "fixture/OrderedBag.class"
            return [bridge_flag(target, "getSize", "()I", 0x1, 0x41),
                    bridge_flag(target, "size", "()I", 0x11, 0x51)]

        (work / "owned-input-flags.json").write_text(json.dumps(encode(originals), indent=2) + "\n")
        records = class_identity.compiler_classes(originals)
        classes, methods = compiler_inventory(verify, originals)
        if classes != {owner} or len(methods) != 3:
            raise RuntimeError("Owned ordered-set declaration inventory changed")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "OrderedSdkSetHarness", "original"], "baseline")
        lines = baseline.splitlines()
        cases = re.fullmatch(r"values=(\d+),identities=(\d+),failures=(\d+)", lines[0]) if lines else None
        if (not cases or int(cases[1]) != 660 or int(cases[2]) != 1495 or int(cases[3]) != 7 or
                lines[1:] != ["ordered-sdk-state-and-comparator-effects=matched,full-superclass-contract-and-original-defaults=retained"]):
            raise RuntimeError("Owned ordered-set baseline missed mutable-state or default cases: " + baseline)

        def make_dex(root: Path, output: Path) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", output, *sorted(root.rglob("*.class"))], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned ordered-set DEX inventory changed")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original ordered-set declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != set(methods) or coverage["class_count"] != 1 or coverage["method_count"] != 3 or
                coverage["unrecovered_method_count"] or coverage["status"] != "partial" or
                coverage.get("generic_signature_projections") or
                {r["identity"] for r in rows.values() if r["status"] == "source-projected"} != bridges or
                rows[owner + "->getSize()I"].get("projection_kind") != "integer-superclass-forwarding-bridge" or
                any(not rows[key].get("metadata_differences") for key in bridges)):
            raise RuntimeError("Ordered-set recovery lost its exact superclass or forwarding receipts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("javac changed inventory beyond reported helpers")
        actual_records = class_identity.compiler_classes(rebuilt)
        before, after = records[owner], actual_records[owner]
        for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
            if after.get(key) != before.get(key):
                raise RuntimeError("Ordered-set original class metadata changed: " + key)
        for identity, metadata in before["methods"].items():
            wanted = dict(metadata)
            if identity in bridges:
                wanted["access"] &= ~0x40
            if after["methods"][identity] != wanted:
                raise RuntimeError("Ordered-set method metadata changed beyond bridge flags: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "OrderedSdkSetHarness", "rebuilt"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Mutable state, comparator effects, object identity or covariant defaults changed")

        def reject(root: Path, label: str) -> None:
            negative_dex = make_dex(root, work / ("negative-" + label + "-dex"))
            rejected = work / ("rejected-" + label)
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                     str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / ("negative-" + label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Ordered-set proof accepted changed inherited arguments or forwarding effects")

        negative = work / "negative-scope"
        shutil.copytree(originals, negative)
        receipt = change_signature(negative / "fixture/OrderedBag.class",
                                   b"<E:Ljava/lang/Object;>Ljava/util/TreeSet<TE;>;",
                                   b"<E:Ljava/lang/Object;>Ljava/util/TreeSet<Ljava/lang/String;>;")
        (work / "negative-scope-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        reject(negative, "scope")
        negative_sources = work / "negative-effect-java"
        shutil.copytree(fixture, negative_sources)
        target = negative_sources / "fixture/OrderedBag.java"
        text = target.read_text()
        statement = "return super.size();"
        if text.count(statement) != 1:
            raise RuntimeError("Missing exact independently authored superclass getter")
        target.write_text(text.replace(statement, "return super.size() + 1;", 1))
        negative_effect = work / "negative-effect"
        verify.compile_local(sorted(negative_sources.rglob("*.java")), negative_effect)
        (work / "negative-effect-flags.json").write_text(json.dumps(encode(negative_effect), indent=2) + "\n")
        reject(negative_effect, "effect")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or
                hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original input, SDK declarations or immutable harness bytes changed")
        evidence = {"status": "success", "class_count": 1, "method_count": 3,
                    "value_cases": int(cases[1]), "identity_cases": int(cases[2]), "failure_cases": 7,
                    "external_covariant_default_return_slots": 4,
                    "sdk_superclass_state_comparator_effects_and_exception_identity": "matched",
                    "full_metadata_and_complete_inventory": "unchanged except reported bridge flags and helpers",
                    "scope_and_effect_counterexamples": "rejected",
                    "input_sdk_and_immutable_harness": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
