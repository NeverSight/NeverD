#!/usr/bin/env python3
"""Verify inherited original implementation bridges through complete generic contracts."""
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
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory

BASE_SIGNATURE = b"<R:Ljava/lang/Object;S:Ljava/lang/Object;>(Ljava/lang/Class<TR;>;TR;)TR;"
FIXED_SIGNATURE = b"(Lfixture/Carrier<Ljava/lang/Number;Ljava/lang/String;>;)Lfixture/Carrier<Ljava/lang/Number;Ljava/lang/String;>;"
BRIDGES = {"Lfixture/Base;->echo(Ljava/lang/Class;Ljava/lang/Object;)Ljava/lang/Object;",
           "Lfixture/Base;->inspect(Lfixture/Carrier;)Lfixture/Carrier;"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-root", action="store_true")
    parser.add_argument("--synthetic", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-inherited-contract-implementations-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "inherited_contract_implementations/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/InheritedContractImplementationsHarness.java"], harness, classpath=originals)
        receipts = [change_method_flags(originals / "fixture/Base.class", prototype, 0x1,
                                       0x1041 if args.synthetic else 0x41, name=name)
                    for name, prototype in ((b"echo", b"(Ljava/lang/Class;Ljava/lang/Object;)Ljava/lang/Object;"),
                                            (b"inspect", b"(Lfixture/Carrier;)Lfixture/Carrier;"))]
        (work / "owned-implementation-flags.json").write_text(json.dumps(receipts, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Api;"} if args.dependency_root else set())
        methods = {i for i in all_methods if i.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "original-declarations.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Api",):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_root:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "InheritedContractImplementationsHarness", "original",
                               "synthetic" if args.synthetic else "ordinary"], "baseline")
        if baseline.splitlines() != ["identities=928,states=416,failures=96",
                "original-inherited-providers-and-full-method-bounds=retained,bridge-flags=reported"]:
            raise RuntimeError("Independent inherited-implementation oracle missed original outcomes: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [p for p in sorted(root.rglob("*.class")) if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned inherited-implementation fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original implementation declarations")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count", 0) != 2
                or {i for i, row in rows.items() if row["status"] == "source-projected"} != BRIDGES
                or coverage["status"] != "partial" or coverage.get("generic_signature_projections")
                or coverage.get("class_source_bindings")):
            raise RuntimeError("Inherited providers changed their original inventory or complete scopes")
        for identity in BRIDGES:
            row = rows[identity]
            expected_kind = ("method-formal-implementation-bridge" if "->echo(" in identity
                             else "full-contract-implementation-bridge")
            if (row.get("projection_kind") != expected_kind
                    or row.get("source_identity") != identity or not row.get("inherited_contract")
                    or row.get("bridge_target") or not row.get("metadata_differences")):
                raise RuntimeError("Inherited original provider lost its complete declaration receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_root else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != methods | helpers:
            raise RuntimeError("Inherited implementations changed complete runtime inventory")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original class/field scope changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                wanted = dict(metadata)
                if identity in BRIDGES:
                    wanted["access"] &= ~0x1040
                if after["methods"][identity] != wanted:
                    raise RuntimeError("Complete implementation metadata changed beyond bridge flags: " + identity)
        actual_exceptions = exception_inventory(rebuilt)
        if any(actual_exceptions[i] != exceptions[i] for i in methods):
            raise RuntimeError("Original implementation exception declarations changed")
        runtime = (rebuilt, harness, dependency) if args.dependency_root else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), "InheritedContractImplementationsHarness", "rebuilt",
                             "synthetic" if args.synthetic else "ordinary"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original implementation scopes, values, callbacks or failures changed")
        for label, original, changed in (
                ("same-erasure-bound", BASE_SIGNATURE, BASE_SIGNATURE.replace(b"S:Ljava/lang/Object;", b"S:TR;")),
                ("same-erasure-result", BASE_SIGNATURE, BASE_SIGNATURE[:-3] + b"TS;"),
                ("fixed-arguments", FIXED_SIGNATURE,
                 FIXED_SIGNATURE.replace(b"Ljava/lang/Number;Ljava/lang/String;", b"Ljava/lang/String;Ljava/lang/Number;"))):
            negative = work / label
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Base.class", original, changed)
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Inherited implementation concealed a changed full contract: " + label)
        authored = work / "invalid-java"
        shutil.copytree(fixture, authored)
        source = authored / "fixture/Base.java"
        text = source.read_text()
        assert text.count("<R, S>") == 1
        source.write_text(text.replace("<R, S>", "<R, S extends R>"))
        rejected_classes = work / "invalid-classes"
        rejected_classes.mkdir()
        result = subprocess.run([str(verify.javac), "-d", str(rejected_classes), *map(str, sorted(authored.rglob("*.java")))],
                                capture_output=True, text=True, timeout=300, env=verify.environment)
        (work / "invalid-javac.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0:
            raise RuntimeError("Independent javac accepted a changed method-formal bound")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original fixture, harness, declarations or SDK changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "identities": 928, "states": 416, "failures": 96, "projected_methods": 2,
                    "synthetic_flag": args.synthetic, "dependency_root": args.dependency_root,
                    "source_coverage": coverage["status"], "original_inherited_providers": "retained without copied methods",
                    "complete_inventory_metadata_and_exceptions": "checked except disclosed bridge flags",
                    "same_erasure_counterexamples": "rejected", "original_input_harness_dependency_and_sdk": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
