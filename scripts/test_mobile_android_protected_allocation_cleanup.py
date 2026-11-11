#!/usr/bin/env python3
"""Verify protected allocations with catch bodies, conditional cleanup and finally."""
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-allocation-cleanup-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "protected_allocation_cleanup/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("ProtectedAllocationCleanupHarness", "ProtectedAllocationCleanupCaller")], harness,
                             classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != 8:
            raise RuntimeError("Owned protected allocation cleanup inventory changed")
        classes = all_classes - ({"Lfixture/Problem;"} if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x") as archive:
            archive.writestr("fixture/Problem.class", (originals / "fixture/Problem.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            outputs = []
            for clean in range(2):
                for route in range(4):
                    for mode in range(10):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "ProtectedAllocationCleanupHarness", str(clean), str(route), str(mode)],
                            f"{label}-{clean}-{route}-{mode}")
                        expected = f"clean={'true' if clean else 'false'},route={route},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Original protected cleanup matrix changed: " + output)
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
            raise RuntimeError("Owned cleanup fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original protected cleanup runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-cleanup-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Maker;" or not name or name[1] != "create":
                continue
            ops = [(int(pc, 16), op, tail) for pc, op, tail in
                   re.findall(r"\|([0-9a-fA-F]+):\s+(\S+)([^\n]*)", block[2])]
            allocation = [pc for pc, op, _ in ops if op == "new-instance"]
            constructor = [pc for pc, op, _ in ops if op.startswith("invoke-direct")]
            record = [pc for pc, op, tail in ops if op.startswith("invoke-static") and ".record:" in tail]
            catches = re.search(r"(?ms)^\s+catches\s+: (\d+)\n(.*?)(?=^\s+positions\s+:)", block[2])
            intervals = [] if not catches else [(int(a, 16), int(b, 16)) for a, b in
                re.findall(r"(?m)^\s+0x([0-9a-fA-F]+) - 0x([0-9a-fA-F]+)$", catches[2])]
            if (len(allocation) != 1 or len(constructor) != 1 or len(record) != 1 or len(intervals) < 2
                    or not any(allocation[0] < pc < constructor[0] and op.startswith("const-string") for pc, op, _ in ops)
                    or not any(begin <= allocation[0] < constructor[0] < end for begin, end in intervals)
                    or not any(begin <= record[0] < end for begin, end in intervals)
                    or not any(op.startswith("if-") for _, op, _ in ops)
                    or sum(op == "move-exception" for _, op, _ in ops) < 2):
                raise RuntimeError("D8 changed the protected allocation and protected cleanup witness")
            witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original protected cleanup witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Protected cleanup recovery changed runtime entries or original contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=8, classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Protected cleanup recovery invented or omitted a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original cleanup class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after["methods"].get(identity) != metadata or actual_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Original cleanup method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Protected cleanup changed values, ordered effects or failure identity")
        text = (fixture / "fixture/Maker.java").read_text()
        moved = text.replace("        try {", '        Problem early = new Problem("owned protected cleanup");\n        try {', 1)
        moved = moved.replace('return new Problem("owned protected cleanup");', "return early;")
        for label, mutant, mode in (
            ("missing-finally-effect", text.replace("Trace.cleanup(2);", ""), "0"),
            ("wrong-caught-identity", text.replace("Trace.record(failure, input)", "Trace.record(new IllegalArgumentException(), input)"), "1"),
            ("allocation-outside-handler", moved, "1"),
            ("inverted-cleanup-branch", text.replace("if (clean)", "if (!clean)"), "0"),
        ):
            if mutant == text:
                raise RuntimeError("Independent cleanup counterexample anchor changed")
            wrong = work / label / "fixture/Maker.java"
            wrong.parent.mkdir(parents=True)
            wrong.write_text(mutant)
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                    "ProtectedAllocationCleanupHarness", "1", "1", mode], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "protected allocation cleanup contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_provider": args.dependency_provider, "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "original_inventory_metadata_and_exceptions": "unchanged", "conditional_finally_and_nested_handlers": "matched",
                    "constructor_and_first_repeated_initialization_failures": "matched",
                    "handler_and_cleanup_failure_identity": "matched", "independent_compiler_counterexamples": "all four detected",
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
