#!/usr/bin/env python3
"""Verify captured fields and complete generic superclass constructor arguments."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
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
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-captured-generic-constructor-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "captured_generic_constructor/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=25)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("CapturedGenericConstructorCaller", "CapturedGenericConstructorHarness")], harness, classpath=originals, release=25)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 8:
            raise RuntimeError("Owned captured-generic-constructor inventory changed")
        providers = {"Lfixture/Base;", "Lfixture/Sink;", "Lfixture/Tag;"}
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        # This D8 release predates class version 69. Change only that two-byte
        # version field, leaving every compiler instruction, descriptor and
        # attribute byte exact; execute both inputs before recovering anything.
        encoded = work / "encoded"
        for original in sorted(originals.rglob("*.class")):
            data = original.read_bytes()
            if data[:4] != b"\xca\xfe\xba\xbe" or int.from_bytes(data[6:8], "big") != 69:
                raise RuntimeError("Original Java 25 class header changed")
            lowered = data[:6] + (52).to_bytes(2, "big") + data[8:]
            destination = encoded / original.relative_to(originals)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(lowered)
            if lowered[:6] != data[:6] or lowered[8:] != data[8:]:
                raise RuntimeError("D8 compatibility encoding changed more than the version")
        encoded_hashes = hashes(encoded)
        encoded_records = class_identity.compiler_classes(encoded)
        if (set(encoded_records) != set(records) or exception_inventory(encoded) != exceptions
                or any({k: v for k, v in record.items() if k not in {"major", "sha256"}} !=
                       {k: v for k, v in encoded_records[owner].items() if k not in {"major", "sha256"}}
                       for owner, record in records.items())):
            raise RuntimeError("Version compatibility encoding changed metadata")
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (encoded / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            result = []
            for route in range(4):
                for mode in range(9):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "CapturedGenericConstructorHarness", str(route), str(mode)], f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected,line) for line in output.splitlines()):
                        raise RuntimeError("Original constructor behavior matrix changed: " + output)
                    result.append(output)
            return result

        baseline = execute((originals, harness), "baseline")
        if execute((encoded, harness), "encoded") != baseline:
            raise RuntimeError("Version-only D8 input encoding changed original runtime behavior")
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(encoded.rglob("*.class"))
                 if "L" + path.relative_to(encoded).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned erased constructor fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original captured-generic-constructor runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-constructor-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] == "Lfixture/Child;" and name and name[1] == "<init>":
                ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
                capture = next(i for i,(op,ref) in enumerate(ops) if op == "iput-object" and "capture" in ref)
                check = next(i for i,(op,ref) in enumerate(ops) if op.startswith("invoke-static") and "Lfixture/Trace;.step:" in ref)
                delegate = next(i for i,(op,ref) in enumerate(ops) if op.startswith("invoke-direct") and "Lfixture/Base;.<init>:(ILfixture/Sink;J)V" in ref)
                if not capture < check < delegate or "check-cast" in [op for op,_ in ops]:
                    raise RuntimeError("javac/D8 changed the original captured generic constructor witness")
                witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original erased constructor forwarding witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Erased constructor recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != 25:
            raise RuntimeError("Erased constructor unexpectedly raised its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if owner not in {"Lfixture/Sink;", "Lfixture/Tag;"}}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Erased constructor recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original erased constructor class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original erased constructor method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Erased constructor call changed an original effect or result")
        # This separately compiled body chooses the other one-argument overload.
        # The immutable observer must detect the missing original target effects.
        wrong = work / "wrong-overload/fixture/Child.java"
        wrong.parent.mkdir(parents=True)
        text = (fixture / "fixture/Child.java").read_text()
        original_call = "super(2, (Sink<Object>) (Sink<?>) completion, bits);"
        if text.count(original_call) != 1:
            raise RuntimeError("Original constructor witness changed")
        wrong.write_text(text.replace(original_call, "super(2, (Object) completion, bits);"))
        verify.compile_local([wrong], work / "wrong-overload-classes", classpath=originals, release=25)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (work / "wrong-overload-classes", originals, harness))),
                                 "CapturedGenericConstructorHarness", "1", "0"], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-overload-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle did not detect a changed constructor target")
        late = work / "late-capture/fixture/Child.java"
        late.parent.mkdir(parents=True)
        assignment = "        this.capture = captured;\n"
        if text.count(assignment) != 1:
            raise RuntimeError("Original captured field assignment changed")
        late.write_text(text.replace(assignment, "").replace(original_call, original_call + "\n" + assignment.strip()))
        verify.compile_local([late], work / "late-capture-classes", classpath=originals, release=25)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (work / "late-capture-classes", originals, harness))),
                                 "CapturedGenericConstructorHarness", "1", "0"], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "late-capture-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle did not detect delayed capture initialization")
        if (hashes(encoded) != encoded_hashes or hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "original_metadata_and_exceptions": "unchanged", "constructor_identity_and_initialization_order": "matched",
                    "original_exception_identity": "matched", "original_erased_forwarding": "independently witnessed",
                    "d8_version_only_encoding": "all Code and attribute bytes unchanged; both baselines matched",
                    "captured_fields_constant_and_wide_arguments": "matched",
                    "independent_late_capture_counterexample": "detected",
                    "independent_wrong_overload_counterexample": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
