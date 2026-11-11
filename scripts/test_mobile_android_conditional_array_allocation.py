#!/usr/bin/env python3
"""Verify signed constructor-argument branches and original reference array loads."""
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
    parser.add_argument("--dependency-target", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-conditional-array-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "conditional_array_allocation/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("ConditionalArrayAllocationCaller", "ConditionalArrayAllocationHarness")],
                             harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 4 or len(all_methods) != 9:
            raise RuntimeError("Owned conditional-array inventory changed")
        providers = ({"Lfixture/Provider;"} if args.dependency_provider else set()) | ({"Lfixture/Target;"} if args.dependency_target else set())
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
            for variant in range(15):
                for route in range(4):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "ConditionalArrayAllocationHarness", str(variant), str(route)], f"{label}-{variant}-{route}")
                    if not re.fullmatch(f"variant={variant},route={route},trace=[0-9]+\n", output):
                        raise RuntimeError("Owned array argument observer changed")
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
            raise RuntimeError("Owned array argument DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", encoded], "sdk-inventory")
        inventory = parse_dexdump(dump, encoded.name, dex_header(encoded.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed original array argument runtime entries")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", encoded], "original-array-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Creator;" or not name or name[1] != "make":
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            if (ops[0] != "new-instance" or ops.count("new-instance") != 1
                    or ops.count("aget-object") != 1 or ops.count("if-gez") != 1
                    or ops.count("goto") != 1 or ops[-2:] != ["invoke-direct", "return-object"]
                    or block[2].count("Lfixture/Provider;.index:") != 2
                    or block[2].count("Lfixture/Provider;.values:") != 1):
                raise RuntimeError("D8 changed signed branch, array load or repeated-index witness")
            witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original signed array allocation witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", encoded, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release") != 14):
            raise RuntimeError("Conditional array recovery changed original contracts or release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=14,
                             classpath=dependency if providers else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Conditional array recovery invented or lost runtime entries")
        actual, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original array argument class metadata changed: " + owner + ":" + key)
            for identity, row in before["methods"].items():
                if after["methods"].get(identity) != row or actual_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original array argument method metadata/Exceptions changed")
        runtime = (rebuilt, harness, dependency) if providers else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Signed array argument, identity, failure or class-init order changed")
        original = (fixture / "fixture/Creator.java").read_text()
        marker = "return new Target(provider.index() < 0 ? provider.fallback : provider.values()[provider.index()]);"
        if original.count(marker) != 1:
            raise RuntimeError("Owned array argument source anchor changed")
        mutants = {
            "hoisted": ("Object value = provider.index() < 0 ? provider.fallback : provider.values()[provider.index()]; return new Target(value);", 0, 8),
            "cached-index": ("return new Target(switch (0) { default -> { int index = provider.index(); yield index < 0 ? provider.fallback : provider.values()[index]; } });", 0, 14),
            "eager-array": ("return new Target(switch (0) { default -> { Object[] values = provider.values(); yield provider.index() < 0 ? provider.fallback : values[provider.index()]; } });", 1, 14),
        }
        for label, (body, variant, release) in mutants.items():
            wrong = work / label / "fixture/Creator.java"
            wrong.parent.mkdir(parents=True)
            wrong.write_text(original.replace(marker, body))
            wrong_classes = work / (label + "-classes")
            verify.compile_local([wrong], wrong_classes, classpath=originals, release=release)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong_classes, originals, harness))),
                                    "ConditionalArrayAllocationHarness", str(variant), "1"],
                                   text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "conditional array allocation contract" not in result.stderr:
                raise RuntimeError("Immutable observer missed array argument mutation: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, frozen callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": 60, "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "dependency_target": args.dependency_target, "original_metadata_and_runtime_inventory": "matched",
                    "signed_branches_array_aliases_bounds_nulls_and_init_order": "matched", "original_failure_identity": "matched",
                    "hoisted_cached_and_eager_counterexamples": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0
if __name__ == "__main__":
    raise SystemExit(main())
