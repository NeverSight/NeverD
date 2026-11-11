#!/usr/bin/env python3
"""Verify private reference and wide calls inside ordinary, protected and synchronized allocations."""
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
    parser.add_argument("--dependency-target", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-allocation-private-calls-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "allocation_private_calls/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("AllocationPrivateCallsHarness", "AllocationPrivateCallsCaller")], harness,
                             classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != 11:
            raise RuntimeError("Owned allocation private result inventory changed")
        classes = all_classes - ({"Lfixture/Target;"} if args.dependency_target else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-target.jar"
        with zipfile.ZipFile(dependency, "x") as archive:
            archive.writestr("fixture/Target.class", (originals / "fixture/Target.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_target:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            outputs = []
            for entry in range(3):
                for route in range(4):
                    for mode in range(8):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "AllocationPrivateCallsHarness", str(entry), str(route), str(mode)],
                            f"{label}-{entry}-{route}-{mode}")
                        expected = f"entry={entry},route={route},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Original allocation private matrix changed: " + output)
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
            raise RuntimeError("Owned private result fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original protected private result runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-private-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] not in {"create", "guarded", "locked"}:
                continue
            ops = [(int(pc, 16), op, tail) for pc, op, tail in
                   re.findall(r"\|([0-9a-fA-F]+):\s+(\S+)([^\n]*)", block[2])]
            allocation = [pc for pc, op, tail in ops if op == "new-instance" and "Lfixture/Target;" in tail]
            constructor = [pc for pc, op, tail in ops if op.startswith("invoke-direct") and "Lfixture/Target;.<init>:" in tail]
            if len(allocation) != 1 or len(constructor) != 1:
                raise RuntimeError("D8 changed private allocation/constructor inventory")
            calls = [(pc, op, tail) for pc, op, tail in ops if allocation[0] < pc < constructor[0]
                     and op.startswith("invoke-direct") and "Lfixture/Core;.read" in tail]
            results = [op for pc, op, _ in ops if allocation[0] < pc < constructor[0] and op.startswith("move-result")]
            if (len(calls) != 2 or results != ["move-result-object", "move-result-wide"]
                    or {re.search(r"\.read([^:]*):", tail)[1] for _, _, tail in calls} != {"", "Long"}):
                raise RuntimeError("D8 changed original private invocation or result carriers")
            if name[1] in {"guarded", "locked"} and not any(op == "move-exception" for _, op, _ in ops):
                raise RuntimeError("D8 lost original protected private cleanup")
            if name[1] == "locked" and (sum(op == "monitor-enter" for _, op, _ in ops) != 1
                                        or sum(op == "monitor-exit" for _, op, _ in ops) != 2):
                raise RuntimeError("D8 lost original private allocation monitor ownership")
            witnessed.add(name[1])
        if witnessed != {"create", "guarded", "locked"}:
            raise RuntimeError("Missing original private result allocation witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Allocation private result recovery changed runtime entries or original contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=8, classpath=dependency if args.dependency_target else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Allocation private result recovery invented or omitted a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original private class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after["methods"].get(identity) != metadata or actual_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Original private method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_target else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Scalar allocation changed raw values, ordered effects or failure identity")
        text = (fixture / "fixture/Core.java").read_text()
        for label, mutant, entry, mode in (
            ("bypassed-reference-call", text.replace("receiver.read(input, mask)", "receiver.value"), "0", "0"),
            ("repeated-reference-call", text.replace("number ^= mask;", "number ^= mask; Trace.hit(1); Trace.consume(input);"), "0", "0"),
            ("narrowed-long-result", text.replace("return number ^ mask;", "return (long) (int) (number ^ mask);"), "0", "0"),
            ("changed-reference-result", text.replace("return value;", "return new Object();"), "0", "0"),
            ("missing-monitor", text.replace("synchronized (Trace.lock) {", "{"), "2", "0"),
            ("wrong-caught-identity", text.replace("Trace.last = failure", "Trace.last = new IllegalArgumentException()"), "1", "1"),
        ):
            if mutant == text:
                raise RuntimeError("Independent private counterexample anchor changed")
            wrong = work / label / "fixture/Core.java"
            wrong.parent.mkdir(parents=True)
            wrong.write_text(mutant)
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                    "AllocationPrivateCallsHarness", entry, "1", mode], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "allocation private call contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_target": args.dependency_target, "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "original_inventory_metadata_and_exceptions": "unchanged", "private_reference_and_wide_calls": "matched",
                    "reference_identity_and_full_long_bits": "matched", "immutable_result_snapshots": "matched",
                    "constructor_and_first_repeated_initialization_failures": "matched",
                    "catch_failure_identity_and_monitor_ownership": "matched", "independent_compiler_counterexamples": "all six detected",
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
