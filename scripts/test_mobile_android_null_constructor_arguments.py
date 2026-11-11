#!/usr/bin/env python3
"""Verify null generic constructor arguments retain original runtime contracts."""
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
    parser.add_argument("--closed-throws", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-null-constructor-arguments-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "null_constructor_arguments/java"
        harness_sources = [FIXTURES / "harness" / (name + ".java") for name in
                           ("NullConstructorCaller", "NullConstructorHarness")]
        if args.closed_throws:
            staged = work / "closed-throws-source"
            shutil.copytree(fixture, staged)
            fixture = staged

            def replace_once(text, before, after):
                if text.count(before) != 1:
                    raise RuntimeError("Owned null constructor throws witness changed")
                return text.replace(before, after)

            owner = fixture / "fixture/Owner.java"
            text = owner.read_text()
            for declaration in ("public Owner(int value)", "public Owner(int value, Sink<String> target)"):
                text = replace_once(text, declaration + " {", declaration + " throws java.io.IOException {")
            for step in (2, 3):
                text = replace_once(text, f"Trace.step({step});", f"Trace.checkedStep({step});")
            owner.write_text(text)
            child = fixture / "fixture/Child.java"
            child.write_text(replace_once(child.read_text(), "public Child(int value) {",
                                         "public Child(int value) throws java.io.IOException {"))
            trace = fixture / "fixture/Trace.java"
            trace.write_text(trace.read_text().removesuffix("}\n") + """    public static java.io.IOException checkedFailure;
    public static void checkedStep(int value) throws java.io.IOException {
        trace = trace * 10 + value;
        if (failAt == value) throw checkedFailure;
    }
}
""")
            staged_harness = work / "closed-throws-harness"
            staged_harness.mkdir()
            for source in harness_sources:
                text = source.read_text()
                if source.name == "NullConstructorCaller.java":
                    text = replace_once(text, "create(int variant, int value) {",
                                        "create(int variant, int value) throws java.io.IOException {")
                else:
                    text = replace_once(text, 'Trace.failure = new RuntimeException("owned construction failure");',
                                        'Trace.failure = new RuntimeException("owned construction failure");\n'
                                        '        Trace.checkedFailure = new java.io.IOException("owned checked failure");')
                    text = replace_once(text, 'else require(error == Trace.failure, "constructor exception identity changed");',
                                        'else require(error == (Trace.failAt == 2 || Trace.failAt == 3 ? '
                                        'Trace.checkedFailure : Trace.failure), "constructor exception identity changed");')
                (staged_harness / source.name).write_text(text)
            harness_sources = sorted(staged_harness.glob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local(harness_sources, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").write_text(json.dumps(
            {"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 4 or len(all_methods) != 9 + int(args.closed_throws):
            raise RuntimeError("Owned null-constructor-arguments inventory changed")
        providers = {"Lfixture/Owner;", "Lfixture/Sink;"}
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
                    for mode in range(8):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "NullConstructorHarness", str(variant), str(route), str(mode)], f"{label}-{variant}-{route}-{mode}")
                        expected = f"variant={variant},route={route},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected,line) for line in output.splitlines()):
                            raise RuntimeError("Original constructor behavior matrix changed: " + output)
                        result.append(output)
            return result

        baseline = execute((originals, harness), "baseline")
        wrong_programs = []
        if args.closed_throws:
            controls = [
                ("lost-target-effect", "Owner.java", "Trace.checkedStep(2);", "", 0, 0),
                ("repeat-target-effect", "Owner.java", "Trace.checkedStep(2);",
                 "Trace.checkedStep(2); Trace.checkedStep(2);", 0, 0),
                ("replaced-checked-failure", "Trace.java", "throw checkedFailure;",
                 'throw new java.io.IOException("changed");', 1, 2),
                ("swallowed-checked-failure", "Trace.java", "if (failAt == value) throw checkedFailure;",
                 "if (false) throw checkedFailure;", 1, 3),
                ("non-null-argument", "Child.java", "super(value, null);",
                 "super(value, (Sink<String>) (text -> {}));", 1, 0),
            ]
            for label, filename, before, after, variant, mode in controls:
                source = work / label / filename
                source.parent.mkdir()
                source.write_text(replace_once((fixture / "fixture" / filename).read_text(), before, after))
                compiled = work / (label + "-classes")
                verify.compile_local([source], compiled, classpath=originals, release=8)
                result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                         "NullConstructorHarness", str(variant), "1", str(mode)], text=True,
                                        capture_output=True, timeout=300, env=verify.environment)
                (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
                if result.returncode == 0 or "AssertionError" not in result.stderr:
                    raise RuntimeError("Independent null constructor observer missed " + label)
                wrong_programs.append(label)
        # This separately compiled body chooses the other two-argument overload.
        # The immutable observer must detect the missing original target effects.
        wrong = work / "wrong-overload/fixture/Child.java"
        wrong.parent.mkdir(parents=True)
        text = (fixture / "fixture/Child.java").read_text()
        original_call = "super(value, null);"
        if text.count(original_call) != 1:
            raise RuntimeError("Original constructor witness changed")
        wrong.write_text(text.replace(original_call, "super(value, (Object) null);"))
        verify.compile_local([wrong], work / "wrong-overload-classes", classpath=originals, release=8)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (work / "wrong-overload-classes", originals, harness))),
                                 "NullConstructorHarness", "1", "1", "0"], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-overload-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle did not detect a changed constructor target")
        wrong_programs.append("wrong-overload")
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned null constructor fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original null-constructor-arguments runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-constructor-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            descriptor = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if (block[1] in {"Lfixture/Owner;", "Lfixture/Child;"} and name and name[1] == "<init>"
                    and descriptor and descriptor[1] == "(I)V"):
                ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
                if (len(ops) < 3 or ops[0][0] != "const/4" or "#int 0" not in ops[0][1]
                        or ops[1][0] != "invoke-direct" or "Lfixture/Owner;.<init>:(ILfixture/Sink;)V" not in ops[1][1]):
                    raise RuntimeError("javac/D8 changed the original null constructor witness")
                witnessed.add(block[1])
        if witnessed != {"Lfixture/Owner;", "Lfixture/Child;"} & classes:
            raise RuntimeError("Missing original null constructor argument witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Null constructor recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != 8:
            raise RuntimeError("Null constructor unexpectedly raised its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if owner != "Lfixture/Sink;"}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Null constructor recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original null constructor class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original null constructor method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Null constructor call changed an original effect or result")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "original_metadata_and_exceptions": "unchanged", "constructor_identity_and_initialization_order": "matched",
                    "original_exception_identity": "matched", "original_null_argument_forwarding": "independently witnessed",
                    "independent_wrong_overload_counterexample": "detected", "input_and_oracle_bytes": "unchanged"}
        evidence["closed_throws"] = args.closed_throws
        evidence["independent_wrong_programs"] = wrong_programs
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
