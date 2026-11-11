#!/usr/bin/env python3
"""Verify unlocked prefix exits, monitor bypasses and complete acquired releases."""
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
    parser.add_argument("--dependency-trace", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-monitor-bypass-tails-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "monitor_bypass_tails/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("MonitorBypassTailsHarness", "MonitorBypassTailsCaller")], harness,
                             classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 2 or len(all_methods) != 9:
            raise RuntimeError("Owned monitor bypass tail inventory changed")
        classes = all_classes - ({"Lfixture/Trace;"} if args.dependency_trace else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x") as archive:
            archive.writestr("fixture/Trace.class", (originals / "fixture/Trace.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_trace:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            outputs = []
            for entry in range(9):
                for route in range(4):
                    for mode in (0, 2, 3, 4, 5, 6, 11):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "MonitorBypassTailsHarness", str(entry), str(route), str(mode)],
                            f"{label}-{entry}-{route}-{mode}")
                        expected = f"entry={entry},route={route},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Original monitor bypass tail matrix changed: " + output)
                        outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        dex_dir = work / "dex"
        dex_dir.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--release", "--min-api", "24", *classpath, "--output", dex_dir, *paths], "d8-original")
        source_dex = dex_dir / "classes.dex"
        if list(dex_dir.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned monitor bypass tail fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original monitor bypass tail runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-carrier-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] != "update": continue
            ops = [(int(pc, 16), op, tail) for pc, op, tail in
                   re.findall(r"\|([0-9a-fA-F]+):\s+(\S+)([^\n]*)", block[2])]
            enters = [pc for pc, op, tail in ops if op == "monitor-enter"]
            exits = [pc for pc, op, tail in ops if op == "monitor-exit"]
            late_returns = [pc for pc, op, tail in ops if op == "return-void" and pc > max(exits, default=0)]
            targets = [int(target, 16) for pc, op, tail in ops if enters and pc < enters[0]
                       and (op.startswith("if-") or op.startswith("goto"))
                       for target in re.findall(r"\b([0-9a-fA-F]{4,})\s+//", tail)]
            normal_tail, pending = set(), [next((pc for pc, op, tail in ops if exits and pc > exits[0]), -1)]
            by_pc = {pc: (index, op, tail) for index, (pc, op, tail) in enumerate(ops)}
            while pending:
                pc = pending.pop()
                if pc in normal_tail: continue
                if pc not in by_pc: raise RuntimeError("D8 monitor continuation has an unknown target")
                normal_tail.add(pc)
                index, op, tail = by_pc[pc]
                if op.startswith("goto") or op.startswith("if-"):
                    branch = re.search(r"\b([0-9a-fA-F]{4,})\s+//", tail)
                    if not branch: raise RuntimeError("D8 monitor continuation has no exact branch target")
                    pending.append(int(branch[1], 16))
                if not op.startswith(("goto", "return", "throw")):
                    if index + 1 == len(ops): raise RuntimeError("D8 monitor continuation falls outside its body")
                    pending.append(ops[index + 1][0])
            if (len(enters) != 1 or len(exits) != 2
                    or not (set(targets).intersection(late_returns) - normal_tail)):
                raise RuntimeError("D8 did not produce the original monitor and late prefix returns")
            witnessed = True
        if not witnessed: raise RuntimeError("Missing actual D8 monitor bypass witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Monitor bypass tail recovery changed runtime entries or original contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=8, classpath=dependency if args.dependency_trace else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Monitor bypass tail recovery invented or omitted a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original monitor bypass tail class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after["methods"].get(identity) != metadata or actual_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Original monitor bypass tail method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_trace else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Monitor bypass tail recovery changed ordered effects, lock ownership or failure identity")
        original = (fixture / "fixture/Core.java").read_text()
        prefix = "int capacity = Trace.capacity(width, height);\n            if (capacity <= 0) return;\n            synchronized (Trace.lock) {"
        for label, mutant, config, mode in (
            ("missing-acquisition", original.replace("synchronized (Trace.lock)", ""), "2", "0"),
            ("after-inside-lock", original.replace("Trace.after();", "").replace("if (state == 3) Trace.hit(4);", "if (state == 3) Trace.hit(4); Trace.after();"), "2", "0"),
            ("missing-bypass", original.replace("if (state != 1)", "if (true)"), "1", "0"),
            ("missing-prefix-return", original.replace("if (width <= 0 || height <= 0) return;", ""), "4", "0"),
            ("capacity-inside-lock", original.replace(prefix, "synchronized (Trace.lock) {\n                int capacity = Trace.capacity(width, height); if (capacity <= 0) return;"), "2", "2"),
        ):
            if mutant == original: raise RuntimeError("Independent monitor counterexample anchor changed")
            wrong = work / label / "fixture/Core.java"
            wrong.parent.mkdir(parents=True); wrong.write_text(mutant)
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                    "MonitorBypassTailsHarness", config, "1", mode], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "monitor bypass tail contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_trace": args.dependency_trace, "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "original_inventory_metadata_and_exceptions": "unchanged", "monitor_acquisition_and_normal_exceptional_releases": "unchanged",
                    "prefix_bypass_early_returns_and_unlocked_continuation": "matched", "original_lock_identity": "matched",
                    "first_repeated_initialization_failures": "matched",
                    "catch_failure_identity_and_monitor_ownership": "matched", "independent_compiler_counterexamples": "all five detected",
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
