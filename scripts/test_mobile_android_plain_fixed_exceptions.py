#!/usr/bin/env python3
"""Verify fixed Exceptions, optional original subsets and complete generic method contracts."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature


def hashes(root: Path) -> dict:
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(root.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-root", action="store_true")
    parser.add_argument("--exception-subsets", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-plain-fixed-exceptions-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture_name = "fixed_exception_subsets" if args.exception_subsets else "plain_fixed_exceptions"
        harness_name = "FixedExceptionSubsetsHarness" if args.exception_subsets else "PlainFixedExceptionsHarness"
        fixture = FIXTURES / fixture_name / "java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness" / (harness_name + ".java")], harness,
                             classpath=originals)
        owner = "Lfixture/Core;"
        runtime_method = owner + "->read(Ljava/lang/Object;)Ljava/lang/Object;"
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Api;"} if args.dependency_root else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        dependency = work / "owned-root.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("fixture/Api.class", (originals / "fixture/Api.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_root:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_name, "original"], "baseline")
        expected = ["values=48,identities=536,states=104,failures=56",
                    ("fixed-exception-subsets-and-order=retained" if args.exception_subsets else "fixed-exception-order=retained")
                    + ",complete-method-formals=retained,original-runtime-entries=retained"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned plain fixed exception contract baseline missed a dispatch, identity or failure case: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if not result.is_file() or list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned plain fixed exception contract fixture did not produce one DEX")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the original plain fixed exception contract runtime inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 0 or coverage["status"] != "recovered"
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")
                or any(row["status"] == "source-projected" for row in rows.values())):
            raise RuntimeError("Plain fixed exception contract changed original runtime inventory, scopes or projection receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_root else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(methods) | helpers:
            raise RuntimeError("Recovered plain fixed exception contract invented a bridge or runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                wanted = before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Original plain fixed exception contract class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"][method] != metadata:
                    raise RuntimeError("Original plain fixed exception contract method scope, access or throws changed: " + method)
        rebuilt_path = (rebuilt, harness, dependency) if args.dependency_root else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, rebuilt_path)),
                             harness_name, "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Direct, inherited, reflected or SDK plain fixed exception contract execution changed")

        negative = work / "method-bound-drift-originals"
        shutil.copytree(originals, negative)
        receipt = change_signature(negative / "fixture/Core.class",
            b"<R:Ljava/lang/Object;>(Ljava/lang/Class<TR;>;)TR;",
            b"<R:Ljava/lang/Number;>(Ljava/lang/Class<TR;>;)TR;")
        (work / "method-bound-drift-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = dex(negative, work / "method-bound-drift-dex")
        rejected = work / "method-bound-drift-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                 *map(str, classpath), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "method-bound-drift-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists():
            raise RuntimeError("Plain fixed Exceptions admitted changed method-owned bounds")
        negative_java = work / "method-bound-drift-java/fixture/Core.java"
        negative_java.parent.mkdir(parents=True)
        content = (fixture / "fixture/Core.java").read_text()
        declaration = "public <R> R unwrap(Class<R> kind) throws First"
        if content.count(declaration) != 1:
            raise RuntimeError("Missing exact independent method-bound counterexample")
        negative_java.write_text(content.replace(
            declaration, "public <R extends Number> R unwrap(Class<R> kind) throws First", 1))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-cp", str(originals), "-d",
                                   str(work / "method-bound-drift-compiler"), str(negative_java)], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "method-bound-drift-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or "name clash" not in compiler.stderr:
            raise RuntimeError("Independent javac accepted method-bound drift beneath fixed Exceptions")
        if args.exception_subsets:
            try:
                from scripts.test_mobile_android_inherited_exception_bridges import change_owned_throws
            except ModuleNotFoundError as error:
                if error.name != "scripts":
                    raise
                from test_mobile_android_inherited_exception_bridges import change_owned_throws
            narrower = work / "required-exception-removed-originals"
            shutil.copytree(originals, narrower)
            receipt = change_owned_throws(narrower / "fixture/Api.class", b"unwrap",
                                         b"(Ljava/lang/Class;)Ljava/lang/Object;", 0x0401, "drop")
            (work / "required-exception-removed-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
            narrower_dex = dex(narrower, work / "required-exception-removed-dex")
            narrower_classpath = list(classpath)
            if args.dependency_root:
                narrower_dependency = work / "required-exception-removed-root.jar"
                with zipfile.ZipFile(narrower_dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                    archive.writestr("fixture/Api.class", (narrower / "fixture/Api.class").read_bytes())
                narrower_classpath[-1] = narrower_dependency
            rejected = work / "required-exception-removed-rejected"
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(narrower_dex),
                                     *map(str, narrower_classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / "required-exception-removed-recovery.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Exception subset admission accepted an uncovered checked target declaration")
            invalid = work / "required-exception-removed-java"
            compiler_inputs = []
            for source in inputs:
                content = source.read_text()
                if source.name == "Api.java":
                    declaration = "<T> T unwrap(Class<T> kind) throws First, UnusedFailure"
                    if content.count(declaration) != 1:
                        raise RuntimeError("Missing exact owned required exception subset counterexample")
                    content = content.replace(declaration, "<T> T unwrap(Class<T> kind) throws UnusedFailure", 1)
                destination = invalid / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                compiler_inputs.append(str(destination))
            compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                       str(work / "required-exception-removed-compiler"), *compiler_inputs],
                                      text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "required-exception-removed-compiler.log").write_text(compiler.stdout + compiler.stderr)
            if compiler.returncode == 0 or not any(word in compiler.stderr for word in ("throw", "override")):
                raise RuntimeError("Independent javac accepted an uncovered checked target declaration")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 0, "value_cases": 48, "identity_cases": 536,
                    "state_cases": 104, "failure_cases": 56,
                    "profile": "dependency-root" if args.dependency_root else "source-root",
                    "fixed_exception_order_and_complete_method_owned_scope": "retained",
                    "original_exception_subsets": "retained" if args.exception_subsets else "not applicable",
                    "same_code_uncovered_checked_exception_counterexample": "rejected by NeverD and independent javac" if args.exception_subsets else "not applicable",
                    "mutable_aliases_virtual_dispatch_and_late_cast_order": "matched",
                    "complete_original_metadata_and_runtime_inventory": "unchanged except reported helpers",
                    "same_code_method_bound_counterexample": "rejected by NeverD and independent javac",
                    "input_harness_and_sdk_bytes": "unchanged", "coverage": coverage["status"]}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
