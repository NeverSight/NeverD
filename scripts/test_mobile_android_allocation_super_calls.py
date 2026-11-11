#!/usr/bin/env python3
"""Verify exact superclass dispatch inside allocation arguments and retained results."""
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
    parser.add_argument("--protected-parent", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-allocation-super-call-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "allocation_super_calls/java"
        parent_package = "fixture"
        if args.protected_parent:
            authored = work / "protected-parent-source"
            shutil.copytree(fixture, authored)
            parent_package = "ancestor"
            for name in ("Base", "Middle"):
                source = authored / "fixture" / (name + ".java")
                content = source.read_text().replace("package fixture;", "package ancestor;\nimport fixture.Trace;")
                if name == "Base":
                    content = content.replace("public Object read()", "protected Object read()")
                    content = content.replace("public int number()", "protected int number()")
                destination = authored / "ancestor" / source.name
                destination.parent.mkdir(exist_ok=True)
                destination.write_text(content)
                source.unlink()
            core = authored / "fixture/Core.java"
            core.write_text(core.read_text().replace("package fixture;", "package fixture;\nimport ancestor.Middle;"))
            trace = authored / "fixture/Trace.java"
            trace.write_text(trace.read_text().replace("package fixture;", "package fixture;\nimport ancestor.Base;"))
            fixture = authored
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("AllocationSuperCallCaller", "AllocationSuperCallHarness")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 6 or len(all_methods) != 16:
            raise RuntimeError("Owned allocation-super-call inventory changed")
        providers = {"L" + parent_package + "/Base;", "L" + parent_package + "/Middle;", "Lfixture/Target;"}
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
            for variant in range(2):
                for kind in range(2):
                    for route in range(4):
                        for mode in range(13):
                            output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                "AllocationSuperCallHarness", str(variant), str(kind), str(route), str(mode)], f"{label}-{variant}-{kind}-{route}-{mode}")
                            expected = f"variant={variant},kind={kind},route={route},mode={mode},attempt=[01],trace=[0-9]+"
                            if len(output.splitlines()) != 2 or not all(re.fullmatch(expected,line) for line in output.splitlines()):
                                raise RuntimeError("Original superclass argument matrix changed: " + output)
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
            raise RuntimeError("Owned allocation super call fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original allocation-super-call runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-constructor-disassembly")
        witnesses = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] not in {"make", "live"}:
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
            new = next(i for i, (op, ref) in enumerate(ops) if op == "new-instance" and "Lfixture/Target;" in ref)
            calls = [(i,ref) for i, (op, ref) in enumerate(ops) if op.split("/")[0] == "invoke-super"]
            constructor = next(i for i, (op, ref) in enumerate(ops) if op.split("/")[0] == "invoke-direct" and "Lfixture/Target;.<init>:" in ref)
            if (len(calls) != 2 or not new < calls[0][0] < calls[1][0] < constructor
                    or "L" + parent_package + "/Middle;.read:" not in calls[0][1]
                    or "L" + parent_package + "/Middle;.number:" not in calls[1][1]
                    or ops[calls[0][0]+1][0] != "move-result-object" or ops[calls[1][0]+1][0] != "move-result"):
                raise RuntimeError("D8 changed the original inherited superclass argument witness")
            witnesses.add(name[1])
        if witnesses != {"make", "live"}:
            raise RuntimeError("Missing original superclass argument witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Allocation super call recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != 8:
            raise RuntimeError("Allocation super call unexpectedly raised its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Allocation super call recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original allocation super call class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original allocation super call method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Allocation super call call changed an original effect or result")
        text = (fixture / "fixture/Core.java").read_text()
        make = "Target result = new Target(this, super.read(), super.number());"
        mutations = {
            "virtual-dispatch": (0, [("super.read()", "this.read()", 2), ("super.number()", "this.number()", 2)]),
            "hoisted-calls": (0, [(make, "Object early = super.read(); int count = super.number(); Target result = new Target(this, early, count);", 1)]),
            "repeated-call": (0, [(make, "Target result = new Target(this, super.read() != null ? super.read() : null, super.number());", 1)]),
            "late-reference": (1, [("Trace.after = saved;", "Trace.after = Trace.value;", 1)]),
            "late-integer": (1, [("Trace.afterInt = number;", "Trace.afterInt = Trace.number;", 1)]),
        }
        for label, (variant, replacements) in mutations.items():
            wrong = work / label / "fixture/Core.java"
            wrong.parent.mkdir(parents=True)
            changed = text
            for before, after, count in replacements:
                if changed.count(before) != count:
                    raise RuntimeError("Original superclass argument counterexample anchor changed")
                changed = changed.replace(before, after)
            wrong.write_text(changed)
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     "AllocationSuperCallHarness", str(variant), "1", "0", "0"], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "allocation super call changed" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_provider": args.dependency_provider, "protected_parent": args.protected_parent,
                    "original_metadata_and_exceptions": "unchanged", "original_inherited_superclass_dispatch": "independently witnessed",
                    "class_initialization_order_and_exception_identity": "matched",
                    "partial_construction_and_retained_reference_integer_results": "matched", "subclass_receivers_nulls_and_integer_boundaries": "matched",
                    "independent_virtual_hoisted_repeated_and_late_counterexamples": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
