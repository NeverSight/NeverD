#!/usr/bin/env python3
"""Verify Kotlin/D8 sparse monitor cleanup against original JVM behavior."""
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
    from scripts.test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory

PROVIDERS = {"Lfixture/Payload;"}


class MonitorClassFile(KotlinFixtureClassFile):
    SCHEMAS = {**KotlinFixtureClassFile.SCHEMAS, "Lkotlin/jvm/internal/SourceDebugExtension;": {"value": "[s"}}


def compiler_facts(root):
    result = {}
    for path in sorted(root.rglob("*.class")):
        row = MonitorClassFile(path.read_bytes()).facts()
        if row["name"] in result or path.relative_to(root).as_posix() != row["name"][1:-1] + ".class":
            raise RuntimeError("Duplicate/misplaced original compiler class")
        result[row["name"]] = row
    return result


def dex_metadata(value):
    if isinstance(value, dict):
        return {k: dex_metadata(v) for k,v in value.items() if k not in
                {"runtime_invisible_annotations", "runtime_invisible_parameter_annotations"}}
    if isinstance(value, list):
        return [dex_metadata(v) for v in value]
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--kotlin-lib", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    profiles = parser.add_mutually_exclusive_group()
    profiles.add_argument("--post-release", action="store_true")
    profiles.add_argument("--multiple-release", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-monitor-sparse-cleanup-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "monitor_regions/java"
        continuation = args.post_release or args.multiple_release
        variant = ("monitor_multiple_release" if args.multiple_release else
                   "monitor_post_release" if args.post_release else "monitor_sparse_cleanup")
        oracle = "MonitorPostRelease" if continuation else "MonitorRegion"
        harness_name = "MonitorMultipleReleaseHarness" if args.multiple_release else oracle + "Harness"
        expected = ("cases=40,ordered-failures=16,unlocked-callbacks=20" if continuation else
                    "concurrent-updates=6000,reentrant-updates=4,null-lock-failures=2,ordered-failures=4,release-checks=6")
        originals, harness = work / "originals", work / "harness"
        kotlin = args.kotlin_lib.resolve()
        jars = sorted(kotlin.glob("*.jar"))
        if not (kotlin / "kotlin-compiler.jar").is_file() or not (kotlin / "kotlin-stdlib.jar").is_file():
            raise RuntimeError("The original witness requires the unmodified Kotlin compiler")
        compiler_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in jars}
        dependencies = []
        for jar in (kotlin / "kotlin-stdlib.jar", kotlin / "annotations-13.0.jar"):
            output = work / jar.name
            with zipfile.ZipFile(jar) as source, zipfile.ZipFile(output, "x") as dest:
                for entry in source.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):
                            raise RuntimeError("Unsafe compiler dependency entry")
                        dest.writestr(entry.filename, source.read(entry))
            dependencies.append(output)
        verify.compile_local([fixture / "fixture/Payload.java"], work / "original-java", release=8)
        originals.mkdir()
        verify.run([verify.java, "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-no-stdlib", "-no-reflect", "-jvm-target", "1.8", "-classpath",
                    os.pathsep.join(map(str, (kotlin / "kotlin-stdlib.jar", work / "original-java"))),
                    "-d", originals, *sorted((FIXTURES / variant / "kotlin").rglob("*.kt"))], "kotlinc-original")
        (originals / "fixture/Payload.class").write_bytes((work / "original-java/fixture/Payload.class").read_bytes())
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              (harness_name, oracle + "Caller")], harness, classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 2 or len(all_methods) != 3:
            raise RuntimeError("Owned original monitor-region inventory changed")
        classes = all_classes - (PROVIDERS if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = dex_metadata(compiler_facts(originals)), exception_inventory(originals)

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
            dependencies.append(dependency)
        for path in dependencies:
            classpath += ["--classpath", path]
        dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness, *dependencies))),
                              harness_name], "baseline")
        if baseline.splitlines() != [expected]:
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
                or len(re.findall(r"\|[0-9a-fA-F]+:\s+monitor-exit\b", disassembly)) != (4 if args.multiple_release else 2)
                or len(re.findall(r"\|[0-9a-fA-F]+:\s+move-exception\b", disassembly)) != 1
                or "<any> ->" not in disassembly):
            raise RuntimeError("D8 changed original acquire/release/cleanup witnesses")
        # Unlike javac's continuous cleanup range, Kotlin/D8 leaves numeric
        # constants after acquisition and its normal release uncovered.
        witness = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] == "Lfixture/Gate;" and name and name[1] == "update":
                ops = [(int(pc,16),op) for pc,op in re.findall(r"\|([0-9a-fA-F]+):\s+(\S+)", block[2])]
                enter = next(i for i,(_,op) in enumerate(ops) if op == "monitor-enter")
                if ops[enter+1][1] != "const/4":
                    raise RuntimeError("Kotlin/D8 omitted the nonthrowing monitor prefix")
                # Parse the SDK's independently dumped half-open catch intervals.
                catches = re.search(r"(?ms)^\s+catches\s+: (\d+)\n(.*?)(?=^\s+positions\s+:)", block[2])
                if not catches:
                    raise RuntimeError("Missing independently dumped catch table")
                intervals = [(int(a,16),int(b,16)) for a,b in re.findall(r"(?m)^\s+0x([0-9a-fA-F]+) - 0x([0-9a-fA-F]+)$", catches[2])]
                if len(intervals) != int(catches[1]):
                    raise RuntimeError("Incomplete independently dumped catch intervals")
                exits = [pc for pc,op in ops if op == "monitor-exit"]
                if not intervals or any(a <= ops[enter+1][0] < b for a,b in intervals) or any(a <= pc < b for pc in exits[:-1] for a,b in intervals):
                    raise RuntimeError("Kotlin/D8 changed sparse cleanup boundaries")
                if continuation:
                    normal_exit = next(i for i,(_,op) in enumerate(ops) if op == "monitor-exit")
                    after = ops[normal_exit+1:]
                    if (not any(op.startswith("if-") for _,op in after)
                            or not any(op.startswith("invoke-interface") for _,op in after)
                            or not any(op == "return" for _,op in after)):
                        raise RuntimeError("D8 omitted the post-release conditional callback")
                witness = True
        if not witness:
            raise RuntimeError("Missing original sparse monitor cleanup witness")
        annotations = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        if "VISIBILITY_BUILD" in annotations or re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations) != ["Lkotlin/Metadata;"]:
            raise RuntimeError("D8 changed the owned compiler annotation retention")
        output = work / "recovered"
        # Full immutable Kotlin declarations need the bounded 600-second/40M
        # analysis profile; the default work budget also fails on the baseline.
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath,
                    "--timeout", "600", "-o", output], "recover")
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
                             classpath=os.pathsep.join(map(str, dependencies)))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        helper_owners = {"Gate"} | (set() if args.dependency_provider else {"Payload"})
        expected_helpers = {"Lfixture/" + name + ";->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for name in helper_owners}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Monitor-region recovery invented a runtime entry or unexpected helper")
        actual_records, actual_exceptions = dex_metadata(compiler_facts(rebuilt)), exception_inventory(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original monitor-region class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"][method] != metadata or actual_exceptions[method] != exceptions[method]:
                    raise RuntimeError("Original monitor-region method metadata or Exceptions changed: " + method)
        runtime = (rebuilt, harness, *dependencies)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             harness_name], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Monitor-region invocation changed identity, ordered failures or virtual dispatch")
        unsafe = work / "unsynchronized/fixture/Gate.java"
        unsafe.parent.mkdir(parents=True)
        if continuation:
            unsafe.write_text((FIXTURES / "monitor_post_release/counterexample/fixture/Gate.java").read_text())
        else:
            text = (fixture / "fixture/Gate.java").read_text()
            if text.count("synchronized (lock)") != 1:
                raise RuntimeError("Original synchronization counterexample changed")
            unsafe.write_text(text.replace("synchronized (lock)", ""))
        verify.compile_local([unsafe], work / "unsynchronized-classes", classpath=originals)
        result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (work / "unsynchronized-classes", originals, harness, *dependencies))),
                                 harness_name], text=True, capture_output=True, timeout=30, env=verify.environment)
        (work / "unsynchronized-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle failed to detect missing monitor ownership")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != h for p,h in compiler_hashes.items())
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != h for p,h in dependency_hashes.items())):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "concurrent_updates": 6000, "reentrant_updates": 4, "null_lock_failures": 2,
                    "ordered_failures": 4, "release_checks": 6, "dependency_provider": args.dependency_provider,
                    "original_dex_metadata_and_exceptions": "unchanged", "original_sparse_cleanup_intervals": "independently witnessed",
                    "monitor_ownership_and_exception_identity": "matched", "unsynchronized_counterexample": "detected",
                    "input_and_oracle_bytes": "unchanged"}
        if continuation:
            for key in ("concurrent_updates", "reentrant_updates", "null_lock_failures", "ordered_failures", "release_checks", "unsynchronized_counterexample"):
                del evidence[key]
            evidence.update(cases=40, ordered_failures=16, unlocked_callbacks=20,
                            post_release_callback="independently witnessed", locked_callback_counterexample="detected",
                            normal_release_paths=3 if args.multiple_release else 1)
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
