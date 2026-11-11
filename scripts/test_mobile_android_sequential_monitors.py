#!/usr/bin/env python3
"""Verify independent sequential monitor regions, unlocked callbacks and exact releases."""
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
    parser.add_argument("--loop", action="store_true")
    parser.add_argument("--outer-cleanup", action="store_true")
    parser.add_argument("--covered-capture", action="store_true",
                        help="Use authored smali with outer protection over an inner exception capture")
    parser.add_argument("--lock-aliases", action="store_true",
                        help="Use authored smali with copied lock objects and wide register reuse")
    args = parser.parse_args()
    if args.lock_aliases:
        args.covered_capture = True
    if args.covered_capture:
        args.outer_cleanup = True
        args.dependency_provider = True
    if args.loop and args.outer_cleanup:
        parser.error("--outer-cleanup uses two sequential regions without a surrounding loop")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-sequential-monitors-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "sequential_monitors/java"
        harness_sources = [FIXTURES / "harness" / (name + ".java") for name in
                           ("SequentialMonitorsHarness", "SequentialMonitorsCaller")]
        if args.outer_cleanup:
            authored = work / "outer-source"
            shutil.copytree(fixture, authored)
            core = authored / "fixture/Core.java"
            text = core.read_text()
            start = "        Trace.check(first, second, input, 1, 0);"
            end = "        return input;\n    }\n}"
            if text.count(start) != 1 or text.count(end) != 1:
                raise RuntimeError("Original outer cleanup source anchors changed")
            core.write_text(text.replace(start, "        try {\n" + start).replace(end,
                "        return input;\n        } finally {\n            Trace.check(first, second, input, 6, 0);\n        }\n    }\n}"))
            trace = authored / "fixture/Trace.java"
            text = trace.read_text()
            declaration = "    public static RuntimeException failure;"
            failure = "        if (event == failAt) throw failure;"
            if text.count(declaration) != 1 or text.count(failure) != 1:
                raise RuntimeError("Original outer cleanup observer anchors changed")
            trace.write_text(text.replace(declaration, declaration + "\n    public static Throwable cleanupFailure;")
                .replace(failure, "        if (event == 6 && cleanupFailure != null) {\n"
                    "            Throwable selected = cleanupFailure;\n"
                    "            if (selected instanceof RuntimeException) throw (RuntimeException) selected;\n"
                    "            throw (Error) selected;\n        }\n" + failure))
            fixture = authored
            oracle = work / "outer-harness"
            oracle.mkdir()
            for original in harness_sources:
                text = original.read_text()
                if original.name.endswith("Harness.java"):
                    before = "        long expectedEvents = 0;"
                    capture = "            Throwable caught = null;"
                    assertion = "            require(nullFailure ? caught instanceof NullPointerException"
                    if any(text.count(anchor) != 1 for anchor in (before, capture, assertion)):
                        raise RuntimeError("Original outer cleanup harness anchors changed")
                    text = text.replace(before, "        int cleanup = Integer.parseInt(args[3]);\n"
                        "        Trace.cleanupFailure = cleanup == 1 ? new IllegalArgumentException(\"owned cleanup\")\n"
                        "            : cleanup == 2 ? new LinkageError(\"owned cleanup\") : null;\n" + before)
                    text = text.replace(capture, "            expectedEvents = expectedEvents * 10 + 6;\n" + capture)
                    text = text.replace(assertion, "            require(Trace.cleanupFailure != null ? caught == Trace.cleanupFailure\n"
                        "                : nullFailure ? caught instanceof NullPointerException")
                (oracle / original.name).open("x").write(text)
            harness_sources = sorted(oracle.glob("*.java"))
        if args.loop:
            authored = work / "loop-source"
            shutil.copytree(fixture, authored)
            core = authored / "fixture/Core.java"
            text = core.read_text()
            start = "        Trace.check(first, second, input, 1, 0);"
            end = "        return input;\n    }\n}"
            if text.count(start) != 1 or text.count(end) != 1:
                raise RuntimeError("Original loop source anchors changed")
            core.write_text(text.replace(start, "        while (Trace.rounds-- > 0) {\n" + start)
                            .replace(end, "        }\n" + end))
            trace = authored / "fixture/Trace.java"
            text = trace.read_text()
            if text.count("failAt, baseMask;") != 1:
                raise RuntimeError("Original loop counter anchor changed")
            trace.write_text(text.replace("failAt, baseMask;", "failAt, baseMask, rounds;"))
            fixture = authored
            oracle = work / "loop-harness"
            oracle.mkdir()
            for original in harness_sources:
                text = original.read_text()
                if original.name.endswith("Harness.java"):
                    text = text.replace("for (int attempt = 0; attempt < 2; attempt++) {",
                        "for (int attempt = 0; attempt < 2; attempt++) { Trace.rounds = 2;")
                    text = text.replace("for (int event = 1; event <= last; event++) {",
                        "expected: for (int round = 0; round < (Trace.early || Trace.skip ? 1 : 2); round++) {\n" +
                        "            for (int event = 1; event <= last; event++) {")
                    text = text.replace("                    break;", "                    break expected;")
                    text = text.replace("expectedFailure = true; break;", "expectedFailure = true; break expected;")
                    text = text.replace("            Throwable caught = null;", "            }\n            Throwable caught = null;")
                (oracle / original.name).write_text(text)
            harness_sources = sorted(oracle.glob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local(harness_sources, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 2 or len(all_methods) != 4:
            raise RuntimeError("Owned sequential monitor inventory changed")
        classes = all_classes - ({"Lfixture/Trace;"} if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x") as archive:
            archive.writestr("fixture/Trace.class", (originals / "fixture/Trace.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            outputs = []
            cases = [(route, mode, mask, 0) for route in range(4) for mode in range(13) for mask in range(4)]
            if args.outer_cleanup:
                cases += [(route, mode, mask, cleanup) for route in (0, 3) for mode in (0, 5, 7, 9, 10, 11)
                          for mask in (0, 3) for cleanup in (1, 2)]
            for route, mode, mask, cleanup in cases:
                output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                    "SequentialMonitorsHarness", str(route), str(mode), str(mask),
                    *([str(cleanup)] if args.outer_cleanup else [])],
                    f"{label}-{route}-{mode}-{mask}-{cleanup}")
                expected = f"route={route},mode={mode},mask={mask},attempt=[01],trace=-?[0-9]+"
                if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                    raise RuntimeError("Original sequential monitor matrix changed: " + output)
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
            raise RuntimeError("Owned sequential monitor fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original sequential monitor runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-array-read-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] != "run":
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            if ops.count("monitor-enter") != 2 or ops.count("monitor-exit") != 5:
                raise RuntimeError("D8 lost original independent acquisitions and releases")
            if args.outer_cleanup:
                enters = [int(pc, 16) for pc in re.findall(r"\|([0-9a-fA-F]+):\s+monitor-enter", block[2])]
                ranges = re.findall(r"0x([0-9a-f]+) - 0x([0-9a-f]+)\s+<any> -> 0x([0-9a-f]+)", block[2])
                targets = [{int(handler, 16) for lo, hi, handler in ranges if int(lo, 16) <= pc < int(hi, 16)}
                           for pc in enters]
                if len(targets) != 2 or len(targets[0]) != 1 or targets[0] != targets[1]:
                    raise RuntimeError("D8 lost the common outer handler covering both acquisitions")
                (work / "outer-monitor-witness.json").open("x").write(json.dumps({
                    "acquisition_pcs": enters, "outer_handler_pcs": sorted(targets[0]),
                    "input_method_bytecode": "unchanged javac and D8 output"}, indent=2) + "\n")
            if args.loop:
                rows = re.findall(r"\|([0-9a-fA-F]+):\s+(\S+)([^\n]*)", block[2])
                backwards = [int(pc, 16) for pc, op, tail in rows if op.startswith("goto")
                             for target in re.findall(r"\b([0-9a-fA-F]{4,})\s+//", tail)
                             if int(target, 16) < int(pc, 16)]
                if not backwards:
                    raise RuntimeError("D8 lost original released continuation loop")
            witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original sequential monitor witness")
        output = work / "recovered"
        source_input = (FIXTURES / "sequential_monitors/smali/AliasCore.smali" if args.lock_aliases else
                        FIXTURES / "sequential_monitors/smali/Core.smali" if args.covered_capture else source_dex)
        source_hash = hashlib.sha256(source_input.read_bytes()).hexdigest()
        (work / "input-method-witness.json").open("x").write(json.dumps({
            "input_method_bytecode": ("authored smali with copied lock objects and wide register reuse" if args.lock_aliases else
                                      "authored smali with covered exception capture" if args.covered_capture else "unchanged javac and D8 output"),
            "sha256": source_hash}, indent=2) + "\n")
        verify.run([args.neverd.resolve(), "mobile", source_input, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count", 0)
                or coverage["status"] != "recovered" or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Sequential monitor recovery changed original inventory or contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=8, classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Sequential monitor recovery invented or omitted a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original monitor class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after["methods"].get(identity) != metadata or actual_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Original monitor method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Sequential monitor changed values, ordered effects, aliases or failure identity")
        text = (fixture / "fixture/Core.java").read_text()
        mutations = (
            ("missing-first", "synchronized (first) {", "{", "0", "0"),
            ("missing-second", "synchronized (second) {", "{", "0", "0"),
            ("eager-second", "if (Trace.skip) return input;", "if (Trace.skip) { synchronized ((Object) null) { } return input; }", "2", "0"),
            ("extended-first", "        Trace.check(first, second, input, 3, 0);", "        synchronized (first) { Trace.check(first, second, input, 3, 0); }", "0", "0"),
            ("changed-second", "synchronized (second) {", "synchronized (first) {", "0", "0"),
        )
        if args.loop:
            mutations += (("missing-iteration", "while (Trace.rounds-- > 0)", "while (Trace.rounds-- > 1)", "0", "0"),)
        if args.outer_cleanup:
            cleanup = "Trace.check(first, second, input, 6, 0);"
            mutations += (("lost-outer-cleanup", cleanup, "", "0", "0"),
                          ("cleanup-while-held", cleanup, "synchronized (first) { " + cleanup + " }", "0", "0"),
                          ("swallowed-cleanup", cleanup, "try { " + cleanup + " } catch (Throwable ignored) {}", "0", "0", "1"))
        for label, old, new, mode, mask, *cleanup in mutations:
            if old not in text:
                raise RuntimeError("Independent sequential monitor counterexample anchor changed")
            wrong = work / label / "fixture/Core.java"
            wrong.parent.mkdir(parents=True)
            wrong.write_text(text.replace(old, new))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                    "SequentialMonitorsHarness", "1", mode, mask,
                                    *([cleanup[0] if cleanup else "0"] if args.outer_cleanup else [])], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned sequential monitor contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(source_input.read_bytes()).hexdigest() != source_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_provider": args.dependency_provider, "loop": args.loop,
                    "outer_cleanup": args.outer_cleanup, "covered_capture": args.covered_capture,
                     "lock_aliases": args.lock_aliases,
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "original_inventory_metadata_and_exceptions": "unchanged",
                    "independent_locks_reentrancy_unlocked_callbacks_and_early_exits": "matched",
                    "null_acquisition_and_original_failure_identity": "matched",
                    "independent_compiler_counterexamples": len(mutations), "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
