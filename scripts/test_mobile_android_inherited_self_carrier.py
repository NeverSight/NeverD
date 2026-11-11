#!/usr/bin/env python3
"""Verify inherited self-typed carrier results with disclosed source-only erasure."""
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
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-root", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-inherited-self-carrier-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "inherited_self_carrier/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/InheritedSelfCarrierHarness.java", FIXTURES / "harness/SelfCarrierCaller.java"], harness, classpath=originals)
        api_signature = b"()Lfixture/Carrier<TT;+Lfixture/Api<TT;>;>;"
        provider_signature = b"()Lfixture/Carrier<TT;Lfixture/Provider<TT;>;>;"
        edit = change_signature(originals / "fixture/Api.class", api_signature, api_signature.replace(b"+", b""))
        (work / "owned-invariant-result.json").write_text(json.dumps(edit, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        declarations = {"Lfixture/Api;", "Lfixture/Carrier;", "Lfixture/View;"}
        classes = all_classes - (declarations if args.dependency_root else set())
        methods = {i for i in all_methods if i.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "original-declarations.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Api", "Carrier", "View"):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_root:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "InheritedSelfCarrierHarness", "original"], "baseline")
        if baseline.splitlines() != ["values=135,identities=285,states=150,failures=20",
                "original-inherited-provider-and-complete-ancestor-scopes=retained,raw-source-result=reported"]:
            raise RuntimeError("Independent self carrier oracle missed original outcomes: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [p for p in sorted(root.rglob("*.class")) if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned self carrier fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original self carrier declarations")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        provider = "Lfixture/Provider;->result()Lfixture/Carrier;"
        projections = coverage.get("generic_signature_projections", [])
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count", 0)
                or coverage["status"] != "partial" or coverage.get("class_source_bindings")
                or len(projections) != 1 or projections[0]["declaration"] != provider
                or len(projections[0]["changes"]) != 1
                or projections[0]["changes"][0]["kind"] != "invariant-self-carrier-result-erasure"
                or projections[0]["original_signature"] != provider_signature.decode()
                or projections[0]["changes"][0]["source_type"] != "()Lfixture/Carrier;"
                or rows[provider].get("generic_return_contract") != "invariant-self-carrier-result-erasure"):
            raise RuntimeError("Self carrier result changed inventory or hid a source Signature projection")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_root else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != methods | helpers:
            raise RuntimeError("Self carrier changed runtime inventory beyond reported helpers")
        after = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after[owner].get(key) != records[owner].get(key):
                    raise RuntimeError("Original class/field scope changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                wanted = dict(metadata)
                if identity == provider:
                    wanted["signature"] = None
                if after[owner]["methods"][identity] != wanted:
                    raise RuntimeError("Original metadata changed beyond disclosed raw result: " + identity)
        actual_exceptions = exception_inventory(rebuilt)
        if any(actual_exceptions[i] != exceptions[i] for i in methods):
            raise RuntimeError("Original exception declarations changed")
        runtime = (rebuilt, harness, dependency) if args.dependency_root else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), "InheritedSelfCarrierHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Self carrier values, virtual dispatch or failures changed")
        for label, changed in (
                ("wrong-input-formal", provider_signature.replace(b"Carrier<TT;", b"Carrier<Ljava/lang/Object;")),
                ("wrong-self-formal", provider_signature.replace(b"Provider<TT;", b"Provider<Ljava/lang/Object;"))):
            negative = work / label
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Provider.class", provider_signature, changed)
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Raw source result hid incompatible original formals: " + label)
        authored = work / "invalid-java"
        shutil.copytree(fixture, authored)
        source = authored / "fixture/Api.java"
        text = source.read_text()
        assert text.count("? extends Api<T>") == 1
        source.write_text(text.replace("? extends Api<T>", "Api<T>"))
        rejected_classes = work / "invalid-classes"
        rejected_classes.mkdir()
        result = subprocess.run([str(verify.javac), "-d", str(rejected_classes), *map(str, sorted(authored.rglob("*.java")))],
                                capture_output=True, text=True, timeout=300, env=verify.environment)
        (work / "invalid-javac.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0:
            raise RuntimeError("Independent javac accepted the invariant self-typed override without source adaptation")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original fixture, harness, declarations or SDK changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "values": 135, "identities": 285, "states": 150, "failures": 20,
                    "projected_methods": 0, "generic_result_projections": 1, "dependency_root": args.dependency_root,
                    "source_coverage": coverage["status"], "original_runtime_inventory_and_exceptions": "unchanged",
                    "original_complete_metadata": "unchanged except one disclosed raw provider result",
                    "inherited_provider": "no copied methods or new bridges",
                    "same_code_counterexamples": "rejected", "original_input_harness_dependency_and_sdk": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
