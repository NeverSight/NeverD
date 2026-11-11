#!/usr/bin/env python3
"""Verify complete original erased interface bodies with checked incoming parameters."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
import re
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
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
    parser.add_argument("--dependency-root", action="store_true")
    parser.add_argument("--recursive-bound", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-checked-inlined-interfaces-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture_name = "checked_inlined_interfaces"
        harness_name = "CheckedInlinedInterfacesHarness"
        fixture = FIXTURES / fixture_name / "java"
        inputs = sorted(fixture.rglob("*.java"))
        if args.recursive_bound:
            inputs += sorted((FIXTURES / "checked_inlined_recursive_bounds/java").rglob("*.java"))
            harness_name = "CheckedInlinedRecursiveBoundsHarness"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness" / (harness_name + ".java")], harness,
                             classpath=originals)
        owner = "Lfixture/Core;"
        runtime_method = owner + "->read(Ljava/lang/Object;)I"
        source_signature = b"Ljava/lang/Object;Lfixture/Api<Ljava/lang/Object;>;"
        input_signature = b"Ljava/lang/Object;Lfixture/Api<Lfixture/Core;>;"
        metadata_receipt = change_signature(originals / "fixture/Core.class", source_signature, input_signature)
        flags_receipt = change_method_flags(originals / "fixture/Core.class", b"(Ljava/lang/Object;)I",
                                           0x1, 0x1041, name=b"read")
        (work / "original-inlined-bridge-encoding.json").write_text(
            json.dumps({"metadata": metadata_receipt, "flags": flags_receipt}, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Api;"} if args.dependency_root else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        dependency = work / "owned-root.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Api",):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_root:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_name, "original"], "baseline")
        counts = (286, 1899, 1045, 66) if args.recursive_bound else (156, 774, 570, 36)
        expected = ["values=%d,identities=%d,states=%d,failures=%d" % counts,
                    "original-inlined-bridge-body=retained,cast-before-argument-before-receiver-effects=retained,object-interface-argument-and-bridge-flags=reported"
                    + (",complete-recursive-lower-bounds=retained" if args.recursive_bound else "")]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned inlined interface baseline missed an alias, effect or failure case: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if not result.is_file() or list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned checked inlined interface fixture did not produce one DEX")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the original checked inlined interface runtime inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-inlined-bridge-disassembly")
        witnesses = {}
        blocks = re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly)
        for block in blocks:
            declaration = block[2]
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", declaration)
            prototype = re.search(r"(?m)^\s+type\s+: '([^']+)'", declaration)
            flags = re.search(r"(?m)^\s+access\s+: 0x([0-9a-fA-F]+)", declaration)
            if not name or not prototype or not prototype[1].startswith("("):
                continue
            identity = block[1] + "->" + name[1] + prototype[1]
            if identity != runtime_method:
                continue
            opcodes = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", declaration)
            calls = re.findall(r"\|[0-9a-fA-F]+:\s+invoke-virtual(?:/range)?[^\n]*", declaration)
            if (identity in witnesses or not flags or int(flags[1], 16) != 0x1041
                    or opcodes.count("check-cast") != 1
                    or opcodes.count("move-result-wide") != 2 or len(calls) != 2
                    or not all("Lfixture/Core;.unbox:()J" in call for call in calls)
                    or opcodes.index("check-cast") > next(i for i, op in enumerate(opcodes) if op.startswith("invoke-"))
                    or any(".read:" in call for call in calls)):
                raise RuntimeError("D8 changed the original inlined bridge or its ordered unboxing calls")
            witnesses[identity] = opcodes
        if set(witnesses) != {runtime_method}:
            raise RuntimeError("Missing complete original inlined interface body opcode witness")
        (work / "original-inlined-bridge-opcodes.json").write_text(json.dumps(witnesses, indent=2) + "\n")

        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projections = coverage.get("generic_signature_projections", [])
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 1 or coverage["status"] != "partial"
                or len(projections) != 1 or coverage.get("class_source_bindings")
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != {runtime_method}):
            raise RuntimeError("Checked inlined interface changed original inventory or projection receipt")
        projection = projections[0]
        changes = projection.get("changes", [])
        if (projection.get("declaration") != owner or projection.get("original_signature") != input_signature.decode()
                or len(changes) != 1 or changes[0].get("kind") != "checked-inlined-interface"
                or changes[0].get("source_type") != source_signature.decode().removeprefix("Ljava/lang/Object;")
                or changes[0].get("scope_origin") != runtime_method
                or rows[runtime_method].get("projection_kind") != "checked-inlined-interface-bridge"
                or rows[runtime_method].get("inherited_contract") != "Lfixture/Api;->read(Ljava/lang/Object;)I"
                or rows[runtime_method].get("source_identity") != runtime_method
                or not rows[runtime_method].get("metadata_differences")):
            raise RuntimeError("Inlined interface proof lost its exact full original edge or body witness")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_root else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(methods) | helpers:
            raise RuntimeError("Recovered inlined interface body invented a bridge or runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                wanted = source_signature.decode() if identity == owner and key == "signature" else before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Original inlined interface class metadata changed beyond its reported Object argument: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if method == runtime_method:
                    if wanted["access"] != 0x1041:
                        raise RuntimeError("Original inlined interface body lost bridge/synthetic flags")
                    wanted["access"] = 0x1
                if after["methods"][method] != wanted:
                    raise RuntimeError("Original inlined interface method metadata changed beyond reported flags: " + method)
        rebuilt_path = (rebuilt, harness, dependency) if args.dependency_root else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, rebuilt_path)),
                             harness_name, "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Direct, interface, reflected or SDK inlined bridge execution changed")

        for label in ("complete-argument-drift", "missing-synthetic-flag"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "complete-argument-drift":
                receipt = change_signature(negative / "fixture/Core.class", input_signature,
                    b"Ljava/lang/Object;Lfixture/Api<Lfixture/Other;>;")
            else:
                receipt = change_method_flags(negative / "fixture/Core.class", b"(Ljava/lang/Object;)I",
                                              0x1041, 0x41, name=b"read")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Inlined interface admitted changed complete metadata or original flags: " + label)
        if args.recursive_bound:
            raw_sources = work / "raw-bound-java"
            shutil.copytree(fixture, raw_sources)
            for extra in (FIXTURES / "checked_inlined_recursive_bounds/java/fixture").glob("*.java"):
                shutil.copy2(extra, raw_sources / "fixture" / extra.name)
            raw_core = raw_sources / "fixture/Core.java"
            content = raw_core.read_text()
            if content.count("implements Api<Object>") != 1:
                raise RuntimeError("Missing exact independent Object-parameter source declaration")
            raw_core.write_text(content.replace("implements Api<Object>", "implements Api", 1))
            compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                       str(work / "raw-bound-compiler"), *map(str, sorted(raw_sources.rglob("*.java")))],
                                      text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "raw-bound-compiler.log").write_text(compiler.stdout + compiler.stderr)
            if compiler.returncode == 0 or "not within bounds" not in compiler.stderr:
                raise RuntimeError("Independent javac accepted a raw interface as the recursive lower-bound argument")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 1, "value_cases": counts[0], "identity_cases": counts[1],
                    "state_cases": counts[2], "failure_cases": counts[3],
                    "profile": "dependency-root" if args.dependency_root else "source-root",
                    "recursive_lower_bounds": "complete original scopes retained" if args.recursive_bound else "not in this profile",
                    "original_inlined_interface_body": "retained without a new typed runtime entry",
                    "ordered_cast_argument_and_receiver_effects": "matched",
                    "complete_original_metadata_and_runtime_inventory": "unchanged except reported Object interface argument, flags and helpers",
                    "same_code_and_descriptor_counterexamples": "complete argument drift and missing original synthetic flag rejected",
                    "original_and_immutable_harness_hashes": original_hashes,
                    "harness_hashes": harness_hashes}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
