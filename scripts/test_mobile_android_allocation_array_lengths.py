#!/usr/bin/env python3
"""Verify array lengths retain allocation, class initialization and constructor effect order."""
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
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--arithmetic-prefix", action="store_true")
    parser.add_argument("--negative-bias", action="store_true")
    args = parser.parse_args()
    args.arithmetic_prefix |= args.negative_bias
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-allocation-array-length-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "allocation_array_lengths/java"
        bias = -7 if args.negative_bias else 17
        prefix_expression = f"prefix+({bias})"
        harness_source = FIXTURES / "harness/AllocationArrayLengthHarness.java"
        if args.arithmetic_prefix:
            authored = work / "arithmetic-source"
            shutil.copytree(fixture, authored)
            core = authored / "fixture/Core.java"
            text = core.read_text()
            anchor = "        Target result = new Target("
            if text.count(anchor) != 4:
                raise RuntimeError("Original integer prefix allocation anchors changed")
            text = text.replace(anchor, "        int prefix = flag ? 1 : 0;\n" + anchor)
            core.write_text(text.replace("new Target(", "new Target(" + prefix_expression + ","))
            target = authored / "fixture/Target.java"
            text = target.read_text()
            for before, after in (
                ("    public int length;", "    public int prefix;\n    public int length;"),
                ("public Target(int length,", "public Target(int prefix, int length,"),
                ("        this.length = length;", "        this.prefix = prefix;\n        this.length = length;"),
            ):
                if text.count(before) != 1:
                    raise RuntimeError("Original prefix provider declaration changed")
                text = text.replace(before, after, 1)
            target.write_text(text)
            text = harness_source.read_text()
            anchor = "                Target escaped = (Target) Trace.target;"
            if text.count(anchor) != 1:
                raise RuntimeError("Original array observer anchor changed")
            text = text.replace(anchor, anchor + "\n                require(escaped.prefix == (completed ? (flag ? 1 : 0) + Integer.parseInt(args[3]) : 0));", 1)
            harness_source = work / "arithmetic-harness/AllocationArrayLengthHarness.java"
            harness_source.parent.mkdir()
            harness_source.open("x").write(text)
            fixture = authored
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness/AllocationArrayLengthCaller.java", harness_source],
                             harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 12:
            raise RuntimeError("Owned allocation-array-length inventory changed")
        providers = {"Lfixture/Target;", "Lfixture/Inner;"}
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            result = []
            for variant in range(4):
                for route in range(4):
                    for mode in range(10):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "AllocationArrayLengthHarness", str(variant), str(route), str(mode), str(bias)], f"{label}-{variant}-{route}-{mode}")
                        expected = f"variant={variant},route={route},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected,line) for line in output.splitlines()):
                            raise RuntimeError("Original constructor behavior matrix changed: " + output)
                        result.append(output)
            return result

        baseline = execute((originals, harness), "baseline")
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned allocation array length fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original allocation-array-length runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-constructor-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] not in {"direct", "field", "live", "nested"}:
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
            new = next(i for i, (op, ref) in enumerate(ops) if op == "new-instance" and "Lfixture/Target;" in ref)
            lengths = [i for i, (op, ref) in enumerate(ops) if op == "array-length"]
            constructor = next(i for i, (op, ref) in enumerate(ops) if op.split("/")[0] == "invoke-direct" and "Lfixture/Target;.<init>:" in ref)
            if len(lengths) != 1 or not new < lengths[0] < constructor:
                raise RuntimeError("D8 changed the original allocation/array-length ordering witness")
            if args.arithmetic_prefix and name[1] == "direct":
                arithmetic = [i for i, (op, _) in enumerate(ops)
                              if op.split("/")[0] in {"add-int", "sub-int"}]
                if not any(new < i < lengths[0] for i in arithmetic):
                    raise RuntimeError("D8 moved the original integer prefix out of the allocation")
            if name[1] == "nested":
                inner = next(i for i, (op, ref) in enumerate(ops) if op == "new-instance" and "Lfixture/Inner;" in ref)
                if not lengths[0] < inner < constructor:
                    raise RuntimeError("D8 changed the length-before-inner-allocation witness")
            witnessed.add(name[1])
        if witnessed != {"direct", "field", "live", "nested"}:
            raise RuntimeError("Missing original array length allocation witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Allocation array length recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != 8:
            raise RuntimeError("Allocation array length unexpectedly raised its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Allocation array length recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original allocation array length class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original allocation array length method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Allocation array length call changed an original effect or result")
        text = (fixture / "fixture/Core.java").read_text()
        mutations = {
            "hoisted-direct-length": (0, 4, [("Target result = new Target(input.length, null, flag);",
                "int early = input.length; Target result = new Target(early, null, flag);")]),
            "hoisted-field-length": (1, 0, [("Target result = new Target(holder.array.length, null, flag);",
                "int early = holder.array.length; Target result = new Target(early, null, flag);")]),
            "late-reread": (2, 9, [("Trace.after = saved;", "Trace.after = holder.array.length;")]),
            "late-nested-length": (3, 0, [
                ("public static Target nested(Holder holder, int[] input, boolean flag) {",
                 "public static Target nested(Holder holder, int[] input, boolean flag) { Inner child;"),
                ("new Target(holder.array.length, new Inner(), flag)",
                 "new Target(after(holder, child = new Inner()), child, flag)"),
                ("\n}\n", "\n    private static int after(Holder holder, Object ignored) { return holder.array.length; }\n}\n")]),
        }
        if args.arithmetic_prefix:
            mutations["wrong-arithmetic-prefix"] = (0, 0, [
                (f"new Target({prefix_expression},input.length",
                 f"new Target(prefix+({bias + 1}),input.length")])
        for label, (variant, mode, replacements) in mutations.items():
            wrong = work / label / "fixture/Core.java"
            wrong.parent.mkdir(parents=True)
            changed = text
            for before, after in replacements:
                if args.arithmetic_prefix and label != "wrong-arithmetic-prefix":
                    before = before.replace("new Target(", "new Target(" + prefix_expression + ",")
                    after = after.replace("new Target(", "new Target(" + prefix_expression + ",")
                if changed.count(before) != 1:
                    raise RuntimeError("Original allocation counterexample anchor changed")
                changed = changed.replace(before, after)
            wrong.write_text(changed)
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     "AllocationArrayLengthHarness", str(variant), "1", str(mode), str(bias)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "allocation array length changed" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "arithmetic_prefix": args.arithmetic_prefix, "arithmetic_bias": bias if args.arithmetic_prefix else None,
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "original_metadata_and_exceptions": "unchanged", "original_array_length_order": "independently witnessed",
                    "class_initialization_order_and_exception_identity": "matched",
                    "array_operand_aliases_and_live_length_registers": "matched", "nested_allocation_and_boolean_argument": "matched",
                    "independent_hoist_reread_and_late_nested_length_counterexamples": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
