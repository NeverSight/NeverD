#!/usr/bin/env python3
"""Verify constant and register preparation inside protected allocations."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
import re
from pathlib import Path
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

PROVIDERS = {"Lfixture/Problem;"}


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-protected-allocation-preparations-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "protected_allocation_preparations/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("ProtectedPreparationHarness", "ProtectedPreparationCaller")], harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != 8:
            raise RuntimeError("Owned protected allocation preparation inventory changed")
        classes = all_classes - (PROVIDERS if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)

        def provider_jar(root: Path, target: Path) -> None:
            with zipfile.ZipFile(target, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                for owner in sorted(PROVIDERS):
                    name = owner[1:-1] + ".class"
                    archive.writestr(name, (root / name).read_bytes())

        dependency = work / "owned-provider.jar"
        provider_jar(originals, dependency)
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]
        def execute(runtime, label):
            result = []
            for variant in range(2):
                for route in range(4):
                    for mode in range(5):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "ProtectedPreparationHarness", str(variant), str(route), str(mode)],
                            f"{label}-{variant}-{route}-{mode}")
                        expected = f"variant={variant},route={route},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Original protected preparation matrix changed: " + output)
                        result.append(output)
            return result

        baseline = execute((originals, harness), "baseline")

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned protected-preparation fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original protected-preparation runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-protected-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Maker;" or not name or name[1] not in {"create", "raised"}:
                continue
            ops = [(int(pc,16),op) for pc,op in re.findall(r"\|([0-9a-fA-F]+):\s+(\S+)", block[2])]
            allocations = [pc for pc,op in ops if op == "new-instance"]
            constructors = [pc for pc,op in ops if op.startswith("invoke-direct")]
            if len(allocations) != 1 or len(constructors) != 1:
                raise RuntimeError("D8 changed the original allocation/initializer inventory")
            allocation, constructor = allocations[0], constructors[0]
            preparation = [op for pc,op in ops if allocation < pc < constructor]
            catches = re.search(r"(?ms)^\s+catches\s+: (\d+)\n(.*?)(?=^\s+positions\s+:)", block[2])
            if not catches:
                raise RuntimeError("Missing original protected preparation catch table")
            intervals = [(int(a,16),int(b,16)) for a,b in re.findall(r"(?m)^\s+0x([0-9a-fA-F]+) - 0x([0-9a-fA-F]+)$", catches[2])]
            if (not any(op.startswith("const-string") for op in preparation)
                    or any(op.startswith("invoke-") for op in preparation)
                    or len(intervals) != 1 or not intervals[0][0] <= allocation < constructor < intervals[0][1]
                    or "<any> ->" not in catches[2]
                    or not any(op == "move-exception" for _,op in ops)):
                raise RuntimeError("D8 changed original protected constant/register preparation: " + name[1])
            witnessed.add(name[1])
        if witnessed != {"create", "raised"}:
            raise RuntimeError("Missing original protected preparation witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Protected-preparation recovery changed runtime entries or original contracts")
        if coverage.get("minimum_java_release", 8) != 8:
            raise RuntimeError("Protected allocation preparation did not report its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        helper_owners = {owner[9:-1] for owner in classes}
        expected_helpers = {"Lfixture/" + name + ";->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for name in helper_owners}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Protected-preparation recovery invented a runtime entry or unexpected helper")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original protected-preparation class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"][method] != metadata or actual_exceptions[method] != exceptions[method]:
                    raise RuntimeError("Original protected-preparation method metadata or Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Protected construction changed values, failure identity or original handlers")
        original = (fixture / "fixture/Maker.java").read_text()
        outside = original
        for statement, arguments in (
            ("return", '"owned create"'),
            ("throw", '"owned raised"'),
        ):
            before = "try {\n            " + statement + " new Problem(" + arguments + ");"
            after = "Problem early = new Problem(" + arguments + ");\n        try {\n            " + statement + " early;"
            if outside.count(before) != 1:
                raise RuntimeError("Independent protected allocation counterexample anchor changed")
            outside = outside.replace(before, after)
        for label, text, mode in (
            ("changed-literal", original.replace('"owned create"', '"changed create"'), "0"),
            ("changed-handler-identity", original.replace("Trace.logFailure(failure, input)",
                "Trace.logFailure(new IllegalArgumentException(), input)"), "2"),
            ("allocation-outside-handler", outside, "1"),
        ):
            wrong = work / label / "fixture/Maker.java"
            wrong.parent.mkdir(parents=True)
            wrong.write_text(text)
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     "ProtectedPreparationHarness", "0", "1", mode], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "protected preparation contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "dependency_provider": args.dependency_provider,
                    "original_metadata_and_exceptions": "unchanged", "runtime_inventory": "original entries and reported helpers only",
                    "allocation_and_constant_argument_values": "matched", "first_and_repeated_initialization_failures": "matched",
                    "original_handler_identity_and_constructor_failures": "matched", "literal_messages": "matched",
                    "independent_literal_handler_and_protection_counterexamples": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
