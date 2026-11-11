#!/usr/bin/env python3
"""Verify primitive array reads and private calls in closed conditional constructor arguments."""
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
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"): parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"): parser.add_argument("--" + name, type=Path)
    parser.add_argument("--wide", action="store_true")
    parser.add_argument("--dependency-target", action="store_true")
    args = parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-conditional-array-private-")
    with context as directory:
        work = Path(directory).resolve(); verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "conditional_array_private"
        authored = work / "authored"
        shutil.copytree(fixture, authored)
        if args.wide:
            changes = {
                "java/fixture/Core.java": [("public volatile int[] values;", "public volatile long[] values;")],
                "java/fixture/Target.java": [("public final int first;", "public final long first;"),
                    ("public final int branch;", "public final long branch;"),
                    ("public Target(int first, Object key, int branch, int next)", "public Target(long first, Object key, long branch, int next)")],
                "java/fixture/Trace.java": [("public static int replacementValue;", "public static long replacementValue;"),
                    ("public static int[] afterValues, afterFlags;", "public static long[] afterValues;\n    public static int[] afterFlags;")],
                "harness/ConditionalArrayPrivateHarness.java": [
                    ("private static int[] numbers(int first, int second) { return new int[]{first, second}; }",
                     "private static long[] numbers(long first, long second) { return new long[]{first, second}; }"),
                    ("private static int[] emptyValues() { return new int[0]; }", "private static long[] emptyValues() { return new long[0]; }"),
                    ("int[] initialValues", "long[] initialValues"), ("int[] nextValues", "long[] nextValues"),
                    ("numbers(101, 202)", "numbers(0x0102030455667788L, 0x0203040566778899L)"),
                    ("numbers(303, 404)", "numbers(0x03040506778899aaL, 0x040506078899aabbL)"),
                    ("Trace.replacementValue = 991;", "Trace.replacementValue = 0x1234567889abcdefL;")],
            }
            for name, replacements in changes.items():
                path = authored / name; text = path.read_text()
                for before, after in replacements:
                    if text.count(before) != 1: raise RuntimeError("Original wide array source anchor changed")
                    text = text.replace(before, after, 1)
                path.write_text(text)
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted((authored / "java").rglob("*.java")), originals, release=8)
        verify.compile_local(sorted((authored / "harness").glob("*.java")), harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != 7: raise RuntimeError("Original conditional array private inventory changed")
        classes = all_classes - ({"Lfixture/Target;"} if args.dependency_target else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-target.jar"
        with zipfile.ZipFile(dependency, "x") as archive: archive.writestr("fixture/Target.class", (originals / "fixture/Target.class").read_bytes())
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        dependency_hash, sdk_hash = sha(dependency), sha(args.android_jar)
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_target: classpath += ["--classpath", dependency]
        def execute(runtime, label):
            outputs = []
            cases = [(variant, 0, 0) for variant in range(16)]
            cases += [(0, fail, kind) for fail in (8, 1, 2, 3, 4) for kind in (0, 1)]
            for route in range(4):
                for variant, fail, kind in cases:
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), "ConditionalArrayPrivateHarness",
                        str(route), str(variant), str(fail), str(kind)], f"{label}-{route}-{variant}-{fail}-{kind}")
                    pattern = f"route={route},variant={variant},fail={fail},kind={kind},attempt=[01],events=[0-9]+,mask=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                        raise RuntimeError("Frozen conditional array private observer changed")
                    outputs.append(output)
            return outputs
        baseline = execute((originals, harness), "baseline")
        dex = work / "dex"; dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8-original")
        source_dex = dex / "classes.dex"; dex_hash = sha(source_dex)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original conditional array private definitions")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-argument-flow")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] != "make": continue
            ops = [(int(pc, 16), op, tail) for pc, op, tail in re.findall(r"\|([0-9a-fA-F]+):\s+(\S+)([^\n]*)", block[2])]
            allocations = [pc for pc, op, tail in ops if op == "new-instance" and "Lfixture/Target;" in tail]
            initializers = [pc for pc, op, tail in ops if op.startswith("invoke-direct") and "Lfixture/Target;.<init>:" in tail]
            if len(allocations) != 1 or len(initializers) != 1: raise RuntimeError("Original argument allocation inventory changed")
            body = [(pc, op, tail) for pc, op, tail in ops if allocations[0] < pc < initializers[0]]
            calls = [pc for pc, op, tail in body if op.startswith("invoke-direct") and "Lfixture/Core;.read:" in tail]
            loads = [(pc, op) for pc, op, _ in body if op in {"aget", "aget-wide"}]
            if (len(calls) != 1 or len(loads) < 2 or not any(op.startswith("if-") for _, op, _ in body)
                    or not any(pc < calls[0] and op == ("aget-wide" if args.wide else "aget") for pc, op in loads)):
                raise RuntimeError("Original scalar-array/private/conditional argument witness changed")
            (work / "input-method-witness.json").open("x").write(json.dumps({"allocation_pc": allocations[0],
                "initializer_pc": initializers[0], "private_call_pcs": calls, "array_loads": loads,
                "input_method_bytecode": "unchanged javac and D8 output", "dex_sha256": dex_hash}, indent=2) + "\n")
            witnessed = True
        if not witnessed: raise RuntimeError("Missing original conditional argument witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({m["identity"] for m in coverage["methods"]} != methods or coverage["class_count"] != len(classes) or
                coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count", 0) or
                coverage["status"] != "recovered" or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections") or
                coverage.get("minimum_java_release", 8) != 14): raise RuntimeError("Conditional argument recovery changed original contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=14,
            classpath=dependency if args.dependency_target else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {m["class"] + "->" + m["name"] + m["prototype"] for m in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Conditional argument recovery invented or omitted a runtime definition")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != after[owner].get(key): raise RuntimeError("Original conditional class metadata changed: " + key)
            for method, metadata in records[owner]["methods"].items():
                if after[owner]["methods"].get(method) != metadata or after_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original conditional method metadata or Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_target else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline: raise RuntimeError("Conditional arguments changed snapshots, arrays, private calls or failures")
        core_source = (authored / "java/fixture/Core.java").read_text()
        target_source = (authored / "java/fixture/Target.java").read_text()
        mutations = (
            ("lost-first", "Core", "return new Target(values[index],", "return new Target(Trace.replacementValue,", 0),
            ("lost-return", "Core", "return captured;", "return token;", 0),
            ("lost-mutation", "Core", "values[index] = Trace.replacementValue;", "values[index] = values[index];", 0),
            ("wrong-index", "Core", "read(index),", "read(index ^ 1),", 0),
            ("wrong-fallback", "Core", " : values[index],", " : Trace.replacementValue,", 1),
            ("duplicate-private", "Core", "read(index),", "read(index) == null ? null : read(index),", 0),
            ("eager-private", "Core", "return new Target(values[index], read(index),", "Object eager = read(index); return new Target(values[index], eager,", 0),
            ("lost-tail", "Target", "Trace.hit(4);", "", 0),
        )
        for label, owner, before, changed, variant in mutations:
            text = core_source if owner == "Core" else target_source
            if text.count(before) != 1: raise RuntimeError("Owned conditional counterexample anchor changed")
            wrong = work / label / (owner + ".java"); wrong.parent.mkdir(); wrong.open("x").write(text.replace(before, changed, 1))
            compiled = work / (label + "-classes"); verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                "ConditionalArrayPrivateHarness", "1", str(variant), "0", "0"], text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned conditional array private contract" not in result.stderr:
                raise RuntimeError("Frozen observer missed conditional argument change: " + label)
        if ({"originals": hashes(originals), "harness": hashes(harness)} != frozen or sha(source_dex) != dex_hash
                or sha(dependency) != dependency_hash or sha(args.android_jar) != sdk_hash): raise RuntimeError("Original conditional inputs or observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods), "wide": args.wide,
            "dependency_target": args.dependency_target, "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
            "independent_wrong_programs": len(mutations), "minimum_java_release": 14,
            "original_inventory_metadata_and_exceptions": "unchanged", "original_inputs_and_frozen_observers": "unchanged",
            "array_private_snapshots_guarded_fallbacks_reference_identity_and_exact_failures": "matched"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n"); print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__": raise SystemExit(main())
