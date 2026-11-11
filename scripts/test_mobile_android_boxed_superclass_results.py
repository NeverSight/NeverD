#!/usr/bin/env python3
"""Verify original indexed scalar factories through complete generic superclass contracts."""
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
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_nested_return import change_signature

PROTO = "(I)Ljava/lang/Object;"
METHOD = "Lfixture/Core;->get" + PROTO
INPUT_SIGNATURE = "Lfixture/Parent<Lfixture/Box;>;Lfixture/Marker;"
SOURCE_SIGNATURE = "Lfixture/Parent<Ljava/lang/Object;>;Lfixture/Marker;"


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
    parser.add_argument("--dependency-parent", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-boxed-superclass-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "boxed_superclass_results/java/fixture"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/BoxedSuperclassResultsHarness.java"], harness, classpath=originals)
        edits = [change_signature(originals / "fixture/Core.class", SOURCE_SIGNATURE.encode(), INPUT_SIGNATURE.encode()),
                 bridge_flag(originals / "fixture/Core.class", "get", PROTO, original_flags=1, input_flags=0x1041)]
        (work / "original-boxed-superclass-encoding.json").write_text(json.dumps(edits, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        declarations = {"Lfixture/Api;", "Lfixture/Base;", "Lfixture/Parent;"}
        classes = all_classes - (declarations if args.dependency_parent else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        dependency = work / "owned-parents.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Api", "Base", "Parent"):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_parent:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "BoxedSuperclassResultsHarness", "original"], "baseline")
        lines = baseline.splitlines()
        match = re.fullmatch(r"values=(\d+),identities=(\d+),states=(\d+),failures=(\d+)", lines[0]) if len(lines) == 2 else None
        counts = tuple(map(int, match.groups())) if match else ()
        if counts != (330, 1036, 820, 42) or lines[1] != "original-indexed-target-and-factory=retained,mutable-aliases-and-inherited-callback-order=retained,object-superclass-argument-and-bridge-flags=reported":
            raise RuntimeError("Owned indexed superclass case matrix changed: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned indexed superclass fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an indexed superclass original runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-indexed-factory-disassembly")
        witnesses = []
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            declaration = block[2]
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", declaration)
            prototype = re.search(r"(?m)^\s+type\s+: '([^']+)'", declaration)
            if not name or not prototype or block[1] + "->" + name[1] + prototype[1] != METHOD:
                continue
            flags = re.search(r"(?m)^\s+access\s+: 0x([0-9a-fA-F]+)", declaration)
            opcodes = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", declaration)
            if (not flags or int(flags[1], 16) != 0x1041 or len(opcodes) != 5
                    or opcodes[0] not in ("invoke-virtual", "invoke-virtual/range")
                    or opcodes[1] != "move-result-wide"
                    or opcodes[2] not in ("invoke-static", "invoke-static/range")
                    or opcodes[3:] != ["move-result-object", "return-object"]
                    or "Lfixture/Core;.readValue:(I)J" not in declaration
                    or "Lfixture/Box;.box:(J)Lfixture/Box;" not in declaration):
                raise RuntimeError("D8 changed the original indexed scalar/factory body")
            witnesses.append(opcodes)
        if len(witnesses) != 1:
            raise RuntimeError("Missing unique original indexed factory body witness")
        (work / "original-indexed-factory-opcodes.json").write_text(json.dumps(witnesses, indent=2) + "\n")

        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        projections = coverage.get("generic_signature_projections", [])
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 1 or projected != {METHOD}
                or coverage["status"] != "partial" or len(projections) != 1
                or coverage.get("class_source_bindings")):
            raise RuntimeError("Boxed superclass changed inventory or an unrelated scope: " + str(projected))
        row = rows[METHOD]
        changes = projections[0].get("changes", [])
        if (row.get("projection_kind") != "boxed-generic-superclass-result-bridge"
                or row.get("inherited_class_contract") not in {owner + "->get" + PROTO for owner in declarations}
                or row.get("source_identity") != METHOD
                or row.get("bridge_target") != "Lfixture/Core;->readValue(I)J"
                or row.get("box_method") != "Lfixture/Box;->box(J)Lfixture/Box;"
                or not row.get("metadata_differences") or projections[0].get("declaration") != "Lfixture/Core;"
                or projections[0].get("original_signature") != INPUT_SIGNATURE or len(changes) != 1
                or changes[0].get("kind") != "boxed-superclass-result-argument"
                or changes[0].get("source_type") != "Lfixture/Parent<Ljava/lang/Object;>;"
                or changes[0].get("scope_origin") != METHOD):
            raise RuntimeError("Boxed superclass lost its full original edge, indexed target or factory")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_parent else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {entry["class"] + "->" + entry["name"] + entry["prototype"]
                   for entry in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != methods | helpers:
            raise RuntimeError("Rebuilt indexed superclass invented or renamed a runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                wanted = SOURCE_SIGNATURE if identity == "Lfixture/Core;" and key == "signature" else before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Original class or field metadata changed beyond reported superclass argument: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if method == METHOD:
                    if wanted["access"] != 0x1041:
                        raise RuntimeError("Original scalar factory lost bridge/synthetic flags")
                    wanted["access"] = 1
                if after["methods"][method] != wanted:
                    raise RuntimeError("Metadata changed beyond reported flags: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_parent else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             "BoxedSuperclassResultsHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original indexed target, factory, mutation, aliases or exception order changed")
        for label in ("complete-argument-drift", "missing-synthetic-flag"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "complete-argument-drift":
                receipt = change_signature(negative / "fixture/Core.class", INPUT_SIGNATURE.encode(),
                    b"Lfixture/Parent<Ljava/lang/String;>;Lfixture/Marker;")
            else:
                receipt = change_method_flags(negative / "fixture/Core.class", PROTO.encode(), 0x1041, 0x41, name=b"get")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Boxed superclass admitted different complete original types or flags: " + label)
        wrong_java = work / "wrong-java/fixture/Wrong.java"
        wrong_java.parent.mkdir(parents=True)
        wrong_java.write_text("package fixture; public final class Wrong extends Parent<String> {\n"
                             "  public Box get(int index) { return Box.box(index); }\n}\n")
        rejected = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-classpath", str(originals),
                                   "-d", str(work / "wrong-compiled"), str(wrong_java)],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-javac.log").write_text(rejected.stdout + rejected.stderr)
        if rejected.returncode == 0:
            raise RuntimeError("Independent javac accepted a different complete superclass result")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness, ancestors or SDK changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": coverage["projected_method_count"], "value_cases": counts[0],
                    "identity_cases": counts[1], "state_cases": counts[2], "failure_cases": counts[3],
                    "profile": "dependency-parent" if args.dependency_parent else "source-parent",
                    "original_runtime_slots_and_bodies": "unchanged",
                    "source_superclass_argument": "Object reported after complete original result and body proof",
                    "same_code_complete_argument_and_javac_counterexamples": "rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
