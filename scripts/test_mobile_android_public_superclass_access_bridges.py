#!/usr/bin/env python3
"""Verify original public access bridges through nonpublic generic superclasses."""
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
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
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
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-public-superclass-access-bridges-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture_name = "public_superclass_access_bridges"
        harness_name = "PublicSuperclassAccessBridgesHarness"
        fixture = FIXTURES / fixture_name / "java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness" / (harness_name + ".java")], harness,
                             classpath=originals)
        owner = "Lfixture/Core;"
        bridge_returns = {"write": "V", "flag": "Z", "scalar": "I", "wide": "J", "decimal": "D", "reference": "Ljava/lang/Object;"}
        bridge_methods = {owner + "->" + name + "(Ljava/lang/Object;IJ)" + result for name, result in bridge_returns.items()}
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Base;"} if args.dependency_root else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        dependency = work / "owned-root.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("fixture/Base.class", (originals / "fixture/Base.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_root:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_name, "original"], "baseline")
        expected = ["values=368,identities=548,states=1928,failures=120",
                    "original-public-access-bridges=retained,nearest-super-dispatch-and-complete-scopes=retained,bridge-flag-differences=reported"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned public superclass access bridge baseline missed a dispatch, identity or failure case: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if not result.is_file() or list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned public superclass access bridge fixture did not produce one DEX")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the original public superclass access bridge runtime inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-access-bridge-disassembly")
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
            if identity not in bridge_methods:
                continue
            opcodes = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", declaration)
            if (identity in witnesses or not flags or int(flags[1], 16) != 0x1041
                    or not any(op in ("invoke-super", "invoke-super/range") for op in opcodes)
                    or len(opcodes) != (2 if prototype[1].endswith("V") else 3)):
                raise RuntimeError("D8 changed an original exact public superclass access bridge: " + identity)
            witnesses[identity] = opcodes
        if set(witnesses) != bridge_methods:
            raise RuntimeError("Original public access bridge opcode witness missed a runtime entry")
        (work / "original-access-bridge-opcodes.json").write_text(json.dumps(witnesses, indent=2) + "\n")

        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 6 or coverage["status"] != "partial"
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != bridge_methods):
            raise RuntimeError("Scoped plain ancestor contract changed original runtime inventory, scopes or projection receipt")
        for identity in bridge_methods:
            row = rows[identity]
            if (row.get("projection_kind") != "public-superclass-access-bridge"
                    or row.get("inherited_contract") != identity.replace(owner, "Lfixture/Root;", 1)
                    or not row.get("metadata_differences")):
                raise RuntimeError("Original public access bridge lost its nearest original declaration or flag receipt: " + identity)
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_root else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(methods) | helpers:
            raise RuntimeError("Recovered public superclass access bridge invented a bridge or runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                wanted = before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Original public superclass access bridge class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                expected_metadata = dict(metadata)
                if method in bridge_methods:
                    if expected_metadata["access"] != 0x1041:
                        raise RuntimeError("Original javac access bridge flags changed")
                    expected_metadata["access"] &= ~0x1040
                if after["methods"][method] != expected_metadata:
                    raise RuntimeError("Original public superclass access bridge method scope, access or throws changed: " + method)
        rebuilt_path = (rebuilt, harness, dependency) if args.dependency_root else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, rebuilt_path)),
                             harness_name, "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Direct, inherited, reflected or SDK public superclass access bridge execution changed")

        negative = work / "final-ancestor-originals"
        shutil.copytree(originals, negative)
        receipt = change_method_flags(negative / "fixture/Root.class",
            b"(Ljava/lang/Object;IJ)I", 0x1, 0x11, name=b"scalar")
        (work / "final-ancestor-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = dex(negative, work / "final-ancestor-dex")
        rejected = work / "final-ancestor-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                 *map(str, classpath), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "final-ancestor-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists():
            raise RuntimeError("Public access bridge admitted an original final ancestor override")
        negative_sources = work / "final-ancestor-java"
        shutil.copytree(fixture, negative_sources)
        root_source = negative_sources / "fixture/Root.java"
        content = root_source.read_text()
        declaration = "public int scalar(Object marker, int number, long word)"
        if content.count(declaration) != 1:
            raise RuntimeError("Missing exact independent final ancestor declaration")
        root_source.write_text(content.replace(declaration, "public final int scalar(Object marker, int number, long word)", 1))
        core_source = negative_sources / "fixture/Core.java"
        content = core_source.read_text()
        explicit_bridge = "    @Override public int scalar(Object marker, int number, long word) { return super.scalar(marker, number, word); }\n"
        offset = content.rfind("}")
        if offset < 0:
            raise RuntimeError("Missing original Core class source boundary")
        core_source.write_text(content[:offset] + explicit_bridge + content[offset:])
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "final-ancestor-compiler"), *map(str, sorted(negative_sources.rglob("*.java")))], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "final-ancestor-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or "final" not in compiler.stderr:
            raise RuntimeError("Independent javac accepted the original public delegation over a final ancestor")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 6, "value_cases": 368, "identity_cases": 548,
                    "state_cases": 1928, "failure_cases": 120,
                    "profile": "dependency-root" if args.dependency_root else "source-root",
                    "complete_class_scopes_and_original_nearest_super_dispatch": "retained",
                    "original_public_access_bridges": "retained with reported bridge/synthetic flag differences",
                    "mutable_aliases_virtual_dispatch_and_ancestor_failures": "matched",
                    "original_metadata_and_runtime_inventory_apart_from_reported_bridge_flags": "unchanged except reported helpers",
                    "same_code_final_ancestor_counterexample": "rejected by NeverD and independent javac",
                    "input_harness_and_sdk_bytes": "unchanged", "coverage": coverage["status"]}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
