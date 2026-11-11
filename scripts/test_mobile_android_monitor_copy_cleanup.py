#!/usr/bin/env python3
"""Verify split monitor cleanup copies against frozen concurrent JVM observers."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
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
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--captured-exception-copies", action="store_true")
    parser.add_argument("--backward-cleanup", action="store_true")
    parser.add_argument("--release-only-handlers", action="store_true")
    parser.add_argument("--release-exception-copies", action="store_true")
    parser.add_argument("--wide-copies", action="store_true")
    parser.add_argument("--interleaved-handlers", action="store_true")
    parser.add_argument("--nested-cleanup", action="store_true")
    parser.add_argument("--outer-cleanup", action="store_true")
    parser.add_argument("--arithmetic-gaps", action="store_true")
    parser.add_argument("--unlocked-loops", action="store_true")
    args = parser.parse_args()
    if args.release_exception_copies and not args.release_only_handlers:
        parser.error("release exception copies require the release-only handler layout")
    if args.release_only_handlers and any((args.backward_cleanup, args.captured_exception_copies,
            args.wide_copies, args.interleaved_handlers, args.nested_cleanup,
            args.outer_cleanup, args.arithmetic_gaps, args.unlocked_loops)):
        parser.error("release-only handlers use their own independently authored layout")
    if args.backward_cleanup and any((args.captured_exception_copies, args.wide_copies,
            args.interleaved_handlers, args.nested_cleanup, args.outer_cleanup,
            args.arithmetic_gaps, args.unlocked_loops)):
        parser.error("backward cleanup uses its own independently authored layout")
    if args.captured_exception_copies and any((args.wide_copies, args.interleaved_handlers,
            args.nested_cleanup, args.outer_cleanup, args.arithmetic_gaps, args.unlocked_loops)):
        parser.error("captured exception cleanup uses its own independently authored layout")
    if args.outer_cleanup:
        args.nested_cleanup = True
    if args.nested_cleanup and (args.wide_copies or args.interleaved_handlers):
        parser.error("nested cleanup uses its own independently authored layout")
    if args.nested_cleanup and args.arithmetic_gaps:
        parser.error("arithmetic gaps use the split cleanup layout")
    if args.unlocked_loops and (args.nested_cleanup or args.arithmetic_gaps or args.wide_copies or args.interleaved_handlers):
        parser.error("unlocked loops use their own independently authored layout")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-monitor-copy-cleanup-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / ("monitor_captured_cleanup" if args.captured_exception_copies else
                              "monitor_unlocked_loops" if args.unlocked_loops else
                              "monitor_outer_cleanup" if args.outer_cleanup else
                              "monitor_nested_cleanup" if args.nested_cleanup else "monitor_copy_cleanup")
        gate_source = (fixture / "java/fixture/Gate.java" if args.nested_cleanup or args.unlocked_loops else
                       FIXTURES / "monitor_regions/java/fixture/Gate.java")
        originals, harness = work / "originals", work / "harness"
        inputs = [gate_source, FIXTURES / "monitor_copy_cleanup/java/fixture/Payload.java"]
        if args.nested_cleanup:
            inputs.append(fixture / "java/fixture/Cleanup.java")
        verify.compile_local(inputs, originals, release=8)
        observers = [FIXTURES / "harness" / (name + ".java") for name in
            ("MonitorRegionHarness", "MonitorRegionCaller", "MonitorCopyCleanupHarness",
             *(["UnlockedMonitorLoopsHarness"] if args.unlocked_loops else
               ["OuterMonitorCleanupHarness"] if args.outer_cleanup else
               ["NestedMonitorCleanupHarness"] if args.nested_cleanup else []))]
        if args.outer_cleanup:
            # Freeze the same concurrency/ownership oracle with a bounded
            # deadline that tolerates busy shared compiler hosts.
            copied = []
            for original in observers:
                if original.name == "MonitorRegionHarness.java":
                    text = original.read_text().replace("join(5000)", "join(30000)").replace("join(2000)", "join(30000)")
                    target = work / original.name
                    target.open("x").write(text)
                    copied.append(target)
                else:
                    copied.append(original)
            observers = copied
        verify.compile_local(observers, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(
            {"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != (3 if args.nested_cleanup else 2) or len(all_methods) != (5 if args.nested_cleanup else 3):
            raise RuntimeError("Original split monitor inventory changed")
        classes = all_classes - ({"Lfixture/Payload;"} if args.dependency_provider else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            jar.writestr("fixture/Payload.class", (originals / "fixture/Payload.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]
        expected = ("concurrent-updates=6000,reentrant-updates=4,null-lock-failures=2,ordered-failures=4,release-checks=6",
                    "split-cleanup-failures=40,exception-and-error-identity=40,reentrant-release-checks=4")
        if args.nested_cleanup:
            expected += (("nested-cleanup-cases=1280,nested-reentrant-cases=4,outer-acquisition-cases=128,outer-normal-cases=8"
                          if args.outer_cleanup else "nested-cleanup-cases=640,nested-reentrant-cases=4"),)
        if args.unlocked_loops:
            expected += ("unlocked-loop-cases=1024,unlocked-null-acquisitions=16,unlocked-null-callbacks=64,unlocked-reentrant-cases=4",)
        harness_name = ("UnlockedMonitorLoopsHarness" if args.unlocked_loops else
                        "OuterMonitorCleanupHarness" if args.outer_cleanup else
                        "NestedMonitorCleanupHarness" if args.nested_cleanup else "MonitorCopyCleanupHarness")

        def execute(runtime, label):
            output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                harness_name], label)
            if tuple(output.splitlines()) != expected:
                raise RuntimeError("Original split monitor behavior matrix changed: " + output)
            return output

        (work / "baseline-cli-sha256.txt").open("x").write(
            hashlib.sha256(args.neverd.resolve().read_bytes()).hexdigest() + "\n")
        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline) + "\n")
        text = gate_source.read_text()
        ordered = "if (callback != null) callback.run();\n            int previous = count;\n            count = previous + 1;"
        mutations = (
            ("unsynchronized", "synchronized (lock)", ""),
            ("changed-lock", "synchronized (lock)", "synchronized (this)"),
            ("early-counter", ordered, "count++;\n            if (callback != null) callback.run();"),
            ("changed-exception", "if (failure != null) throw failure;", "if (failure != null) throw new IllegalStateException(failure);"),
            ("duplicate-callback", "if (callback != null) callback.run();", "if (callback != null) { callback.run(); callback.run(); }"))
        if args.nested_cleanup:
            inner_indent = "                " if args.outer_cleanup else "                    "
            mutations += (
                ("lost-outer-finally", "finally { Cleanup.hit(4); }", "finally {}"),
                ("repeated-inner-finally", "finally { Cleanup.hit(3); }", "finally { Cleanup.hit(3); Cleanup.hit(3); }"),
                ("skipped-error-cleanup", "catch (Throwable original)", "catch (RuntimeException original)"),
                ("reversed-inner-finally", "Cleanup.hit(2); }\n" + inner_indent + "finally { Cleanup.hit(3); }",
                 "Cleanup.hit(3); }\n" + inner_indent + "finally { Cleanup.hit(2); }"),
            )
        if args.outer_cleanup:
            mutations += (("late-restored-reference", "restored = saved;", "restored = Cleanup.seed;"),
                          ("lost-restored-count", "restoredCount = count;", "restoredCount = 73;"),
                          ("skipped-null-cleanup", "catch (Throwable original) {", "catch (Throwable original) { if(lock==null) throw original;"))
        if args.unlocked_loops:
            mutations += (
                ("lost-leading-loop", "int before = leadSteps;", "int before = 0;"),
                ("lost-trailing-loop", "int after = tailSteps;", "int after = 0;"),
                ("late-trailing-count", "        }\n        while (after-- > 0)",
                 "        }\n        after = tailSteps;\n        while (after-- > 0)"),
                ("early-trailing-count", "if (callback != null) { callback.run(); }\n            tailCount++;",
                 "tailCount++;\n            if (callback != null) { callback.run(); }"),
                ("locked-leading-loop", "if (callback != null) { callback.run(); }\n            leadCount++;",
                 "if (callback != null) { synchronized (lock) { callback.run(); } }\n            leadCount++;"),
                ("locked-trailing-loop", "        }\n        while (after-- > 0) {\n            if (callback != null) { callback.run(); }\n            tailCount++;\n        }",
                 "        while (after-- > 0) {\n            if (callback != null) { callback.run(); }\n            tailCount++;\n        }\n        }"),
            )
        for label, before, after in mutations:
            if text.count(before) != 1:
                raise RuntimeError("Original monitor counterexample anchor changed: " + label)
            source = work / label / "Gate.java"
            source.parent.mkdir()
            source.open("x").write(text.replace(before, after, 1))
            wrong = work / (label + "-classes")
            verify.compile_local([source], wrong, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness))),
                harness_name], text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or not any(marker in result.stderr for marker in
                    ("monitor region contract", "owned monitor copy cleanup", "owned nested monitor cleanup", "owned outer monitor cleanup", "owned unlocked monitor loops")):
                raise RuntimeError("Independent observer missed incorrect synchronization: " + label)
        dex = work / "reference-dex"
        dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8-reference")
        source_dex = dex / "classes.dex"
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-reference-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed a reference split monitor entry")
        (work / "sdk-reference-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        # Independently authored input, not a rewritten javac or D8 artifact.
        # Its exception adapters copy dead locals before one shared rethrow.
        source_input = work / "input"
        source_input.mkdir()
        smali = (fixture / "smali/Gate.smali").read_text()
        if args.arithmetic_gaps:
            marker = "    add-int/lit8 v0, v0, 1\n"
            tail = "    .catchall {:third_start .. :third_end} :third_failure"
            if smali.count(marker) != 1 or smali.count(tail) != 1:
                raise RuntimeError("Original arithmetic-gap anchors changed")
            smali = smali.replace(marker, "    :counter_loaded\n    const/4 v9, 1\n"
                "    xor-int/2addr v0, v9\n    xor-int/2addr v0, v9\n" + marker + "    :counter_ready\n", 1)
            smali = smali.replace(tail, "    .catchall {:third_start .. :counter_loaded} :third_failure\n"
                "    .catchall {:counter_ready .. :third_end} :third_failure", 1)
        if args.wide_copies:
            if smali.count("    move v12, v0") != 3:
                raise RuntimeError("Original cleanup copy anchors changed")
            smali = smali.replace("    move v12, v0", "    move-wide v9, v7")
            smali = smali.replace("    move-object v3, p0", "    move-wide/from16 v9, v7\n    move-object v3, p0", 1)
        if args.interleaved_handlers:
            # Author the alternate physical block layout before recovery.
            # Normal edges skip both early handlers; exceptional entries
            # retain the same captured object and shared release tail.
            for ordinal in ("first", "second"):
                begin = smali.index("    :" + ordinal + "_failure\n")
                finish = smali.index("    goto :cleanup\n", begin) + len("    goto :cleanup\n")
                handler = smali[begin:finish]
                smali = smali[:begin] + smali[finish:]
                marker = "    :" + ordinal + "_end\n"
                if smali.count(marker) != 1:
                    raise RuntimeError("Original interleaved handler layout anchor changed")
                after = "after_" + ordinal + "_cleanup"
                smali = smali.replace(marker, marker + "    goto :" + after + "\n" + handler + "    :" + after + "\n", 1)
        if args.backward_cleanup:
            adapter = ("    :third_failure\n    move-exception v2\n"
                       "    move-object v11, v6\n    move v12, v0\n")
            release = "    :cleanup\n    monitor-exit p1\n    throw v2\n"
            if smali.count(adapter + release) != 1:
                raise RuntimeError("Original shared release layout anchor changed")
            shared = "    :cleanup\n    monitor-exit p1\n    :cleanup_released\n    throw v2\n"
            backward = adapter.replace("    move-object v11, v6", "    :third_adapter\n    move-object v11, p0")
            smali = smali.replace(adapter + release, shared + backward + "    goto :cleanup\n", 1)
            smali = smali.replace(".catchall {:third_start .. :third_end}",
                                  ".catchall {:third_start .. :cleanup_released}", 1)
        if args.release_only_handlers:
            normal = "    :third_end\n    monitor-exit p1\n    return-void"
            exceptional = "    :cleanup\n    monitor-exit p1\n    throw v2\n"
            if smali.count(normal) != 1 or smali.count(exceptional) != 1:
                raise RuntimeError("Original release-only handler anchors changed")
            smali = smali.replace(normal, "    :third_end\n    monitor-exit p1\n    :normal_released\n    return-void", 1)
            smali = smali.replace(exceptional,
                "    :cleanup\n    monitor-exit p1\n    :cleanup_released\n    throw v2\n    :cleanup_thrown\n"
                "    :outer_failure\n    move-exception v2\n    throw v2\n"
                "    :release_failure\n    move-exception v2\n    goto :cleanup\n", 1)
            end = smali.rfind(".end method")
            smali = smali[:end] + (
                "    .catchall {:third_end .. :normal_released} :release_failure\n"
                "    .catchall {:cleanup .. :cleanup_thrown} :outer_failure\n") + smali[end:]
        if args.release_exception_copies:
            for label in ("first_failure", "second_failure", "third_failure", "release_failure"):
                capture = "    :" + label + "\n    move-exception v2\n"
                if smali.count(capture) != 1:
                    raise RuntimeError("Original copied release capture anchor changed")
                smali = smali.replace(capture, capture + "    move-object v9, v2\n", 1)
            rethrow = "    :cleanup_released\n    throw v2"
            if smali.count(rethrow) != 1:
                raise RuntimeError("Original copied release rethrow anchor changed")
            smali = smali.replace(rethrow, "    :cleanup_released\n    throw v9", 1)
        (source_input / "Gate.smali").open("x").write(smali)
        if not args.dependency_provider:
            (source_input / "Payload.smali").open("x").write((FIXTURES / "monitor_copy_cleanup/smali/Payload.smali").read_text())
        if args.nested_cleanup:
            (source_input / "Cleanup.smali").open("x").write((fixture / "smali/Cleanup.smali").read_text())
        input_hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in source_input.iterdir()}
        (work / "split-cleanup-witness.json").open("x").write(json.dumps({
            "input_method_bytecode": "independently authored immutable smali; not executed as a JVM classfile",
            "catch_all_entries": 8 if args.outer_cleanup else 7 if args.nested_cleanup else 5 if args.release_only_handlers else 3, "shared_exceptional_release": 1, "same_captured_exception": True,
            "unprotected_reference_copies": True, "wide_cleanup_copies": args.wide_copies,
            "interleaved_catch_all_blocks": args.interleaved_handlers,
            "backward_total_copy_cleanup": args.backward_cleanup,
            "release_only_exception_entries": args.release_only_handlers,
            "copied_release_exception": args.release_exception_copies,
            "outer_handler_spans_release_and_rethrow": args.release_only_handlers,
            "nested_exception_cleanup": args.nested_cleanup,
            "total_arithmetic_outside_exception_ranges": args.arithmetic_gaps,
            "ordinary_unlocked_loops": args.unlocked_loops,
            "input_hashes": input_hashes,
            **({"captured_exception_register_copies": True, "backward_shared_release": True}
               if args.captured_exception_copies else {})}, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_input, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Split monitor recovery changed original entries or metadata contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_provider else None, release=8)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Split monitor recovery invented or omitted an original entry")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for c in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[c].get(key) != after[c].get(key):
                    raise RuntimeError("Original split monitor class metadata changed: " + c + ":" + key)
            for identity, metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original split monitor method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt-execution") != baseline:
            raise RuntimeError("Split monitor recovery changed locking, ordering or failure identity")
        controls = []
        control_mutations = ()
        if args.release_only_handlers:
            release_capture = ("    :release_failure\n    move-exception v2\n" +
                               ("    move-object v9, v2\n" if args.release_exception_copies else ""))
            release_throw = "v9" if args.release_exception_copies else "v2"
            control_mutations = (
                ("normal-entry-to-release-handler", "    :normal_released\n    return-void",
                 "    :normal_released\n    goto :release_failure"),
                ("changed-release-handler-lock", "    :cleanup\n    monitor-exit p1\n    :cleanup_released",
                 "    :cleanup\n    monitor-exit p0\n    :cleanup_released"),
                ("release-handler-skips-lock", release_capture + "    goto :cleanup",
                 release_capture + "    throw " + release_throw),
                ("release-handler-recaptures-normally", release_capture + "    goto :cleanup",
                 release_capture + "    goto :release_failure"),
            )
        elif args.captured_exception_copies:
            control_mutations = (
                ("normal-entry-to-handler", "    :third_end\n    monitor-exit p1\n    return-void",
                 "    :third_end\n    monitor-exit p1\n    goto :first_failure"),
                ("cleanup-bypasses-release", "    move-object v10, v2\n    goto :cleanup\n    .catchall",
                 "    move-object v10, v2\n    goto :normal_return\n    .catchall"),
                ("changed-cleanup-lock", "    :cleanup\n    monitor-exit p1\n    :cleanup_released",
                 "    :cleanup\n    monitor-exit p0\n    :cleanup_released"),
            )
        elif args.backward_cleanup:
            control_mutations = (
                ("cleanup-cycle", "    goto :cleanup\n    .catchall {:first_start",
                 "    goto :third_adapter\n    .catchall {:first_start"),
                ("normal-entry-to-handler", "    :third_end\n    monitor-exit p1\n    return-void",
                 "    :third_end\n    monitor-exit p1\n    goto :first_failure"),
                ("cleanup-bypasses-release", "    goto :cleanup\n    .catchall {:first_start",
                 "    goto :normal_return\n    .catchall {:first_start"),
                ("changed-cleanup-lock", "    :cleanup\n    monitor-exit p1\n    :cleanup_released",
                 "    :cleanup\n    monitor-exit p0\n    :cleanup_released"),
            )
        elif args.interleaved_handlers:
            control_mutations = (
                ("normal-entry-to-handler", "    goto :after_first_cleanup\n    :first_failure",
                 "    goto :first_failure\n    :first_failure"),
                ("cleanup-bypasses-release", "    goto :cleanup\n    :after_first_cleanup",
                 "    goto :normal_return\n    :after_first_cleanup"),
                ("changed-cleanup-lock", "    :cleanup\n    monitor-exit p1\n    throw v2",
                 "    :cleanup\n    monitor-exit v11\n    throw v2"),
            )
        elif args.unlocked_loops:
            control_mutations = (
                ("leading-loop-enters-locked-body", "    goto :lead_loop", "    goto :first_start"),
                ("trailing-loop-enters-release", "    goto :tail_loop", "    goto :third_end"),
                ("trailing-loop-enters-handler", "    goto :tail_loop", "    goto :second_failure"),
            )
        elif args.arithmetic_gaps:
            marker = "    :counter_loaded\n    const/4 v9, 1\n    xor-int/2addr v0, v9"
            control_mutations = (
                ("uncovered-integer-division", marker,
                 "    :counter_loaded\n    const/4 v9, 0\n    div-int/2addr v0, v9"),
                ("uncovered-string-resolution", marker,
                 "    :counter_loaded\n    const-string v10, \"owned uncovered constant\"\n    const/4 v9, 1\n    xor-int/2addr v0, v9"),
                ("unsupported-gap-opcode", marker,
                 "    :counter_loaded\n    const/4 v9, 1\n    unsupported-owned-gap v0, v9"),
            )
        elif args.outer_cleanup:
            control_mutations = (
                ("uncovered-locked-cleanup", "    .catchall {:locked_cleanup_start .. :locked_cleanup_end} :release_failure", ""),
                ("unacquired-cleanup-entry", "    .catchall {:acquire_start .. :acquire_end} :outer_failure",
                 "    .catchall {:acquire_start .. :acquire_end} :release_failure"),
                ("changed-final-release-lock", "    :release_failure\n    move-exception v2\n    monitor-exit p1",
                 "    :release_failure\n    move-exception v2\n    monitor-exit p0"),
            )
        elif args.nested_cleanup:
            control_mutations = (
                ("uncovered-finally-failure", "    .catchall {:abandon_failure_start .. :abandon_failure_end} :release_failure", ""),
                ("early-exceptional-release", "    :body_failure\n    move-exception v2",
                 "    :body_failure\n    move-exception v2\n    monitor-exit p1"),
                ("changed-final-exception", "    :release_failure\n    move-exception v2\n    monitor-exit p1\n    throw v2",
                 "    :release_failure\n    move-exception v2\n    monitor-exit p1\n    throw v1"),
            )
        if control_mutations:
            for label, before, after in control_mutations:
                if smali.count(before) != 1:
                    raise RuntimeError("Owned monitor control anchor changed: " + label)
                negative = work / (label + "-input")
                negative.mkdir()
                changed = smali.replace(before, after, 1)
                if label == "cleanup-bypasses-release":
                    marker = "    :third_end\n    monitor-exit p1\n    return-void"
                    if changed.count(marker) != 1:
                        raise RuntimeError("Owned unlocked return anchor changed")
                    changed = changed.replace(marker, "    :third_end\n    monitor-exit p1\n    :normal_return\n    return-void", 1)
                (negative / "Gate.smali").open("x").write(changed)
                for original in source_input.iterdir():
                    if original.name != "Gate.smali":
                        (negative / original.name).open("xb").write(original.read_bytes())
                rejected = work / (label + "-rejected")
                result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative),
                    *map(str, classpath), "-o", str(rejected)], text=True, capture_output=True,
                    timeout=600, env=verify.environment)
                (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
                expected_error = ("undefined or incompatible register" if label == "changed-final-exception" else
                                  "unsupported" if label == "unsupported-gap-opcode" else "monitor")
                if result.returncode == 0 or rejected.exists() or expected_error not in result.stderr:
                    raise RuntimeError("Unproved monitor ownership was accepted: " + label)
                controls.append(label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in source_input.iterdir()} != input_hashes):
            raise RuntimeError("An original input, frozen observer or SDK declaration changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
            "concurrent_updates": 6000, "reentrant_updates": 4, "additional_reentrant_release_checks": 4, "null_lock_failures": 2,
            "split_cleanup_failures": 40, "release_checks": 50, "independent_wrong_programs": len(mutations),
            "wide_copies": args.wide_copies, "dependency_provider": args.dependency_provider,
            "interleaved_handlers": args.interleaved_handlers, "ownership_counterexamples": controls,
            "backward_cleanup": args.backward_cleanup,
            "release_only_handlers": args.release_only_handlers,
            "release_exception_copies": args.release_exception_copies,
            "nested_cleanup": args.nested_cleanup, "nested_cleanup_cases": 1280 if args.outer_cleanup else 640 if args.nested_cleanup else 0,
            "outer_cleanup": args.outer_cleanup,
            "arithmetic_gaps": args.arithmetic_gaps,
            "unlocked_loops": args.unlocked_loops,
            "unlocked_loop_cases": 1024 if args.unlocked_loops else 0,
            "unlocked_null_acquisitions": 16 if args.unlocked_loops else 0,
            "unlocked_null_callbacks": 64 if args.unlocked_loops else 0,
            "unlocked_reentrant_cases": 4 if args.unlocked_loops else 0,
            "original_class_and_method_metadata_and_exceptions": "unchanged",
            "monitor_ownership_partial_objects_exception_and_error_identity": "matched",
            "input_and_frozen_oracle_bytes": "unchanged",
            **({"captured_exception_copies": True, "backward_shared_release": True}
               if args.captured_exception_copies else {})}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
