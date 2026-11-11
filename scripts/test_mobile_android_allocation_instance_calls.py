#!/usr/bin/env python3
"""Verify same-package instance calls preserve allocation order and virtual dispatch."""
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
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_protected_array_callbacks import change_method_flags


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--inherited-provider", action="store_true")
    parser.add_argument("--call-access", choices=("package", "protected"), default="package")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-allocation-instance-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        original_fixture = FIXTURES / "allocation_instance_calls/java"
        fixture = work / "fixture"
        for original in sorted(original_fixture.rglob("*.java")):
            text = original.read_text()
            if original.name in {"Provider.java", "Derived.java"} and args.call_access == "protected":
                declaration = "    Object read(Object input)"
                if text.count(declaration) != 1:
                    raise RuntimeError("Original instance declaration changed")
                text = text.replace(declaration, "    protected Object read(Object input)")
            if original.name == "Provider.java" and args.inherited_provider:
                anchor = "    public Payload self(Object input)"
                if text.count(anchor) != 1 or text.count("public class Provider") != 1:
                    raise RuntimeError("Original inherited provider declaration changed")
                split = text.index(anchor)
                base = text[:split].replace("public class Provider", "public class Base", 1) + "}\n"
                (fixture / "fixture").mkdir(parents=True, exist_ok=True)
                (fixture / "fixture/Base.java").open("x").write(base)
                text = "package fixture;\npublic class Provider extends Base {\n" + text[split:]
            path = fixture / original.relative_to(original_fixture)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("AllocationInstanceCaller", "AllocationInstanceHarness")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != (6 if args.inherited_provider else 5) or len(all_methods) != (12 if args.inherited_provider else 11):
            raise RuntimeError("Owned allocation-instance-call inventory changed")
        providers = {"Lfixture/Payload;", "Lfixture/Provider;", "Lfixture/Derived;"}
        if args.inherited_provider:
            providers.add("Lfixture/Base;")
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            result = []
            for variant in range(2):
                for route in range(4):
                    for child in range(2):
                        for mode in range(6):
                            output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                "AllocationInstanceHarness", str(variant), str(route), str(child), str(mode)],
                                f"{label}-{variant}-{route}-{child}-{mode}")
                            expected = f"variant={variant},route={route},child={child},mode={mode},attempt=[01],trace=[0-9]+"
                            if len(output.splitlines()) != 2 or not all(re.fullmatch(expected,line) for line in output.splitlines()):
                                raise RuntimeError("Original instance allocation matrix changed: " + output)
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
            raise RuntimeError("Owned allocation instance fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original allocation-instance-call runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-constructor-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] == "Lfixture/Core;" and name and name[1] == "make":
                ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
                new = next(i for i,(op,ref) in enumerate(ops) if op == "new-instance" and "Lfixture/Payload;" in ref)
                call = next(i for i,(op,ref) in enumerate(ops) if op.split("/")[0] == "invoke-virtual" and "Lfixture/Provider;.read:" in ref)
                constructor = next(i for i,(op,ref) in enumerate(ops) if op.split("/")[0] == "invoke-direct" and "Lfixture/Payload;.<init>:" in ref)
                if not new < call < call + 1 < constructor or ops[call + 1][0] != "move-result-object":
                    raise RuntimeError("D8 changed original allocation/instance-call/result order")
                witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original instance allocation witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Allocation alias recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != 8:
            raise RuntimeError("Allocation alias unexpectedly raised its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Allocation alias recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original allocation instance class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original allocation instance method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Allocation alias call changed an original effect or result")
        negative = work / "private-originals"
        shutil.copytree(originals, negative)
        declaring_class = "Base" if args.inherited_provider else "Provider"
        encoding = change_method_flags(negative / ("fixture/" + declaring_class + ".class"), b"(Ljava/lang/Object;)Ljava/lang/Object;",
                                       0 if args.call_access == "package" else 4, 2, name=b"read")
        (work / "private-encoding.json").write_text(json.dumps(encoding, indent=2) + "\n")
        negative_classpath = list(classpath)
        if args.dependency_provider:
            negative_jar = work / "private-provider.jar"
            with zipfile.ZipFile(negative_jar, "x", compression=zipfile.ZIP_DEFLATED) as jar:
                for owner in sorted(providers):
                    name = owner[1:-1] + ".class"
                    jar.writestr(name, (negative / name).read_bytes())
            negative_classpath[-1] = negative_jar
        negative_dex = work / "private-dex"
        negative_dex.mkdir()
        negative_paths = [path for path in sorted(negative.rglob("*.class"))
                          if "L" + path.relative_to(negative).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *negative_classpath,
                    "--output", negative_dex, *negative_paths], "private-d8")
        rejected = work / "private-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex / "classes.dex"),
                                 *map(str, negative_classpath), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "private-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "private method" not in result.stderr:
            raise RuntimeError("Private instance counterexample lost its dispatch guard: " + result.stderr)
        counterexamples = [
            ("hoisted-call", "Core", "return new Payload(provider.read(input), provider);",
             "Object early = provider.read(input); return new Payload(early, provider);", "0"),
            ("lost-override", "Derived", None, None, "1"),
        ]
        for label, owner, before, after, child in counterexamples:
            wrong = work / label / ("fixture/" + owner + ".java")
            wrong.parent.mkdir(parents=True)
            text = (fixture / ("fixture/" + owner + ".java")).read_text()
            if before:
                if text.count(before) != 1:
                    raise RuntimeError("Original instance counterexample anchor changed")
                text = text.replace(before, after)
            else:
                access = "protected " if args.call_access == "protected" else ""
                text = "package fixture; public final class Derived extends Provider { " + access + "Object read(Object input) { return super.read(input); } }\n"
            wrong.write_text(text)
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     "AllocationInstanceHarness", "0", "1", child, "0"], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "allocation instance contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "call_access": args.call_access,
                    "inherited_provider": args.inherited_provider,
                    "original_metadata_and_exceptions": "unchanged", "original_allocation_instance_call": "independently witnessed",
                    "class_initialization_order_and_exception_identity": "matched",
                    "same_package_virtual_and_self_calls": "matched", "receiver_and_mutable_result_identity": "matched",
                    "private_dispatch_counterexample": "rejected", "independent_hoist_and_override_counterexamples": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
