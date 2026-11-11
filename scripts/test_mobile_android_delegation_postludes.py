#!/usr/bin/env python3
"""Verify closed this-constructor chains with original mutable postludes."""
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
    parser.add_argument("--dependency-trace", action="store_true")
    parser.add_argument("--loop", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-delegation-postludes-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "delegation_postludes/java"
        harness_sources = [FIXTURES / "harness" / (name + ".java") for name in
                           ("DelegationPostludeCaller", "DelegationPostludeHarness")]
        if args.loop:
            generated = work / "loop-sources"
            shutil.copytree(fixture, generated)
            fixture = generated
            core = fixture / "fixture/Core.java"
            text = core.read_text().replace("        Trace.event(3);", """        Trace.event(3);
        for (int index = 0; index < 3; index++) {
            mark += 7;
            Trace.event(index + 4);
        }""").replace("        Trace.event(4);", "        Trace.event(7);")
            core.write_text(text)
            trace = fixture / "fixture/Trace.java"
            trace.write_text(trace.read_text().replace(
                "value >= 3 ? expectedMark : 0",
                "value >= 3 ? expectedMark + (value >= 7 ? 21 : (value - 3) * 7) : 0"
            ).replace("value >= 4 ? expectedStamp : 0", "value >= 7 ? expectedStamp : 0"))
            observer = work / "loop-harness/DelegationPostludeHarness.java"
            observer.parent.mkdir()
            text = harness_sources[1].read_text().replace(
                "level >= 1 ? mark : 0", "level >= 1 ? mark + 21 : 0"
            ).replace(
                "level >= 1 && (mode == 0 || mode >= 3) ? mark : 0",
                "level >= 1 && (mode == 0 || mode >= 3) ? mark + (mode == 0 || mode >= 7 ? 21 : (mode - 3) * 7) : 0"
            ).replace("mode >= 4", "mode >= 7").replace(
                "mode == 0 ? level + 2 : mode", "mode == 0 ? (level == 0 ? 2 : level == 1 ? 6 : 7) : mode")
            observer.write_text(text)
            harness_sources[1] = observer
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local(harness_sources, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 2 or len(all_methods) != 5:
            raise RuntimeError("Owned delegation-postlude inventory changed")
        classes = all_classes - ({"Lfixture/Trace;"} if args.dependency_trace else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-trace.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            jar.writestr("fixture/Trace.class", (originals / "fixture/Trace.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_trace:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            result = []
            for level in range(3):
                for route in range(4):
                    for mode in range((3 if level == 0 else 7 if level == 1 else 8) if args.loop else level + 3):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "DelegationPostludeHarness", str(level), str(route), str(mode)], f"{label}-{level}-{route}-{mode}")
                        if not re.fullmatch(f"level={level},route={route},mode={mode},trace=[0-9]+\\n", output):
                            raise RuntimeError("Original postlude matrix changed: " + output)
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
            raise RuntimeError("Owned postlude fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original delegation-postlude runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-delegation-disassembly")
        witnesses = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            kind = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not kind or kind[1] not in {"(Ljava/lang/Object;I)V", "(Ljava/lang/Object;IJ)V"}:
                continue
            wide = kind[1] == "(Ljava/lang/Object;IJ)V"
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            expected = ["invoke-direct", "iput-wide" if wide else "iput", "const/4", "invoke-static", "return-void"]
            if args.loop and not wide:
                if (ops[:4] != expected[:4] or ops[-1] != "return-void"
                        or sum(op.startswith("if-") for op in ops) != 1
                        or ops.count("goto") != 1 or ops.count("iput") != 2
                        or ops.count("invoke-static") != 2):
                    raise RuntimeError("D8 changed the original delegation loop witness")
                targets = re.findall(r"\|([0-9a-fA-F]+):\s+goto\s+([0-9a-fA-F]+)", block[2])
                if len(targets) != 1 or not 0 < int(targets[0][1], 16) < int(targets[0][0], 16):
                    raise RuntimeError("Missing original post-delegation loop backedge")
            elif ops != expected:
                raise RuntimeError("D8 changed the original leading delegation/postlude witness")
            if "Lfixture/Core;.<init>:" + ("(Ljava/lang/Object;I)V" if wide else "(Ljava/lang/Object;)V") not in block[2]:
                raise RuntimeError("D8 changed the original constructor target")
            witnesses.add(kind[1])
        if len(witnesses) != 2:
            raise RuntimeError("Missing original two-stage constructor delegation")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Postlude recovery changed original inventory or contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_trace else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Postlude recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original postlude class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original postlude method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_trace else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Final writes crossed an original effect or delegation-postlude boundary")
        # Independently compile a legal wrong order within the initialized-this
        # postlude. The frozen observer must detect the missing field value.
        wrong = work / "reordered/fixture/Core.java"
        wrong.parent.mkdir(parents=True)
        text = (fixture / "fixture/Core.java").read_text()
        marker = "        mark = mode;\n        Trace.event(3);"
        if text.count(marker) != 1:
            raise RuntimeError("Owned postlude source changed")
        wrong.write_text(text.replace(marker, "        Trace.event(3);\n        mark = mode;"))
        verify.compile_local([wrong], work / "reordered-classes", classpath=originals, release=8)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (work / "reordered-classes", originals, harness))),
                                 "DelegationPostludeHarness", "2", "1", "0"], text=True, capture_output=True,
                                timeout=300, env=verify.environment)
        (work / "reordered-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle did not detect reordered postlude effects")
        if args.loop:
            for name, before, after in (
                ("short-loop", "index < 3", "index < 2"),
                ("loop-write-order", "            mark += 7;\n            Trace.event(index + 4);",
                 "            Trace.event(index + 4);\n            mark += 7;"),
            ):
                text = (fixture / "fixture/Core.java").read_text()
                if text.count(before) != 1:
                    raise RuntimeError("Loop counterexample marker changed")
                source = work / name / "fixture/Core.java"
                source.parent.mkdir(parents=True)
                source.write_text(text.replace(before, after))
                variant = work / (name + "-classes")
                verify.compile_local([source], variant, classpath=originals, release=8)
                result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (variant, originals, harness))),
                                         "DelegationPostludeHarness", "2", "1", "0"], text=True, capture_output=True,
                                        timeout=300, env=verify.environment)
                (work / (name + "-counterexample.log")).write_text(result.stdout + result.stderr)
                if result.returncode == 0 or "AssertionError" not in result.stderr:
                    raise RuntimeError("Independent observer missed " + name)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": 72 if args.loop else 48, "dependency_trace": args.dependency_trace,
                    "loop": args.loop, "independent_compiled_counterexamples": 3 if args.loop else 1,
                    "original_metadata_and_exceptions": "unchanged", "primary_and_postlude_observations": "matched",
                    "original_exception_identity": "matched",
                    "independent_reordered_postlude_counterexample": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
