#!/usr/bin/env python3
"""Verify array lengths inside conditional constructor arguments and their failures."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "java-home", "compiler-java-home"):
        parser.add_argument("--" + name, required=True, type=Path)
    for name in ("d8", "work-dir", "baseline-neverd"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-target", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-conditional-array-length-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "conditional_array_lengths/java"
        originals, harness = work / "originals", work / "harness"
        name = "ConditionalArrayLengthHarness"
        compiler.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        compiler.compile_local([FIXTURES / "harness" / (n + ".java") for n in (name, "ConditionalArrayLengthCaller")],
                               harness, classpath=originals, release=8)
        sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
        frozen = {"originals": hashes(originals), "harness": hashes(harness), "sdk": sha(args.android_jar), "cli": sha(args.neverd)}
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        all_classes, all_methods = compiler_inventory(compiler, originals)
        if len(all_classes) != 4 or len(all_methods) != 9:
            raise RuntimeError("Owned conditional length inventory changed")
        classes = all_classes - ({"Lfixture/Target;"} if args.dependency_target else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        dependency = work / "owned-target.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            jar.writestr("fixture/Target.class", (originals / "fixture/Target.class").read_bytes())
        dependency_hash = sha(dependency)
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_target:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            results = []
            for route in range(4):
                for mode in range(16):
                    output = verify.run([compiler.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), str(mode)],
                                        f"{label}-{route}-{mode}")
                    if len(output.splitlines()) != 2 or any(not re.fullmatch(
                            f"route={route},mode={mode},attempt={attempt},events=[0-9]+", line)
                            for attempt, line in enumerate(output.splitlines())):
                        raise RuntimeError("Frozen conditional length observer changed")
                    results.append(output)
            return results

        baseline = execute((originals, harness), "baseline")
        mutations = (
            ("hoisted-length", "Core", "return new Target(holder.first.length - 2 > 0 ? holder.first.length - 2 : holder.first.length,",
             "int early = holder.first.length; return new Target(early - 2 > 0 ? early - 2 : early,", 0),
            ("eager-unselected-length", "Core", "return new Target(", "int early = holder.second.length; return new Target(", 7),
            ("wrong-capacity", "Core", "holder.first.length - 2 > 0 ? holder.first.length - 2 : holder.first.length", "holder.first.length", 0),
            ("lost-read", "Core", ": Trace.read(holder)", ": Trace.value", 0),
            ("duplicate-read", "Core", ": Trace.read(holder)", ": Trace.read(holder) + Trace.read(holder) - Trace.value", 0),
            ("late-length", "Target", "this.capacity = capacity;", "this.capacity = Trace.holder.first.length;", 0),
            ("lost-checked-failure", "Target", "Trace.hit(3);", "", 11),
            ("changed-exception-identity", "Trace", "throw checked;", "throw new IOException(checked);", 10),
        )
        for label, owner, before, after, mode in mutations:
            text = (fixture / "fixture" / (owner + ".java")).read_text()
            if text.count(before) != 1:
                raise RuntimeError("Owned conditional length counterexample anchor changed")
            wrong = work / label / (owner + ".java")
            wrong.parent.mkdir()
            wrong.write_text(text.replace(before, after, 1))
            compiled = work / (label + "-classes")
            compiler.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([compiler.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     name, "1", str(mode)], capture_output=True, text=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned conditional length contract" not in result.stderr:
                raise RuntimeError("Frozen observer missed conditional length change: " + label)
        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        dex_hash = sha(source_dex)
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned conditional length DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {c["descriptor"] for c in inventory["classes"]} != classes or {m["identity"] for m in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original conditional length definitions")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-arguments")
        witnessed = []
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            method = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not method or method[1] != "make":
                continue
            ops = [(int(pc, 16), op, tail) for pc, op, tail in re.findall(r"\|([0-9a-fA-F]+):\s+(\S+)([^\n]*)", block[2])]
            allocations = [pc for pc, op, tail in ops if op == "new-instance" and "Lfixture/Target;" in tail]
            initializers = [pc for pc, op, tail in ops if op.startswith("invoke-direct") and "Lfixture/Target;.<init>:" in tail]
            if len(allocations) != 1 or len(initializers) != 1:
                raise RuntimeError("Original conditional length allocation inventory changed")
            body = [(pc, op) for pc, op, _ in ops if allocations[0] < pc < initializers[0]]
            if (sum(op == "array-length" for _, op in body) != 4
                    or sum(op.startswith("if-") for _, op in body) != 2
                    or not any(op.split("/")[0] == "add-int" for _, op in body)):
                raise RuntimeError("D8 moved or changed conditional length argument witnesses")
            witnessed.append({"allocation_pc": allocations[0], "initializer_pc": initializers[0], "argument_operations": body})
        if len(witnessed) != 1:
            raise RuntimeError("Missing original conditional length argument witness")
        (work / "input-method-witness.json").write_text(json.dumps({"dex_sha256": dex_hash, "methods": witnessed}, indent=2) + "\n")
        if args.baseline_neverd:
            before_hash = sha(args.baseline_neverd)
            result = subprocess.run([args.baseline_neverd.resolve(), "mobile", source_dex, *classpath, "-o", work / "before"],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / "before.log").write_text(result.stdout + result.stderr)
            if (result.returncode != 1 or (work / "before").exists()
                    or "nested allocation has an unproved executable target" not in result.stdout + result.stderr
                    or sha(args.baseline_neverd) != before_hash):
                raise RuntimeError("Conditional length baseline rejection changed")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({m["identity"] for m in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count") or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release") != 14
                or any(m["status"] != "recovered" for m in coverage["methods"])):
            raise RuntimeError("Conditional length recovery changed original contracts")
        rebuilt = work / "rebuilt"
        sources = sorted((output / "sources").rglob("*.java"))
        if len(sources) != len(classes):
            raise RuntimeError("Conditional length source inventory changed")
        compiler.compile_local(sources, rebuilt, classpath=dependency if args.dependency_target else None, release=14)
        actual, actual_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        helpers = {m["class"] + "->" + m["name"] + m["prototype"] for m in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if set(actual) != classes or helpers != expected_helpers:
            raise RuntimeError("Conditional length recovery changed definitions or helper accounting")
        for owner in classes:
            before, after = copy.deepcopy(facts[owner]), copy.deepcopy(actual[owner])
            after["methods"] = {m: f for m, f in after["methods"].items() if m not in helpers}
            if before.pop("major") != 52 or after.pop("major") != 58 or before != after:
                raise RuntimeError("Original conditional length metadata, roles or Code presence changed")
            if any(exceptions[m] != actual_exceptions[m] for m in facts[owner]["methods"]):
                raise RuntimeError("Original conditional length Exceptions changed")
        runtime = (rebuilt, harness, dependency) if args.dependency_target else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Conditional length changed effects, snapshots or failures")
        if (frozen != {"originals": hashes(originals), "harness": hashes(harness), "sdk": sha(args.android_jar), "cli": sha(args.neverd)}
                or sha(dependency) != dependency_hash or sha(source_dex) != dex_hash):
            raise RuntimeError("Original conditional length inputs or observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "independent_wrong_programs": len(mutations),
                    "dependency_target": args.dependency_target, "minimum_java_release": 14,
                    "original_metadata_code_presence_roles_and_exceptions": "unchanged", "all_emitted_sources_compiled": True,
                    "original_conditional_array_length_order": "independently witnessed",
                    "class_initialization_branch_selection_snapshots_and_exact_failures": "matched", "frozen_inputs": frozen}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps({k: v for k, v in evidence.items() if k != "frozen_inputs"}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
