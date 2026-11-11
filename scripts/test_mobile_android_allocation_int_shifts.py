#!/usr/bin/env python3
"""Verify int shifts and masked distances in original allocation arguments."""
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
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-target", action="store_true")
    parser.add_argument("--fields", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-int-shifts-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "allocation_int_shifts/java"
        harness_sources = sorted((FIXTURES / "allocation_int_shifts/harness").glob("*.java"))
        if args.fields:
            authored = work / "field-source"
            shutil.copytree(fixture, authored)
            core = authored / "fixture/Core.java"
            text = core.read_text()
            for value in ("x", "y"):
                text = text.replace(value + " <<", "Trace." + value + " <<").replace(value + " >>", "Trace." + value + " >>")
            core.write_text(text)
            fixture = authored
            oracle = work / "field-harness"
            oracle.mkdir()
            for original in harness_sources:
                text = original.read_text()
                if original.name.endswith("Harness.java"):
                    text = text.replace("require(Trace.failAt == 0 && result != previous",
                        "int actualX=attempt==1?99:x, actualY=attempt==1?-71:y;\n                require(Trace.failAt == 0 && result != previous")
                    text = text.replace("shift(x,", "shift(actualX,").replace("shift(y,", "shift(actualY,")
                (oracle / original.name).open("x").write(text)
            harness_sources = sorted(oracle.glob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local(harness_sources, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != 7:
            raise RuntimeError("Owned integer-arithmetic inventory changed")
        providers = {"Lfixture/Target;"} if args.dependency_target else set()
        classes = all_classes - providers
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if providers:
            classpath += ["--classpath", dependency]
        def execute(runtime, label):
            result = []
            for variant in range(8):
                for kind in range(2):
                    for route in range(4):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "AllocationIntegerArithmeticHarness", str(variant), str(kind), str(route)],
                            f"{label}-{variant}-{kind}-{route}")
                        if not re.fullmatch(f"variant={variant},kind={kind},route={route},trace=[0-9]+\n", output):
                            raise RuntimeError("Owned integer argument observer changed")
                        result.append(output)
            return result
        baseline = execute((originals, harness), "baseline")
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        encoded = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [encoded]:
            raise RuntimeError("Owned integer arithmetic argument DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", encoded], "sdk-inventory")
        inventory = parse_dexdump(dump, encoded.name, dex_header(encoded.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed original integer arithmetic argument runtime entries")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", encoded], "original-array-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] not in ("make", "bias"):
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            if (ops[0] != "new-instance" or ops.count("new-instance") != 1
                    or ops[-2].split("/")[0] != "invoke-direct" or ops[-1] != "return-object"):
                raise RuntimeError("D8 changed original arithmetic/allocation witness")
            arithmetic = [op.split("/", 1)[0] for op in ops]
            if not all(op in arithmetic for op in ("shl-int", "shr-int", "ushr-int")):
                raise RuntimeError("D8 lost original typed int shift witnesses")
            if args.fields and ops.count("sget") != 4:
                raise RuntimeError("D8 lost original ordered field shift inputs")
            if name[1] == "bias" and not all(op+"/lit8" in ops for op in ("shl-int", "shr-int", "ushr-int")):
                raise RuntimeError("D8 lost original literal shift witnesses")
            witnessed.add(name[1])
        if witnessed != {"make", "bias"}:
            raise RuntimeError("Missing original integer arithmetic witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", encoded, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Integer arithmetic recovery changed original contracts or release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=8,
                             classpath=dependency if providers else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Integer arithmetic recovery invented or lost runtime entries")
        actual, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original integer arithmetic argument class metadata changed: " + owner + ":" + key)
            for identity, row in before["methods"].items():
                if after["methods"].get(identity) != row or actual_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original integer arithmetic argument method metadata/Exceptions changed")
        runtime = (rebuilt, harness, dependency) if providers else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Integer arithmetic argument, identity, failure or class-init order changed")
        original = (fixture / "fixture/Core.java").read_text()
        marker = "new Target(x << distance, y >> distance, x >>> distance, y << distance)"
        bias = "new Target(x << 13, y >> -1, x >>> 32, y << 33)"
        if args.fields:
            for value in ("x", "y"):
                marker = marker.replace(value + " <<", "Trace." + value + " <<").replace(value + " >>", "Trace." + value + " >>")
                bias = bias.replace(value + " <<", "Trace." + value + " <<").replace(value + " >>", "Trace." + value + " >>")
        x = "Trace.x" if args.fields else "x"
        y = "Trace.y" if args.fields else "y"
        mutants = {
            "logical-for-arithmetic": (marker, marker.replace(y+" >> distance", y+" >>> distance"), 4, 0),
            "arithmetic-for-logical": (marker, marker.replace(x+" >>> distance", x+" >> distance"), 0, 0),
            "wrong-mask-width": (marker, marker.replace(x+" << distance", "(int)((long)"+x+" << (distance & 63))").replace(x+" >>> distance", "(int)(("+x+" & 0xffffffffL) >>> (distance & 63))"), 1, 0),
            "unmasked-large-distance": (marker, marker.replace(x+" << distance", "distance<0?0:"+x+" << distance"), 1, 0),
            "literal-sign": (bias, bias.replace(">> -1", ">> 1"), 4, 1),
            "lost-literal-mask": (bias, bias.replace(">>> 32", ">>> 31"), 4, 1),
            "changed-left-distance": (bias, bias.replace("<< 33", "<< 32"), 4, 1),
            "wrong-input-identity": (marker, marker.replace(y+" << distance", x+" << distance"), 4, 0),
        }
        if args.fields:
            mutants["early-field-input"] = (marker, marker.replace("Trace.x", "x").replace("Trace.y", "y"), 4, 0)
        else:
            mutants["late-field-input"] = (marker, marker.replace("x << distance", "Trace.x << distance").replace("y >> distance", "Trace.y >> distance").replace("x >>> distance", "Trace.x >>> distance").replace("y << distance", "Trace.y << distance"), 4, 0)
        for label, (before, after, variant, kind) in mutants.items():
            if original.count(before) != 1:
                raise RuntimeError("Owned integer arithmetic source anchor changed")
            wrong = work / label / "fixture/Core.java"
            wrong.parent.mkdir(parents=True)
            wrong.write_text(original.replace(before, after))
            wrong_classes = work / (label + "-classes")
            verify.compile_local([wrong], wrong_classes, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong_classes, originals, harness))),
                                    "AllocationIntegerArithmeticHarness", str(variant), str(kind), "1"],
                                   text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "integer arithmetic allocation contract" not in result.stderr:
                raise RuntimeError("Immutable observer missed integer arithmetic mutation: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, frozen callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "fields": args.fields,
                    "independent_wrong_programs": len(mutants),
                    "attempts_per_case": 2, "dependency_target": args.dependency_target,
                    "original_metadata_and_runtime_inventory": "matched",
                    "overflow_literals_input_snapshots_and_init_order": "matched", "original_failure_identity": "matched",
                    "overflow_late_input_and_literal_counterexamples": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0
if __name__ == "__main__":
    raise SystemExit(main())
