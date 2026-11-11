#!/usr/bin/env python3
"""Verify original static integer reads inside allocation argument programs."""
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
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--field-first", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-static-integer-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "allocation_static_integer/java"
        harness_sources = [FIXTURES / "harness" / (name + ".java") for name in
                           ("AllocationStaticIntegerCaller", "AllocationStaticIntegerHarness")]
        if args.field_first:
            owned = work / "field-first-source"
            shutil.copytree(fixture, owned)
            target = owned / "fixture/Target.java"
            text = target.read_text()
            marker = "Target(Object value, int count)"
            if text.count(marker) != 1:
                raise RuntimeError("Original constructor source anchor changed")
            target.write_text(text.replace(marker, "Target(int count, Object value)"))
            creator = owned / "fixture/Creator.java"
            text = creator.read_text()
            marker = "new Target(provider.context(), Fields.number)"
            if text.count(marker) != 1:
                raise RuntimeError("Original static read source anchor changed")
            creator.write_text(text.replace(marker, "new Target(Fields.number, provider.context())"))
            fixture = owned
            source = harness_sources[1]
            text = source.read_text()
            changes = (
                ("result.number == Trace.afterCall", "result.number == (attempt == 1 ? 19 : Trace.afterConstructor)"),
                ("attempt == 1 || Trace.failAt != 1", "attempt == 1 || (Trace.failAt != 1 && Trace.failAt != 3)"),
                ("new int[] {1,2,3,7}", "new int[] {1,3,2,7}"),
                ("Trace.failAt == 1 ? 0 : attempt", "Trace.failAt == 1 || Trace.failAt == 3 ? 0 : attempt"),
            )
            for before, after in changes:
                if text.count(before) != 1:
                    raise RuntimeError("Original field-first observer anchor changed")
                text = text.replace(before, after)
            observer = work / "field-first-harness" / source.name
            observer.parent.mkdir()
            observer.write_text(text)
            harness_sources[1] = observer
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local(harness_sources, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 10:
            raise RuntimeError("Owned static-integer inventory changed")
        providers = {"Lfixture/Provider;", "Lfixture/Fields;", "Lfixture/Target;"} if args.dependency_provider else set()
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
            for variant in range(9):
                for route in range(4):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "AllocationStaticIntegerHarness", str(variant), str(route)], f"{label}-{variant}-{route}")
                    if not re.fullmatch(f"variant={variant},route={route},trace=[0-9]+\n", output):
                        raise RuntimeError("Owned static integer argument observer changed")
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
            raise RuntimeError("Owned static integer argument DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", encoded], "sdk-inventory")
        inventory = parse_dexdump(dump, encoded.name, dex_header(encoded.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed original static integer argument runtime entries")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", encoded], "original-array-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Creator;" or not name or name[1] != "make":
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            if (ops[0] != "new-instance" or ops.count("new-instance") != 1
                    or ops.count("sget") != 1 or ops.count("invoke-virtual") != 1
                    or ops[-2:] != ["invoke-direct", "return-object"]
                    or block[2].count("Lfixture/Provider;.context:") != 1
                    or block[2].count("Lfixture/Fields;.number:") != 1
                    or (ops.index("sget") < ops.index("invoke-virtual")) != args.field_first):
                raise RuntimeError("D8 changed original static read and getter order witness")
            witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original static integer allocation witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", encoded, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Static integer recovery changed original contracts or release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=8,
                             classpath=dependency if providers else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Static integer recovery invented or lost runtime entries")
        actual, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original static integer argument class metadata changed: " + owner + ":" + key)
            for identity, row in before["methods"].items():
                if after["methods"].get(identity) != row or actual_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original static integer argument method metadata/Exceptions changed")
        runtime = (rebuilt, harness, dependency) if providers else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Static integer argument, identity, failure or class-init order changed")
        creator_marker = "return new Target(provider.context(), Fields.number);"
        mutants = {
            "hoisted": ("Creator", creator_marker, "int early = Fields.number; return new Target(provider.context(), early);", 8),
            "eager-field": ("Creator", creator_marker, "return new Target(switch (0) { default -> { if (Fields.number == 0) {} yield provider.context(); } }, Fields.number);", 14),
            "late-field": ("Target", "number = count;", "number = Fields.number;", 8),
            "late-owner": ("Target", "owner = value;", "owner = Trace.owner;", 8),
        }
        if args.field_first:
            creator_marker = "return new Target(Fields.number, provider.context());"
            mutants["hoisted"] = ("Creator", creator_marker, "Object early = provider.context(); return new Target(Fields.number, early);", 8)
            mutants["eager-field"] = ("Creator", creator_marker, "return new Target(switch (0) { default -> { provider.context(); yield Fields.number; } }, Trace.owner);", 14)
        for label, (name, marker, replacement, release) in mutants.items():
            original = (fixture / "fixture" / (name + ".java")).read_text()
            if original.count(marker) != 1:
                raise RuntimeError("Owned static field source anchor changed")
            wrong = work / label / "fixture" / (name + ".java")
            wrong.parent.mkdir(parents=True)
            wrong.write_text(original.replace(marker, replacement))
            wrong_classes = work / (label + "-classes")
            verify.compile_local([wrong], wrong_classes, classpath=originals, release=release)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong_classes, originals, harness))),
                                    "AllocationStaticIntegerHarness", "0", "1"],
                                   text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "static integer allocation contract" not in result.stderr:
                raise RuntimeError("Immutable observer missed static integer mutation: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, frozen callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": 36, "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "field_first": args.field_first,
                    "original_metadata_and_runtime_inventory": "matched",
                    "integer_extrema_snapshots_aliases_and_init_order": "matched", "original_failure_identity": "matched",
                    "hoisted_eager_and_late_read_counterexamples": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0
if __name__ == "__main__":
    raise SystemExit(main())
