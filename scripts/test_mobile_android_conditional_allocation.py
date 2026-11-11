#!/usr/bin/env python3
"""Verify allocation before original conditional constructor-argument effects."""
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
    parser.add_argument("--dependency-target", action="store_true")
    parser.add_argument("--constructor-access", choices=("public", "package", "protected"), default="public")
    parser.add_argument("--type-test", action="store_true")
    parser.add_argument("--interface-test", action="store_true")
    args = parser.parse_args()
    if args.interface_test:
        args.type_test = True
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-conditional-allocation-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "conditional_allocation/java"
        checked = "Selection" if args.interface_test else "Item"
        type_expression = "provider.current() instanceof " + checked + " ? ((" + checked + ") provider.current()).extras() : null"
        if args.constructor_access != "public" or args.type_test:
            owned = work / "fixture"
            shutil.copytree(fixture, owned)
            target = owned / "fixture/Target.java"
            text = target.read_text()
            prefix = "" if args.constructor_access == "package" else args.constructor_access + " "
            if text.count("public Target(") != 1:
                raise RuntimeError("Original conditional constructor declaration changed")
            target.write_text(text.replace("public Target(", prefix + "Target("))
            fixture = owned
            if args.type_test:
                target = fixture / "fixture/Provider.java"
                text = target.read_text()
                for before, after in [("public Item first;", "public Object first;"),
                                      ("public Item second;", "public Object second;"),
                                      ("public Item current()", "public Object current()")]:
                    if text.count(before) != 1:
                        raise RuntimeError("Original type-test provider anchor changed")
                    text = text.replace(before, after)
                target.write_text(text)
                target = fixture / "fixture/Creator.java"
                text = target.read_text()
                before = "provider.current() != null ? provider.current().extras() : null"
                if text.count(before) != 1:
                    raise RuntimeError("Original type-test condition anchor changed")
                target.write_text(text.replace(before, type_expression))
                if args.interface_test:
                    target = fixture / "fixture/Item.java"
                    text = target.read_text()
                    if text.count("public final class Item {") != 1:
                        raise RuntimeError("Original type-test interface anchor changed")
                    target.write_text(text.replace("public final class Item {", "public final class Item implements Selection {"))
                    (fixture / "fixture/Selection.java").open("x").write("package fixture; public interface Selection { Object extras(); }\n")
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        harness_sources = [FIXTURES / "harness" / (name + ".java") for name in
                           ("ConditionalAllocationCaller", "ConditionalAllocationHarness")]
        if args.type_test:
            text = harness_sources[1].read_text()
            replacements = [
                ("provider.first = variant == 1 ? null : new Item(first);", "provider.first = variant == 7 ? new Object() : variant == 1 ? null : new Item(first);"),
                ("provider.second = variant == 2 ? null : new Item(variant == 6 ? null : second);", "provider.second = variant == 8 ? new Object() : variant == 2 ? null : new Item(variant == 6 ? null : second);"),
                ("variant == 1 ? new int[]", "variant == 1 || variant == 7 ? new int[]"),
                ("variant == 2 ? new int[]", "variant == 2 || variant == 8 ? new int[]"),
                ("variant != 2 && variant != 3 && variant != 4", "variant != 2 && variant != 3 && variant != 4 && variant != 8"),
                ("variant == 1 || variant == 6 ? null : second", "variant == 1 || variant == 6 || variant == 7 ? null : second"),
                ("else if (variant == 3) require(caught instanceof ClassCastException);", "else if (variant == 3 || variant == 8) require(caught instanceof ClassCastException);")]
            for before, after in replacements:
                if text.count(before) != 1:
                    raise RuntimeError("Original type-test observer anchor changed")
                text = text.replace(before, after)
            target = work / "type-test-harness" / harness_sources[1].name
            target.parent.mkdir()
            target.open("x").write(text)
            harness_sources[1] = target
        verify.compile_local(harness_sources, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != (7 if args.interface_test else 6) or len(all_methods) != (13 if args.interface_test else 12):
            raise RuntimeError("Owned conditional-allocation inventory changed")
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
            variants = ((0, 1, 2, 3, 4, 5, 6, 7), (0, 1, 2, 3, 4, 7), (0,), (0,), (0,), (0,), (0,))
            if args.type_test:
                variants += ((0,), (0,))
            for variant, modes in enumerate(variants):
                for route in range(4):
                    for mode in modes:
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "ConditionalAllocationHarness", str(variant), str(route), str(mode)], f"{label}-{variant}-{route}-{mode}")
                        if not re.fullmatch(f"variant={variant},route={route},mode={mode},trace=[0-9]+\\n", output):
                            raise RuntimeError("Original conditional argument matrix changed: " + output)
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
            raise RuntimeError("Owned conditional argument fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original conditional-allocation runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-argument-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Creator;" or not name or name[1] != "make":
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            if (ops[0] != "new-instance" or ops.count("new-instance") != 1
                    or ops.count("invoke-virtual") != (4 if args.interface_test else 5)
                    or ops.count("invoke-interface") != (1 if args.interface_test else 0)
                    or ops.count("check-cast") != (2 if args.type_test else 1)
                    or ops.count("instance-of") != (1 if args.type_test else 0)
                    or sum(ops.count(op) for op in ("if-eqz", "if-nez")) != 1
                    or ops.count("goto") != 1 or ops[-2:] != ["invoke-direct", "return-object"]
                    or block[2].count("Lfixture/Provider;.current:") != 2):
                raise RuntimeError("D8 changed the original allocation/conditional/repeated-getter witness")
            witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original conditional allocation witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Conditional allocation recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release") != 14:
            raise RuntimeError("Conditional allocation did not disclose its switch-expression release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if providers else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if owner not in {"Lfixture/Owner;", "Lfixture/Selection;"}}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Conditional allocation recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original conditional argument class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original conditional argument method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if providers else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Final writes crossed an original effect or conditional-allocation boundary")
        # Keep new/allocation order but independently cache the first getter.
        # The second invocation and returned object identity must be observable.
        wrong = work / "cached/fixture/Creator.java"
        wrong.parent.mkdir(parents=True)
        text = (fixture / "fixture/Creator.java").read_text()
        marker = type_expression if args.type_test else "provider.current() != null ? provider.current().extras() : null"
        if text.count(marker) != 1:
            raise RuntimeError("Owned conditional source changed")
        replacement = ("switch (0) { default -> { Object selected = provider.current(); yield selected instanceof "
            + checked + " ? ((" + checked + ") selected).extras() : null; } }") if args.type_test else (
            "switch (0) { default -> { Item selected = provider.current(); yield selected != null ? selected.extras() : null; } }")
        wrong.write_text(text.replace(marker, replacement))
        verify.compile_local([wrong], work / "cached-classes", classpath=originals, release=14)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (work / "cached-classes", originals, harness))),
                                 "ConditionalAllocationHarness", "0", "1", "0"], text=True, capture_output=True,
                                timeout=300, env=verify.environment)
        (work / "cached-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle did not detect a cached conditional getter")
        if args.type_test:
            mutations = [
                ("lost-type-test", "provider.current() instanceof " + checked, "provider.current() != null", 7),
                ("cast-before-test", "provider.current() instanceof " + checked, "((" + checked + ") provider.current()) != null", 7),
                ("early-argument", "return new Target(provider.app(), ", "long early = provider.app(); return new Target(early, ", 0)]
            for label, before, after, variant in mutations:
                if text.count(before) != 1:
                    raise RuntimeError("Original type-test counterexample anchor changed")
                changed = work / label / "fixture/Creator.java"
                changed.parent.mkdir(parents=True)
                changed.open("x").write(text.replace(before, after))
                wrong_classes = work / (label + "-classes")
                verify.compile_local([changed], wrong_classes, classpath=originals, release=8)
                result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong_classes, originals, harness))),
                    "ConditionalAllocationHarness", str(variant), "1", "0"], text=True, capture_output=True,
                    timeout=300, env=verify.environment)
                (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
                if result.returncode == 0 or "conditional allocation contract" not in result.stderr:
                    raise RuntimeError("Independent observer missed incorrect type-test allocation: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "dependency_provider": args.dependency_provider,
                    "dependency_target": args.dependency_target, "constructor_access": args.constructor_access,
                    "original_metadata_and_exceptions": "unchanged", "allocation_initialization_and_conditional_getters": "matched",
                    "original_exception_identity": "matched",
                    "type_test": args.type_test, "interface_test": args.interface_test,
                    "independent_wrong_programs": 4 if args.type_test else 1,
                    "independent_cached_getter_counterexample": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
