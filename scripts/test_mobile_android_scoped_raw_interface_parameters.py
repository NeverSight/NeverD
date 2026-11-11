#!/usr/bin/env python3
"""Verify scoped raw parameters through a complete original interface diamond."""
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

BRIDGE = "Lfixture/Core;->matches(Ljava/lang/Object;)Z"
TARGET = "Lfixture/Core;->matches(Lfixture/Bound;)Z"
PARENTS = {"Lfixture/" + name + ";" for name in ("Root", "Left", "Right", "Api")}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-interfaces", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-scoped-raw-interface-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "scoped_raw_interface_parameters" / "java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("ScopedRawInterfaceHarness", "ScopedRawInterfaceCaller")], harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 8 or len(all_methods) != 13:
            raise RuntimeError("Owned original interface fixture inventory changed")
        classes = all_classes - (PARENTS if args.dependency_interfaces else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)

        def jar(root: Path, destination: Path) -> None:
            with zipfile.ZipFile(destination, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                for owner in sorted(PARENTS):
                    name = owner[1:-1] + ".class"
                    archive.writestr(name, (root / name).read_bytes())

        dependency = work / "owned-interfaces.jar"
        jar(originals, dependency)
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_interfaces:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                              "ScopedRawInterfaceHarness"], "baseline")
        if baseline.splitlines() != ["values=73,identities=156,states=161,failures=56",
                "original-interface-diamond-and-class-scope=retained,raw-target-and-bridge-metadata=retained,external-overrides-and-ordered-effects=retained"]:
            raise RuntimeError("Owned scoped interface matrix changed: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned scoped interface fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original scoped interface runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-bridge-disassembly")
        witnesses = []
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            prototype = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if not name or not prototype or block[1] + "->" + name[1] + prototype[1] != BRIDGE:
                continue
            flags = re.search(r"(?m)^\s+access\s+: 0x([0-9a-fA-F]+)", block[2])
            ops = [op.removesuffix("/range") for op in re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])]
            if not flags or int(flags[1], 16) != 0x1041 or ops != ["check-cast", "invoke-virtual", "move-result", "return"]:
                raise RuntimeError("D8 changed the original scoped checked bridge")
            witnesses.append(ops)
        if len(witnesses) != 1:
            raise RuntimeError("Missing exact original scoped bridge witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count") != 1
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != {BRIDGE}
                or coverage["status"] != "partial" or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections")
                or rows[BRIDGE].get("projection_kind") != "generic-interface-bridge"
                or rows[BRIDGE].get("bridge_target") != TARGET):
            raise RuntimeError("Scoped interface recovery changed runtime entries or original generic contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_interfaces else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {"Lfixture/" + name + ";->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for name in ("Core", "Caller", "Bound", "Leaf")}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Scoped interface recovery changed an original entry or added an unexpected helper")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"][method] != metadata or actual_exceptions[method] != exceptions[method]:
                    raise RuntimeError("Original method metadata or Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_interfaces else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             "ScopedRawInterfaceHarness"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Scoped interface dispatch, mutable aliases or exception identity changed")
        for label in ("missing-synthetic", "class-bound-drift", "diamond-binding"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            target = negative / "fixture/Core.class"
            if label == "missing-synthetic":
                receipt = change_method_flags(target, b"(Ljava/lang/Object;)Z", 0x1041, 0x41, name=b"matches")
            elif label == "class-bound-drift":
                receipt = change_signature(target, b"<T:Lfixture/Bound;>Ljava/lang/Object;Lfixture/Api<TT;>;",
                    b"<T:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Api<TT;>;")
            else:
                receipt = change_signature(negative / "fixture/Right.class",
                    b"<R:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Root<TR;>;",
                    b"<R:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Root<Lfixture/Bound;>;")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_classpath = list(classpath)
            if args.dependency_interfaces:
                negative_dependency = work / (label + "-declarations.jar")
                jar(negative, negative_dependency)
                negative_classpath[-1] = negative_dependency
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, negative_classpath),
                                     "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Scoped interface accepted an unproved original contract: " + label)
        negative_sources = work / "javac-negative"
        shutil.copytree(fixture, negative_sources)
        target = negative_sources / "fixture/Core.java"
        text = target.read_text(); assert text.count("T extends Bound") == 1
        target.write_text(text.replace("T extends Bound", "T extends Object"))
        result = subprocess.run([str(verify.javac), "--release", "8", "-proc:none", "-d", str(work / "javac-negative-classes"),
                                 *map(str, sorted(negative_sources.rglob("*.java")))], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "javac-negative.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "does not override" not in result.stderr:
            raise RuntimeError("Independent javac accepted a widened class bound with the original raw target")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original inputs, immutable callers or dependency bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "values": 73, "identities": 156, "states": 161, "failures": 56,
                    "dependency_interfaces": args.dependency_interfaces,
                    "original_class_and_method_metadata_and_exceptions": "unchanged",
                    "original_bridge_and_external_overrides": "matched",
                    "same_code_counterexamples": "rejected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
