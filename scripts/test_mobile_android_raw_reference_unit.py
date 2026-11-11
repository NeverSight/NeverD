#!/usr/bin/env python3
"""Verify raw reference callback bridges retain their original void and Unit entries."""
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
import zipfile
try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_protected_array_callbacks import change_method_flags

PROVIDERS = {"Lfixture/Api;", "Lfixture/Marker;", "Lfixture/Context;", "Lkotlin/Unit;"}
BRIDGE = "Lfixture/Core;->invoke(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"
TARGET = "Lfixture/Core;->invoke(Ljava/lang/Throwable;Ljava/lang/Void;Lfixture/Context;)V"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--integer-tail", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-raw-reference-unit-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "raw_reference_unit/java"
        mixed = FIXTURES / "mixed_raw_unit"
        target = "Lfixture/Core;->invoke(Ljava/lang/Void;Lfixture/Context;I)V" if args.integer_tail else TARGET
        originals, harness = work / "originals", work / "harness"
        inputs = sorted(fixture.rglob("*.java"))
        observers = sorted((fixture.parent / "harness").glob("*.java"))
        if args.integer_tail:
            inputs = [p for p in inputs if p.name not in ("Core.java", "Trace.java")]
            inputs += sorted((mixed / "java").rglob("*.java"))
            observers = sorted((mixed / "harness").glob("*.java"))
        verify.compile_local(inputs, originals)
        verify.compile_local(observers, harness, classpath=originals)
        descriptor = BRIDGE.split("->invoke", 1)[1].encode()
        receipt = change_method_flags(originals / "fixture/Core.class", descriptor, 0x1, 0x1041, name=b"invoke")
        (work / "owned-bridge-flags.json").write_text(json.dumps(receipt, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 6 or len(all_methods) != 9:
            raise RuntimeError("Owned raw reference Unit inventory changed")
        classes = all_classes - (PROVIDERS if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(PROVIDERS):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label, flags):
            result = []
            for route in range(6):
                modes = range(18) if args.integer_tail else range(12) if route != 5 else (0, 4, 5, 6, 7, 8, 9)
                for mode in modes:
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "RawReferenceUnitHarness", str(route), str(mode), flags], f"{label}-{route}-{mode}")
                    pattern = f"route={route},mode={mode},attempt=[01],trace=[0-9]+,failure=(none|[A-Za-z]+)"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, row) for row in output.splitlines()):
                        raise RuntimeError("Original raw Unit oracle matrix changed: " + output)
                    result.append(output)
            return result

        baseline = execute((originals, harness), "baseline", "original")

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [p for p in sorted(root.rglob("*.class")) if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned raw Unit fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original raw Unit entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        if args.integer_tail:
            disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-mixed-bridge-disassembly")
            witnesses = 0
            for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
                if block[1] != "Lfixture/Core;" or not re.search(r"(?m)^\s+type\s+: '\(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;\)Ljava/lang/Object;'", block[2]):
                    continue
                ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
                semantic = [op for op in ops if op != "move-object"]
                if semantic != ["check-cast", "check-cast", "check-cast", "invoke-virtual", "move-result", "invoke-virtual", "sget-object", "return-object"] or "Ljava/lang/Number;.intValue:()I" not in block[2]:
                    raise RuntimeError("Original raw casts, Number conversion or Unit bridge Code changed")
                witnesses += 1
            if witnesses != 1:
                raise RuntimeError("Missing original mixed raw Unit bridge witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count", 0) != 1
                or projected != {BRIDGE} or coverage["status"] != "partial"
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")):
            raise RuntimeError("Raw Unit recovery changed original entries, ancestry or target names")
        row = rows[BRIDGE]
        if (row.get("projection_kind") != "raw-reference-unit-bridge" or row.get("source_identity") != BRIDGE
                or row.get("bridge_target") != target or not row.get("metadata_differences")
                or row.get("result_field") != "Lkotlin/Unit;->INSTANCE:Lkotlin/Unit;"):
            raise RuntimeError("Raw Unit bridge receipt was not reported exactly")
        if args.integer_tail and (row.get("unboxed_parameters") != [2] or row.get("unbox_method") != "Ljava/lang/Number;->intValue()I"):
            raise RuntimeError("Mixed raw Unit bridge omitted its unchanged conversion receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner[:-1] + ";->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes - {"Lfixture/Api;", "Lfixture/Marker;"}}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Raw Unit source invented a runtime entry or helper")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original raw Unit class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                wanted = dict(metadata)
                if identity == BRIDGE:
                    wanted["access"] &= ~0x1040
                if after["methods"][identity] != wanted or actual_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original raw Unit method metadata changed beyond reported flags: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt", "source") != baseline:
            raise RuntimeError("Raw Unit casts, dispatch, identities or initialization changed")
        negative = work / "missing-synthetic-originals"
        shutil.copytree(originals, negative)
        receipt = change_method_flags(negative / "fixture/Core.class", descriptor, 0x1041, 0x41, name=b"invoke")
        (work / "missing-synthetic-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        rejected = work / "missing-synthetic-rejected"
        negative_dex = dex(negative, work / "missing-synthetic-dex")
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "missing-synthetic-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists():
            raise RuntimeError("Incomplete raw Unit bridge flags were accepted")
        invocation = "invoke((Throwable) first, (Void) second, (Context) third);"
        counterexamples = (
            ("missing-first-cast", invocation, "invoke((Throwable) null, (Void) second, (Context) third);", 0),
            ("reordered-casts", invocation, "Void checked = (Void) second; invoke((Throwable) first, checked, (Context) third);", 10),
            ("early-singleton", invocation, "Object early = kotlin.Unit.INSTANCE; " + invocation, 7),
            ("missing-singleton", "return kotlin.Unit.INSTANCE;", "return null;", 0),
            ("repeated-target", invocation, invocation + " " + invocation, 0),
            ("different-receiver", invocation, "new Core()." + invocation, 0),
        )
        if args.integer_tail:
            invocation = "invoke((Void) first, (Context) second, ((Number) third).intValue());"
            counterexamples = (
                ("missing-first-cast", invocation, "invoke((Void) null, (Context) second, ((Number) third).intValue());", 1),
                ("reordered-casts", invocation, "Context checked = (Context) second; invoke((Void) first, checked, ((Number) third).intValue());", 10),
                ("early-unbox", invocation, "int early = ((Number) third).intValue(); invoke((Void) first, (Context) second, early);", 2),
                ("narrowed-number", invocation, "invoke((Void) first, (Context) second, ((Integer) third).intValue());", 0),
                ("early-singleton", invocation, "Object early = kotlin.Unit.INSTANCE; " + invocation, 7),
                ("missing-singleton", "return kotlin.Unit.INSTANCE;", "return null;", 0),
                ("repeated-target", invocation, invocation + " " + invocation, 0),
                ("different-receiver", invocation, "new Core()." + invocation, 0),
            )
        for label, before, after, mode in counterexamples:
            wrong = work / label / "fixture/Core.java"
            wrong.parent.mkdir(parents=True)
            text = ((mixed / "java") if args.integer_tail else fixture).joinpath("fixture/Core.java").read_text()
            if text.count(before) != 1:
                raise RuntimeError("Independent raw Unit counterexample anchor changed")
            wrong.write_text(text.replace(before, after))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                "RawReferenceUnitHarness", "0", str(mode), "source"], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "raw reference Unit contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "integer_tail": args.integer_tail,
                    "original_metadata_and_exceptions": "reported bridge/synthetic flags only", "original_raw_ancestry_and_both_descriptors": "unchanged",
                    "cast_precedence_receiver_and_argument_identity": "matched", "partial_effects_and_body_failure_identity": "matched",
                    "typed_void_and_first_repeated_singleton_initialization": "matched", "independent_counterexamples": len(counterexamples),
                    "same_Code_incomplete_bridge_flags": "rejected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
