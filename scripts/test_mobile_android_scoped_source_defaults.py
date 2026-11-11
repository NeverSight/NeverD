#!/usr/bin/env python3
"""Verify class-owned source default returns through complete generic SDK ancestry."""
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
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-interfaces", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-scoped-source-defaults-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "scoped_source_defaults/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/ScopedSourceDefaultsHarness.java"],
                             harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        all_classes, all_methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        classes = all_classes - ({"Lfixture/Api;", "Lfixture/Box;"} if args.dependency_interfaces else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        if (len(classes), len(methods)) != ((2, 4) if args.dependency_interfaces else (4, 5)):
            raise RuntimeError("Owned scoped source default inventory changed")
        dependency = work / "owned-api.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Api", "Box"):
                archive.writestr("fixture/" + name + ".class", (originals / "fixture" / (name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_interfaces:
            classpath += ["--classpath", dependency]

        def dex(root: Path, output: Path) -> Path:
            paths = [p for p in sorted(root.rglob("*.class"))
                     if not (args.dependency_interfaces and p.relative_to(root).as_posix() in ("fixture/Api.class", "fixture/Box.class"))]
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", output, *paths], "d8-" + output.name)
            result = output / "classes.dex"
            if not result.is_file() or list(output.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned scoped source default fixture did not produce exactly one DEX")
            return result

        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "ScopedSourceDefaultsHarness"], "baseline")
        expected = ["values=10,identities=10,states=15,failures=5",
                    "class-formal-default-return=retained,source-declaring-owner=retained,no-consumer-default-copy=retained"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned scoped source default baseline missed an exception or dispatch case: " + baseline)
        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed the original scoped source default inventory")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage.get("projected_method_count", 0) != 0
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "recovered"
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")
                or any(row["status"] == "source-projected" for row in rows.values())):
            raise RuntimeError("Scoped source default proof changed original scopes, methods or source bindings")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_interfaces else None,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != methods | helpers:
            raise RuntimeError("Scoped source default recompile changed original inventory or added an unreported helper")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            wanted, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != wanted.get(key):
                    raise RuntimeError("Original scoped source default class metadata changed: " + owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original scoped source default metadata changed: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "ScopedSourceDefaultsHarness"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Scoped default identity, nulls or effects changed")
        negative = work / "wrong-formal-result-originals"
        shutil.copytree(originals, negative)
        receipt = change_signature(negative / "fixture/Source.class",
                                   b"()Lfixture/Box<TS;>;", b"()Lfixture/Box<Ljava/lang/String;>;")
        (work / "wrong-formal-result-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = dex(negative, work / "wrong-formal-result-dex")
        rejected = work / "wrong-formal-result-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                 *map(str, classpath), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-formal-result-recovery.log").write_text(result.stdout + result.stderr)
        if (result.returncode == 0 or rejected.exists()
                or not any(word in result.stderr for word in ("generic", "interface", "inheritance"))):
            raise RuntimeError("Scoped source default accepted a different complete formal result")
        invalid = work / "wrong-formal-result-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "Source.java":
                if content.count("default Box<S> getBox()") != 1:
                    raise RuntimeError("Missing exact owned default result counterexample")
                content = content.replace("default Box<S> getBox()", "default Box<String> getBox()", 1)
            destination = invalid / source.relative_to(fixture)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "wrong-formal-result-compiler"), *compiler_inputs], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-formal-result-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or not any(word in compiler.stderr for word in ("return", "override", "incompatible")):
            raise RuntimeError("Independent javac accepted an incompatible default result")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original classes, immutable harness or declaration bytes changed")
        evidence = {"status": "success", "dependency_interfaces": args.dependency_interfaces,
                    "class_count": len(classes), "method_count": len(methods), "projected_methods": 0,
                    "value_cases": 10, "identity_cases": 10, "state_cases": 15, "failure_cases": 5,
                    "complete_original_metadata_and_inventory": "unchanged except reported helpers",
                    "class_owned_default_result_and_declaring_owner": "unchanged",
                    "returned_self_identity_and_default_effect_order": "matched",
                    "same_Code_complete_formal_result_counterexample": "rejected",
                    "independent_javac_default_result_counterexample": "rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged", "coverage": "recovered"}
        (work / "verified.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
