#!/usr/bin/env python3
"""Verify nested anonymous capture allocations and exact initialization order."""
from __future__ import annotations

import argparse
import copy
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nested-captures-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "nested_capture_allocations/java"
        originals, harness = work / "originals", work / "harness"
        inputs = sorted(fixture.rglob("*.java"))
        verify.compile_local(inputs, originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("NestedCaptureHarness", "NestedCaptureCaller")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 8 or len(all_methods) != 15:
            raise RuntimeError("Owned nested capture inventory changed")
        classes = all_classes - ({"Lfixture/Base;", "Lfixture/Reader;"} if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)

        def provider_jar(root: Path, target: Path) -> None:
            with zipfile.ZipFile(target, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                for name in ("Base", "Reader"):
                    archive.writestr("fixture/" + name + ".class", (root / ("fixture/" + name + ".class")).read_bytes())

        dependency = work / "owned-provider.jar"
        provider_jar(originals, dependency)
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime: tuple[Path, ...], prefix: str) -> list[str]:
            actual = []
            for mode in range(6):
                for route in range(4):
                    trace = 1 if mode == 1 else 12 if mode == 2 else 1233 if mode == 3 else 123434 if mode == 4 else 12345345
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "NestedCaptureHarness", str(mode), str(route)], f"{prefix}-{mode}-{route}")
                    if output.splitlines() != [f"mode={mode},route={route},trace={trace}"]:
                        raise RuntimeError("Original nested capture order changed: " + output)
                    actual.append(output)
            return actual

        baseline = execute((originals, harness), "baseline")

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned nested capture fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original nested capture runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-call-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Creator;" or not name or name[1] != "make":
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            if (ops.count("new-instance") != 3 or ops.count("invoke-direct") != 3
                    or [op for op in ops if op in {"new-instance", "invoke-direct"}]
                    != ["new-instance", "new-instance", "invoke-direct", "new-instance", "invoke-direct", "invoke-direct"]):
                raise RuntimeError("D8 changed the original nested capture allocation order")
            witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original nested capture witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage["status"] != "partial" or coverage["minimum_java_release"] != 25
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Nested capture recovery lost original runtime entries or contracts")
        bindings = coverage.get("class_source_bindings", [])
        expected_owners = {"Lfixture/Creator$1;", "Lfixture/Creator$2;"}
        if ({row["class"] for row in bindings} != expected_owners or len(bindings) != 2
                or any(row["binding_kind"] != "anonymous-to-named-local" or not row["metadata_differences"] for row in bindings)):
            raise RuntimeError("Nested capture recovery lost its exact local source bindings")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage["minimum_java_release"],
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        renames = {}
        for row in bindings:
            matches = [name for name, metadata in actual_records.items()
                       if (metadata.get("inner_class") or {}).get("name") == row["source_name"]
                       and metadata.get("enclosing_method") == records[row["class"]]["enclosing_method"]]
            if len(matches) != 1:
                raise RuntimeError("Source binding has no unique independently compiled local class")
            renames[row["class"]] = matches[0]
        def rename(value: str) -> str:
            for original, rebuilt_name in renames.items():
                value = value.replace(original, rebuilt_name)
            return value
        helpers = {rename(r["class"] + "->" + r["name"] + r["prototype"]) for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {rename(owner) + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if owner != "Lfixture/Reader;"}
        if (actual_classes != {rename(owner) for owner in classes}
                or set(actual_methods) != {rename(identity) for identity in methods} | expected_helpers
                or helpers != expected_helpers):
            raise RuntimeError("Nested capture recovery changed inventory beyond disclosed local bindings and helpers")
        for identity in classes:
            before = copy.deepcopy(records[identity])
            after = actual_records[rename(identity)]
            before["fields"] = {rename(key): value for key, value in before["fields"].items()}
            before["methods"] = {rename(key): value for key, value in before["methods"].items()}
            if identity in renames:
                binding = next(row for row in bindings if row["class"] == identity)
                before["inner_class"] = {"inner": renames[identity], "outer": None,
                                         "name": binding["source_name"], "access": before["inner_class"]["access"]}
                for field in before["fields"].values():
                    field["access"] &= ~0x1000
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original capture metadata changed beyond its binding: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                original = next(key for key in exceptions if rename(key) == method)
                if after["methods"][method] != metadata or actual_exceptions[method] != exceptions[original]:
                    raise RuntimeError("Original capture method metadata or Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Nested capture invocation changed initialization cycles or ordered failures")
        negative = work / "private-constructor-originals"
        shutil.copytree(originals, negative)
        receipt = change_method_flags(negative / "fixture/Creator$1.class", b"(ILfixture/Box;)V",
                                      0, 2, name=b"<init>")
        (work / "private-constructor-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = dex(negative, work / "private-constructor-dex")
        rejected = work / "private-constructor-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath),
                                 "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "private-constructor-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "constructor" not in result.stderr:
            raise RuntimeError("Nested capture private constructor counterexample was not rejected")
        hoisted = work / "hoisted/fixture/Creator.java"
        hoisted.parent.mkdir(parents=True)
        hoisted.write_text("""package fixture;
public final class Creator {
    public static Bundle make(Box box, Object value) {
        Base<Object> first = new Base<Object>(3) { public Object read() { return box.read(); } };
        Base<Object> second = new Base<Object>(4) { public Object read() { return value; } };
        return new Bundle(first, second, value);
    }
}
""")
        verify.compile_local([hoisted], work / "hoisted-classes", classpath=originals, release=8)
        result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (work / "hoisted-classes", originals, harness))),
                                 "NestedCaptureHarness", "0", "0"], text=True, capture_output=True,
                                timeout=300, env=verify.environment)
        (work / "hoisted-capture-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle failed to detect captures before outer allocation")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": 24, "successful_constructions": 8, "ordered_failures": 40,
                    "dependency_provider": args.dependency_provider,
                    "original_metadata_and_exceptions": "unchanged_except_disclosed_local_binding",
                    "nested_capture_allocation_order": "matched", "same_code_counterexample": "rejected",
                    "independent_hoisted_capture_counterexample": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
