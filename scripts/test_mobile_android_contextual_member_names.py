#!/usr/bin/env python3
"""Verify contextual Java member names, qualified calls and original binary contracts."""
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
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-contextual-names-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "contextual_member_names/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("ContextualMemberNamesCaller", "ContextualMemberNamesHarness")],
                             harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 2 or len(all_methods) != 9:
            raise RuntimeError("Owned contextual member inventory changed")
        providers = {"Lfixture/yield/Base;"} if args.dependency_provider else set()
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
                for route in range(4):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "ContextualMemberNamesHarness", str(variant), str(route)], f"{label}-{variant}-{route}")
                    if not re.fullmatch(f"variant={variant},route={route},ok\n", output):
                        raise RuntimeError("Owned contextual member observer changed")
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
            raise RuntimeError("Owned contextual member DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", encoded], "sdk-inventory")
        inventory = parse_dexdump(dump, encoded.name, dex_header(encoded.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed original contextual member runtime entries")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", encoded], "original-member-disassembly")
        if not re.search(r"invoke-super[^\n]*Lfixture/yield/Base;\.yield:\(I\)I", disassembly):
            raise RuntimeError("Missing original qualified superclass member witness")
        for name in ("yield", "record", "sealed", "permits", "var"):
            if not any("->" + name + "(" in identity for identity in all_methods):
                raise RuntimeError("Missing original contextual member identity")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", encoded, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Contextual member recovery changed original contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=8,
                             classpath=dependency if providers else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Contextual member recovery invented or lost runtime entries")
        modern = work / "rebuilt-java21"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), modern, release=21,
                             classpath=dependency if providers else None)
        modern_classes, modern_methods = compiler_inventory(verify, modern)
        if modern_classes != actual_classes or set(modern_methods) != set(actual_methods):
            raise RuntimeError("Contextual member identities changed on Java 21")
        actual, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original contextual member class metadata changed: " + owner + ":" + key)
            for identity, row in before["methods"].items():
                if after["methods"].get(identity) != row or actual_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original contextual member method metadata/Exceptions changed")
        runtime = (rebuilt, harness, dependency) if providers else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Contextual member reads, copies, failure identity or prefix changed")
        modern_runtime = (modern, harness, dependency) if providers else (modern, harness)
        if execute(modern_runtime, "rebuilt-java21") != baseline:
            raise RuntimeError("Contextual member behavior changed on Java 21")
        original = (fixture / "fixture/yield/Core.java").read_text()
        mutants = {
            "lost-super": ("super.yield(value) ^ this.record", "value ^ this.record", 3),
            "field-for-call": ("Base.record(value)", "target.record", 3),
            "wrong-var": ("value - 23", "value + 23", 3),
        }
        for label, (before, after, variant) in mutants.items():
            if original.count(before) != 1:
                raise RuntimeError("Owned contextual member source anchor changed")
            wrong = work / label / "fixture/yield/Core.java"
            wrong.parent.mkdir(parents=True)
            wrong.write_text(original.replace(before, after))
            wrong_classes = work / (label + "-classes")
            verify.compile_local([wrong], wrong_classes, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong_classes, originals, harness))),
                                    "ContextualMemberNamesHarness", str(variant), "1"],
                                   text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "contextual member name contract" not in result.stderr:
                raise RuntimeError("Immutable observer missed contextual member mutation: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, frozen callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "verified_java_releases": [8, 21],
                    "fresh_jvm_cases": 32, "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "original_metadata_and_runtime_inventory": "matched", "qualified_calls_fields_and_original_failure_identity": "matched",
                    "dispatch_field_and_arithmetic_counterexamples": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
