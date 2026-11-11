#!/usr/bin/env python3
"""Verify terminal Void/void siblings through plain class and generic method ancestry."""
from __future__ import annotations

import argparse
import copy
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
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_private_producer_return_slots import rename_owned_entry
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_private_producer_return_slots import rename_owned_entry
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory

OWNER = "Lfixture/Core;"
PARAMETERS = {"dispatch": "(Ljava/lang/Object;Ljava/lang/Object;)",
              "schedule": "(JLfixture/Entry;)"}
SIGNATURE = b"(JLfixture/Entry<-Ljava/lang/Throwable;>;)Ljava/lang/Void;"


def encode(root: Path) -> list:
    target = root / "fixture/Core.class"
    receipts = []
    for name, parameters in PARAMETERS.items():
        receipts += [rename_owned_entry(target, ("typed" + name.capitalize()).encode(), name.encode()),
                     bridge_flag(target, name, parameters + "V", 0x1, 0x1041)]
    return receipts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parents", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-plain-void-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "plain_nonreturning_void/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/PlainNonReturningVoidHarness.java",
                              FIXTURES / "harness/PlainNonReturningVoidCaller.java"], harness, classpath=originals)
        (work / "owned-input-encoding.json").write_text(json.dumps(encode(originals), indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        parents = {"Lfixture/" + name + ";" for name in ("Api", "Entry", "Base", "Middle")}
        classes = all_classes - (parents if args.dependency_parents else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, original_exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-parents.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for owner in sorted(parents):
                name = owner[1:-1] + ".class"
                archive.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_parents:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                              "PlainNonReturningVoidHarness"], "baseline")
        if baseline.splitlines() != ["values=14,identities=2246,states=1120,failures=672",
                "ancestor-and-interface-dispatch=matched,private-prefix-effects=retained,source-name-and-bridge-metadata=reported"]:
            raise RuntimeError("Owned plain void matrix changed: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned plain void fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original plain void runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        typed = {OWNER + "->" + name + p + "Ljava/lang/Void;" for name, p in PARAMETERS.items()}
        bridges = {OWNER + "->" + name + p + "V" for name, p in PARAMETERS.items()}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count") != 4 or projected != typed | bridges
                or coverage["status"] != "partial" or coverage.get("generic_signature_projections")
                or coverage.get("class_source_bindings")):
            raise RuntimeError("Plain void projection changed inventory, source binding or signatures")
        for name, parameters in PARAMETERS.items():
            target, bridge = OWNER + "->" + name + parameters + "Ljava/lang/Void;", OWNER + "->" + name + parameters + "V"
            expected_signature = "(JLfixture/Entry<-Ljava/lang/Throwable;>;)V" if name == "schedule" else None
            if (rows[target].get("projection_kind") != "nonreturning-void-target-alias"
                    or rows[target].get("original_bridge") != bridge or not rows[target].get("source_name")
                    or rows[bridge].get("projection_kind") != "nonreturning-void-forwarding-bridge"
                    or rows[bridge].get("bridge_target") != target
                    or rows[bridge].get("source_generic_signature") != expected_signature
                    or not rows[target].get("metadata_differences") or not rows[bridge].get("metadata_differences")):
                raise RuntimeError("Original Void/void receipt was not completely reported")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_parents else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        source_ids = {identity: row.get("source_identity", identity) for identity, row in rows.items()}
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(source_ids.values()) | helpers:
            raise RuntimeError("Rebuilt plain void inventory differs outside the reported source mapping")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Unreported class or field metadata change: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = copy.deepcopy(metadata)
                if method in typed:
                    wanted["name"] = rows[method]["source_name"]
                if method in bridges:
                    wanted["access"] = 1
                    wanted["signature"] = rows[method].get("source_generic_signature")
                if after["methods"][source_ids[method]] != wanted or actual_exceptions[source_ids[method]] != original_exceptions[method]:
                    raise RuntimeError("Unreported method contract or Exceptions change: " + method)
        aliases = [rows[OWNER + "->" + name + p + "Ljava/lang/Void;"]["source_name"] for name, p in PARAMETERS.items()]
        runtime = (rebuilt, harness, dependency) if args.dependency_parents else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             "PlainNonReturningVoidHarness", *aliases], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original dispatch, effects, generic contracts or exception identity changed")
        for label in ("missing-synthetic", "parameter-binding", "method-formal"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            target = negative / "fixture/Core.class"
            if label == "missing-synthetic":
                receipt = change_method_flags(target, (PARAMETERS["dispatch"] + "V").encode(), 0x1041, 0x41, name=b"dispatch")
            else:
                changed = (b"(JLfixture/Entry<Ljava/lang/Throwable;>;)Ljava/lang/Void;" if label == "parameter-binding"
                           else b"<T:Ljava/lang/Object;>" + SIGNATURE)
                receipt = change_signature(target, SIGNATURE, changed)
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath),
                                     "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Plain void proof accepted altered flags or parameter scope: " + label)
        negative_sources = work / "javac-negative"
        shutil.copytree(fixture, negative_sources)
        target = negative_sources / "fixture/Core.java"
        original_source = target.read_text()
        assert original_source.count("public void schedule(long delay, Entry entry)") == 1
        target.write_text(original_source.replace("public void schedule(long delay, Entry entry)",
                                                 "public void schedule(long delay, Entry<Throwable> entry)"))
        result = subprocess.run([str(verify.javac), "--release", "8", "-d", str(work / "javac-negative-classes"),
                                 *map(str, sorted(negative_sources.rglob("*.java")))], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "javac-negative.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "name clash" not in result.stderr:
            raise RuntimeError("Independent javac failed to reject the incompatible generic parameter")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original inputs, immutable callers or dependency bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 14, "identity_cases": 2246, "state_cases": 1120, "ordered_failure_cases": 672,
                    "complete_rebuilt_inventory": "reported aliases only", "original_bodies": "retained",
                    "ancestor_method_formals_and_parameter_signatures": "retained",
                    "source_names_flags_and_parameter_reflection": "reported", "dependency_parents": args.dependency_parents,
                    "same_code_counterexamples_and_independent_javac": "rejected",
                    "original_harness_and_dependencies": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
