#!/usr/bin/env python3
"""Verify original raw self-parameter defaults with scoped Comparable parents."""
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

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags


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
    parser.add_argument("--inherited-consumer", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-raw-self-interface-default-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "raw_self_interface_default/java"
        inputs = sorted(fixture.rglob("*.java"))
        if args.inherited_consumer:
            inputs += sorted((FIXTURES / "raw_self_interface_default_consumer/java").rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        harness_source = FIXTURES / "harness/RawSelfInterfaceDefaultHarness.java"
        if args.inherited_consumer:
            content = harness_source.read_text()
            start = content.index("    private static class Entry implements Self<String> {")
            end = content.index("    private static final class Leaf extends Entry", start)
            content = content[:start] + "    private static class Entry extends Core<String> {\n" \
                    + "        Entry(int score, Object payload) { super(score, payload); }\n    }\n" + content[end:]
            harness_source = work / "owned-harness/RawSelfInterfaceDefaultHarness.java"
            harness_source.parent.mkdir()
            harness_source.write_text(content)
        verify.compile_local([harness_source], harness, classpath=originals)
        owner = "Lfixture/Self;"
        runtime_method = owner + "->compareTo(Ljava/lang/Object;)I"
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        classes, methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        classpath = ["--classpath", args.android_jar.resolve()]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "RawSelfInterfaceDefaultHarness", "original"], "baseline")
        expected = ["values=30,identities=270,states=70,failures=40",
                    "raw-self-parameter=original,scoped-comparable-parent=retained,original-default-and-checked-bridge=retained"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned raw default baseline missed a dispatch, identity or failure case: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if not result.is_file() or list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned raw default fixture did not produce one DEX")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the original raw default runtime inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 1 or coverage["status"] != "partial"
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")
                or rows[runtime_method].get("projection_kind") != "generic-interface-bridge"):
            raise RuntimeError("Raw interface default changed original runtime inventory, scopes or projection receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(methods) | helpers:
            raise RuntimeError("Recovered raw default invented a bridge or runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                wanted = before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Original raw default class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"][method] != metadata:
                    raise RuntimeError("Original raw default method scope, access or throws changed: " + method)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "RawSelfInterfaceDefaultHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Direct, inherited, reflected or SDK raw default execution changed")

        negative = work / "partial-parameter-originals"
        shutil.copytree(originals, negative)
        receipt = add_parameter_signature(negative / "fixture/Self.class",
                    "(Lfixture/Self<Ljava/lang/String;>;)I", method_name="compareTo",
                    prototype="(Lfixture/Self;)I", original_flags=1)
        (work / "partial-parameter-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = dex(negative, work / "partial-parameter-dex")
        rejected = work / "partial-parameter-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                 *map(str, classpath), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "partial-parameter-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists():
            raise RuntimeError("A partial typed parameter was accepted as a whole raw subsignature")
        unmarked = work / "unmarked-bridge-originals"
        shutil.copytree(originals, unmarked)
        receipt = change_method_flags(unmarked / "fixture/Self.class", b"(Ljava/lang/Object;)I",
                                      0x1041, 1, name=b"compareTo")
        (work / "unmarked-bridge-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        unmarked_dex = dex(unmarked, work / "unmarked-bridge-dex")
        rejected = work / "unmarked-bridge-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(unmarked_dex),
                                 *map(str, classpath), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "unmarked-bridge-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists():
            raise RuntimeError("Open raw default accepted an erased entry without original bridge flags")
        negative_java = work / "explicit-erased-java/fixture/Self.java"
        negative_java.parent.mkdir(parents=True)
        content = (fixture / "fixture/Self.java").read_text()
        declaration = "default int compareTo(Self other)"
        if content.count(declaration) != 1:
            raise RuntimeError("Missing exact independent explicit-erased-entry counterexample")
        negative_java.write_text(content.replace(declaration,
            "default int compareTo(Object other) { return 0; }\n    " + declaration, 1))
        result = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-cp", str(originals), "-d",
                                 str(work / "explicit-erased-compiler"), str(negative_java)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "explicit-erased-compiler.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "name clash" not in result.stderr:
            raise RuntimeError("Independent javac did not reject the explicit erased default")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 1, "value_cases": 30, "identity_cases": 270,
                    "profile": "inherited-consumer" if args.inherited_consumer else "default-owner",
                    "state_cases": 70, "failure_cases": 40,
                    "original_raw_subsignature_scoped_parent_and_default_owner": "retained",
                    "checked_bridge_virtual_dispatch_effects_and_exception_order": "matched",
                    "complete_original_metadata_and_runtime_inventory": "unchanged except reported helpers",
                    "same_code_partial_parameter_counterexample": "outside supported whole raw subsignature",
                    "same_code_unmarked_bridge_counterexample": "rejected",
                    "independent_explicit_erased_default": "rejected by javac",
                    "input_harness_and_sdk_bytes": "unchanged", "coverage": coverage["status"]}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
