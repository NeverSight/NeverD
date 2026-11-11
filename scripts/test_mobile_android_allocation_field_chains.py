#!/usr/bin/env python3
"""Verify chained reference fields and scalar constructor arguments retain allocation and alias order."""
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
    parser.add_argument("--boolean-fields", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-allocation-field-chain-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "allocation_field_chains/java"
        originals, harness = work / "originals", work / "harness"
        harness_sources = [FIXTURES / "harness" / (name + ".java") for name in
                           ("AllocationFieldChainCaller", "AllocationFieldChainHarness")]
        if args.boolean_fields:
            # Author the boolean source profile before its original javac/D8
            # compilation. No classfile, DEX, or original caller is patched.
            boolean_fixture = work / "boolean-source"
            for source in sorted(fixture.rglob("*.java")):
                text = source.read_text()
                text = text.replace("int count", "boolean count").replace("int number", "boolean number")
                text = text.replace("public static int events, failAt, initialNumber, afterNumber, afterInt;",
                                    "public static int events, failAt;\n    public static boolean initialNumber, afterNumber, afterInt;")
                target = boolean_fixture / source.relative_to(fixture)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text)
            fixture = boolean_fixture
            source = harness_sources[1]
            text = source.read_text()
            replacements = [
                ("mode == 1 ? Integer.MIN_VALUE : mode == 2 ? Integer.MAX_VALUE : 17", "mode == 1"),
                ("Trace.initialNumber ^ 0x5a5a5a5a", "!Trace.initialNumber"),
                ("int beforeNumber = ~Trace.initialNumber;", "boolean beforeNumber = !Trace.initialNumber;"),
                ("Trace.afterInt = 53;", "Trace.afterInt = true;"),
                ("int trace = 0, kind = 0, number = 0;", "int trace = 0, kind = 0; boolean number = false;"),
                ("int expectedNumber = beforeNumber;", "boolean expectedNumber = beforeNumber;"),
                ("published ? number : 53", "published ? number : true"),
                ("fields ? number : 0", "fields ? number : false"),
            ]
            for before, after in replacements:
                if text.count(before) != 1:
                    raise RuntimeError("Original boolean profile source anchor changed")
                text = text.replace(before, after)
            target = work / "boolean-harness" / source.name
            target.parent.mkdir()
            target.write_text(text)
            harness_sources[1] = target
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local(harness_sources, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 10:
            raise RuntimeError("Owned allocation-field-chain inventory changed")
        providers = {"Lfixture/Target;"}
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
            for variant in range(3):
                for route in range(4):
                    for mode in range(12):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "AllocationFieldChainHarness", str(variant), str(route), str(mode)], f"{label}-{variant}-{route}-{mode}")
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
            raise RuntimeError("Owned allocation field chain fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original allocation-field-chain runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-constructor-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] != "make":
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
            new = next(i for i, (op, ref) in enumerate(ops) if op == "new-instance" and "Lfixture/Target;" in ref)
            link = next(i for i, (op, ref) in enumerate(ops) if op == "iget-object" and "Lfixture/Holder;.link:" in ref)
            value = next(i for i, (op, ref) in enumerate(ops) if op == "iget-object" and "Lfixture/Link;.value:" in ref)
            number = next(i for i, (op, ref) in enumerate(ops) if op == ("iget-boolean" if args.boolean_fields else "iget") and "Lfixture/Holder;.count:" in ref)
            constructor = next(i for i, (op, ref) in enumerate(ops) if op.split("/")[0] == "invoke-direct" and "Lfixture/Target;.<init>:" in ref)
            def regs(index):
                return re.findall(r"\bv(\d+)\b", ops[index][1])
            if not new < link < value < number < constructor or regs(value)[0] != regs(value)[1] or regs(value)[1] != regs(link)[0]:
                raise RuntimeError("D8 changed the original chained-field register reuse witness")
            witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original allocation field chain witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Allocation field chain recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != 8:
            raise RuntimeError("Allocation field chain unexpectedly raised its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Allocation field chain recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original allocation field chain class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original allocation field chain method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Allocation field chain call changed an original effect or result")
        text = (fixture / "fixture/Core.java").read_text()
        make = "Target result = new Target(holder.link.value, holder.count);"
        mutations = {
            "hoisted-reference": (0, 0, [(make, "Object early = holder.link.value; Target result = new Target(early, holder.count);", 1)]),
            "hoisted-integer": (0, 0, [(make, "int early = holder.count; Target result = new Target(holder.link.value, early);", 1)]),
            "late-reference": (1, 0, [("Trace.after = saved;", "Trace.after = holder.link.value;", 1)]),
            "late-integer": (1, 0, [("Trace.afterInt = number;", "Trace.afterInt = holder.count;", 1)]),
            "late-link": (2, 0, [("Trace.afterLink = saved;", "Trace.afterLink = holder.link;", 1)]),
        }
        for label, (variant, mode, replacements) in mutations.items():
            if args.boolean_fields:
                replacements = [(before, after.replace("int early", "boolean early"), count)
                                for before, after, count in replacements]
            wrong = work / label / "fixture/Core.java"
            wrong.parent.mkdir(parents=True)
            changed = text
            for before, after, count in replacements:
                if changed.count(before) != count:
                    raise RuntimeError("Original chained-field counterexample anchor changed")
                changed = changed.replace(before, after)
            wrong.write_text(changed)
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     "AllocationFieldChainHarness", str(variant), "1", str(mode)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "allocation field chain changed" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "original_metadata_and_exceptions": "unchanged",
                    "boolean_fields": args.boolean_fields,
                    "original_chained_fields_and_scalar_read": "independently witnessed",
                    "class_initialization_order_and_exception_identity": "matched",
                    "partial_construction_and_retained_reference_integer_and_link": "matched", "null_receivers_and_scalar_values": "matched",
                    "independent_hoisted_and_late_field_counterexamples": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
