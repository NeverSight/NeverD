#!/usr/bin/env python3
"""Verify structured monitor regions against original JVM synchronization and failures."""
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

PROVIDERS = {"Lfixture/Payload;"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--shared-return", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-monitor-regions-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "monitor_regions/java"
        originals, harness = work / "originals", work / "harness"
        gate_fixture = (FIXTURES / "monitor_shared_return/java" if args.shared_return else fixture)
        verify.compile_local([gate_fixture / "fixture/Gate.java", fixture / "fixture/Payload.java"], originals)
        harness_name = "MonitorSharedReturnHarness" if args.shared_return else "MonitorRegionHarness"
        harnesses = ["MonitorRegionHarness", "MonitorRegionCaller"]
        if args.shared_return:
            harnesses.append(harness_name)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in harnesses],
                             harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 2 or len(all_methods) != 3:
            raise RuntimeError("Owned original monitor-region inventory changed")
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
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                              harness_name], "baseline")
        expected = ["concurrent-updates=6000,reentrant-updates=4,null-lock-failures=2,ordered-failures=4,release-checks=6"]
        if args.shared_return:
            expected.append("shared-continuations=64,branch-routes=4")
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned monitor behavior matrix changed: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned monitor-region fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original monitor-region runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "monitor-disassembly")
        if (len(re.findall(r"\|[0-9a-fA-F]+:\s+monitor-enter\b", disassembly)) != 1
                or len(re.findall(r"\|[0-9a-fA-F]+:\s+monitor-exit\b", disassembly)) != 2
                or len(re.findall(r"\|[0-9a-fA-F]+:\s+move-exception\b", disassembly)) != 1
                or "<any> ->" not in disassembly):
            raise RuntimeError("D8 changed original acquire/release/cleanup witnesses")
        if args.shared_return:
            # The independent D8 listing must place the unchanged release/throw
            # handler before an unlocked suffix reached by a forward goto.
            ops = [(int(pc, 16), op, tail) for pc, op, tail in
                   re.findall(r"\|([0-9a-fA-F]+):\s+(\S+)([^\n]*)",
                              disassembly.split("name          : 'update'", 1)[1].split("source_file_idx", 1)[0])]
            handler = next(i for i, (_, op, _) in enumerate(ops) if op == "move-exception")
            if ([op for _, op, _ in ops[handler:handler+3]] != ["move-exception", "monitor-exit", "throw"]
                    or not any(op == "return-void" for _, op, _ in ops[handler+3:])
                    or not any(op.startswith("goto") and int(re.search(r"[0-9a-fA-F]+", tail)[0], 16) > ops[handler+2][0]
                               for _, op, tail in ops[:handler])):
                raise RuntimeError("D8 omitted the shared post-cleanup continuation")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Monitor-region recovery changed runtime entries or original contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        helper_owners = {"Gate"} | (set() if args.dependency_provider else {"Payload"})
        expected_helpers = {"Lfixture/" + name + ";->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for name in helper_owners}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Monitor-region recovery invented a runtime entry or unexpected helper")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original monitor-region class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"][method] != metadata or actual_exceptions[method] != exceptions[method]:
                    raise RuntimeError("Original monitor-region method metadata or Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             harness_name], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Monitor-region invocation changed identity, ordered failures or virtual dispatch")
        unsafe = work / "unsynchronized/fixture/Gate.java"
        unsafe.parent.mkdir(parents=True)
        text = (gate_fixture / "fixture/Gate.java").read_text()
        if text.count("synchronized (lock)") != 1:
            raise RuntimeError("Original synchronization counterexample changed")
        unsafe.write_text(text.replace("synchronized (lock)", ""))
        verify.compile_local([unsafe], work / "unsynchronized-classes", classpath=originals)
        result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (work / "unsynchronized-classes", originals, harness))),
                                 harness_name], text=True, capture_output=True, timeout=30, env=verify.environment)
        (work / "unsynchronized-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle failed to detect missing monitor ownership")
        if args.shared_return:
            guard = "else if (callback != null) synchronized (lock)"
            if text.count(guard) != 1:
                raise RuntimeError("Original acquisition guard changed")
            wrong = work / "eager-acquisition/fixture/Gate.java"
            wrong.parent.mkdir(parents=True)
            wrong.write_text(text.replace(guard, "else synchronized (lock)"))
            verify.compile_local([wrong], work / "eager-acquisition-classes", classpath=originals)
            result = subprocess.run([str(verify.java), "-cp",
                                     os.pathsep.join(map(str, (work / "eager-acquisition-classes", originals, harness))),
                                     harness_name], text=True, capture_output=True, timeout=30, env=verify.environment)
            (work / "eager-acquisition-counterexample.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "AssertionError" not in result.stderr:
                raise RuntimeError("Independent oracle missed acquisition on a skipped branch")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "shared_continuation_cases": 64 if args.shared_return else 0,
                    "concurrent_updates": 6000, "reentrant_updates": 4, "null_lock_failures": 2,
                    "ordered_failures": 4, "release_checks": 6, "dependency_provider": args.dependency_provider,
                    "original_class_and_method_metadata_and_exceptions": "unchanged",
                    "monitor_ownership_and_exception_identity": "matched", "unsynchronized_counterexample": "detected",
                    "input_and_oracle_bytes": "unchanged"}
        if args.shared_return:
            evidence["eager_acquisition_counterexample"] = "detected"
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
