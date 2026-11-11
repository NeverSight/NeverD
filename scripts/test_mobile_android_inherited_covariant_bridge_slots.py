#!/usr/bin/env python3
"""Verify inherited broad bridges beside an unchanged typed superclass slot."""
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
    from scripts.test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from scripts.test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge

PROVIDERS = {"Lfixture/Root;", "Lfixture/Editable;", "Lfixture/Base;", "Lfixture/Failure;"}
PARAMETERS = "(I[Ljava/lang/Object;JD)"


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-inherited-covariance-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "inherited_covariant_bridge_slots/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("InheritedCovariantSlotDerived", "InheritedCovariantSlotHarness")],
                             harness, classpath=originals, release=8)
        omissions = [omit_owned_interface_bridge(harness / "InheritedCovariantSlotDerived.class",
                     ("update" + PARAMETERS + "Lfixture/" + result + ";").encode())
                     for result in ("Root", "Editable")]
        (work / "frozen-subclass-bridge-omissions.json").write_text(json.dumps(omissions, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 6 or len(all_methods) != 16:
            raise RuntimeError("Owned inherited covariance inventory changed")
        classes = all_classes - (PROVIDERS if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        if any(record["signature"] for record in records.values()):
            raise RuntimeError("Inherited covariance acquired a class Signature")
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(PROVIDERS):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                 "InheritedCovariantSlotHarness"], label)
            if output.splitlines() != ["calls=160,failures=80,original-slots-throws-and-dispatch=matched"]:
                raise RuntimeError("Original slot dispatch matrix changed: " + output)
            return output

        def dex(root, label):
            destination = work / label
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", destination, *paths], "d8-" + label)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Unexpected original DEX inventory")
            return result

        baseline = execute((originals, harness), "baseline")
        source_dex = dex(originals, "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed original covariance entries")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        bridges = {identity for owner in classes for identity, row in records[owner]["methods"].items()
                   if row["access"] & 0x40}
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-bridge-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            prototype = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if not name or not prototype:
                continue
            identity = block[1] + "->" + name[1] + prototype[1]
            if identity not in bridges:
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            if (len(ops) != 3 or ops[0] not in {"invoke-virtual/range", "invoke-interface/range"}
                    or ops[1:] != ["move-result-object", "return-object"]):
                raise RuntimeError("Original covariance bridge body changed")
            witnessed.add(identity)
        if witnessed != bridges or len(bridges) != (2 if args.dependency_provider else 5):
            raise RuntimeError("Missing original bridge witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["projected_method_count"] != len(bridges)
                or coverage["status"] != "partial" or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != (8 if args.dependency_provider else 9)):
            raise RuntimeError("Inherited covariance lost entries or complete contracts")
        rows = {row["identity"]: row for row in coverage["methods"]}
        for identity in bridges:
            owner = identity.split("->", 1)[0]
            result = owner if owner == "Lfixture/Editable;" else "Lfixture/Base;"
            if rows[identity].get("bridge_target") != owner + "->update" + PARAMETERS + result:
                raise RuntimeError("Original bridge lost its exact typed target")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8), classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes - {"Lfixture/Root;"}}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Inherited covariance invented or lost runtime entries")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "enclosing_method", "inner_class",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original class contract changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after["methods"].get(identity) != metadata or actual_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Original method contract or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Rebuilt inherited covariance changed runtime behavior")
        negative = work / "method-formal-originals"
        shutil.copytree(originals, negative)
        receipt = add_parameter_signature(negative / "fixture/Worker.class",
                  "<T:Ljava/lang/Object;>" + PARAMETERS + "Lfixture/Base;", method_name="update",
                  prototype=PARAMETERS + "Lfixture/Base;")
        (work / "method-formal-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        encoded, rejected = dex(negative, "method-formal-dex"), work / "method-formal-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(encoded), *map(str, classpath), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "method-formal-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Same-Code changed source contract was admitted")
        wrong = work / "changed-result/fixture/Worker.java"
        wrong.parent.mkdir(parents=True)
        text = (fixture / "fixture/Worker.java").read_text()
        if text.count("return result;") != 1:
            raise RuntimeError("Owned result anchor changed")
        wrong.write_text(text.replace("return result;", "return null;"))
        wrong_classes = work / "changed-result-classes"
        verify.compile_local([wrong], wrong_classes, classpath=originals, release=8)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong_classes, originals, harness))),
                                 "InheritedCovariantSlotHarness"], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "changed-result-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "inherited covariance contract" not in result.stderr:
            raise RuntimeError("Independent observer missed changed result identity")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, frozen observer or SDK bytes changed")
        evidence = {"status": "success", "coverage": "partial", "class_count": len(classes),
                    "method_count": len(methods), "projected_original_bridges": len(bridges),
                    "runtime_calls": 160, "expected_failures": 80, "dependency_provider": args.dependency_provider,
                    "metadata_Exceptions_and_runtime_inventories": "matched",
                    "original_broad_slots_and_typed_virtual_dispatch": "matched",
                    "same_code_constraint_counterexample": "rejected", "changed_result_counterexample": "detected",
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
