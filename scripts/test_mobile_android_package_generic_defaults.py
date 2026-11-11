#!/usr/bin/env python3
"""Verify exact package static generic default calls and original result-check order."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-package-generic-defaults-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "package_generic_defaults/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("PackageGenericDefaultsHarness", "PackageGenericDefaultsCaller")], harness,
                             classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != 11:
            raise RuntimeError("Owned package generic default inventory changed")
        classes = all_classes
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x") as archive:
            archive.writestr("fixture/Provider.class", (originals / "fixture/Provider.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]

        def execute(runtime, label):
            outputs = []
            for entry in range(3):
                for route in range(4):
                    for mode in range(11):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "PackageGenericDefaultsHarness", str(entry), str(route), str(mode)],
                            f"{label}-{entry}-{route}-{mode}")
                        expected = f"entry={entry},route={route},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Original allocation scalar matrix changed: " + output)
                        outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        dex_dir = work / "dex"
        dex_dir.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex_dir, *paths], "d8-original")
        source_dex = dex_dir / "classes.dex"
        if list(dex_dir.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned package generic default fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original package generic default runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-carrier-disassembly")
        if len(re.findall(r"invoke-static[^\n]*Lfixture/Provider;.pick:\(Ljava/lang/Object;Ljava/lang/String;Ljava/lang/Object;\)Ljava/lang/Object;", disassembly)) != 3:
            raise RuntimeError("D8 changed the three original raw carrier call sites")
        signature = "<T:Ljava/lang/Object;>(Ljava/lang/Object;Ljava/lang/String;TT;)TT;"
        if records["Lfixture/Provider;"]["methods"]["Lfixture/Provider;->pick(Ljava/lang/Object;Ljava/lang/String;Ljava/lang/Object;)Ljava/lang/Object;"].get("signature") != signature:
            raise RuntimeError("Original compiler did not preserve its complete nested bound and original Object seed")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Seeded scalar carrier recovery changed runtime entries or original contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=8)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Seeded scalar carrier recovery invented or omitted a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original package generic default class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after["methods"].get(identity) != metadata or actual_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Original package generic default method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Seeded scalar carrier changed ordered comparisons, mutations or failure identity")
        provider = (fixture / "fixture/Provider.java").read_text()
        for label, mutant, mode in (
            ("missing-default", provider.replace("result = fallback", "result = null"), "1"),
            ("late-default", provider.replace("result = fallback", "result = Trace.fallback"), "1"),
            ("late-result", provider.replace("return (T) result", "return (T) Trace.input"), "0"),
            ("missing-finally", provider.replace("Trace.hit(5);", ""), "0"),
            ("wrong-caught-identity", provider.replace("Trace.last = failure", "Trace.last = new IllegalArgumentException()"), "3"),
            ("early-result-cast", provider.replace("result = Trace.read(input, label)", "result = (String) Trace.read(input, label)"), "10"),
        ):
            if mutant == provider:
                raise RuntimeError("Independent package default counterexample anchor changed")
            wrong = work / label / "fixture/Provider.java"
            wrong.parent.mkdir(parents=True)
            wrong.write_text(mutant)
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                    "PackageGenericDefaultsHarness", "0", "1", mode], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "package generic default contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        try:
            from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
        except ModuleNotFoundError as error:
            if error.name != "scripts": raise
            from test_mobile_android_protected_array_callbacks import change_method_flags
        negative = work / "private-originals"
        shutil.copytree(originals, negative)
        receipt = change_method_flags(negative / "fixture/Provider.class",
            b"(Ljava/lang/Object;Ljava/lang/String;Ljava/lang/Object;)Ljava/lang/Object;", 0x8, 0xa, name=b"pick")
        (work / "private-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        encoded = work / "private-dex"; encoded.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", encoded,
                    *sorted(negative.rglob("*.class"))], "d8-private")
        rejected = work / "private-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(encoded / "classes.dex"),
                                 *map(str, classpath), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "private-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic invocation" not in result.stderr:
            raise RuntimeError("A same-Code foreign-class private generic entry was accepted")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "original_inventory_metadata_and_exceptions": "unchanged", "package_access_formal_default_and_raw_result": "unchanged",
                    "default_snapshot_late_cast_finally_and_failure_identity": "matched", "original_carrier_identity": "matched",
                    "constructor_and_first_repeated_initialization_failures": "matched",
                    "catch_failure_identity_and_monitor_ownership": "matched", "independent_compiler_counterexamples": "all six detected",
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
