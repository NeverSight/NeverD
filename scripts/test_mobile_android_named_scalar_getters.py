#!/usr/bin/env python3
"""Verify exact named scalar getter bridges and unchanged original interface contracts."""
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
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-named-scalar-getters-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture_name = "named_scalar_getters"
        harness_name = "NamedScalarGettersHarness"
        fixture = FIXTURES / fixture_name / "java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness" / (harness_name + ".java")], harness,
                             classpath=originals)
        owner = "Lfixture/Core;"
        getters = {"size": ("getSize", "I"), "span": ("getSpan", "J"),
                   "ratio": ("getRatio", "F"), "weight": ("getWeight", "D")}
        bridges = {owner + "->" + name + "()" + primitive:
                   (owner + "->" + target + "()" + primitive,
                    "Lfixture/Api;->" + name + "()" + primitive)
                   for name, (target, primitive) in getters.items()}
        flags_receipts = [change_method_flags(originals / "fixture/Core.class", ("()" + primitive).encode(),
                                             0x1, 0x1041, name=name.encode())
                          for name, (_, primitive) in getters.items()]
        (work / "original-named-scalar-bridge-encoding.json").write_text(
            json.dumps(flags_receipts, indent=2) + "\n")
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
        suffix = "original-scalar-targets-effects-bits-and-failure-order=retained,complete-interface-argument=unchanged,bridge-flags=reported"
        lines = baseline.splitlines()
        matched = re.fullmatch(r"values=(\d+),identities=(\d+),states=(\d+),failures=(\d+)", lines[0]) if len(lines) == 2 else None
        if not matched or lines[1] != suffix:
            raise RuntimeError("Owned named scalar baseline missed a result, effect or failure case: " + baseline)
        counts = tuple(map(int, matched.groups()))
        if counts != (401, 1991, 1272, 48):
            raise RuntimeError("Owned named scalar getter baseline changed its independent case matrix: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if not result.is_file() or list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned named scalar getter fixture did not produce one DEX")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the original named scalar getter runtime inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-named-scalar-bridge-disassembly")
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
            if identity not in bridges:
                continue
            opcodes = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", declaration)
            target, _ = bridges[identity]
            wide = prototype[1] in ("()J", "()D")
            if (identity in witnesses or not flags or int(flags[1], 16) != 0x1041
                    or len(opcodes) != 3 or opcodes[0] not in ("invoke-virtual", "invoke-virtual/range")
                    or opcodes[1:] != (["move-result-wide", "return-wide"] if wide else ["move-result", "return"])
                    or ("Lfixture/Core;." + target.split("->", 1)[1].replace("()", ":()")) not in declaration):
                raise RuntimeError("D8 changed the original receiver, named scalar call or exact result width")
            witnesses[identity] = opcodes
        if set(witnesses) != set(bridges):
            raise RuntimeError("Missing complete original named scalar opcode witnesses")
        (work / "original-named-scalar-bridge-opcodes.json").write_text(json.dumps(witnesses, indent=2) + "\n")

        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 4 or coverage["status"] != "partial"
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != set(bridges)):
            raise RuntimeError("Named scalar getter changed original inventory, complete Signature or scope")
        for identity, (target, inherited) in bridges.items():
            row = rows[identity]
            if (row.get("projection_kind") != "named-scalar-getter-bridge"
                    or row.get("inherited_class_contract") != inherited
                    or row.get("source_identity") != identity or row.get("bridge_target") != target
                    or not row.get("metadata_differences")):
                raise RuntimeError("Named scalar getter lost the original target, runtime slot or contract")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_root else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(methods) | helpers:
            raise RuntimeError("Recovered named scalar getter invented a bridge or runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                wanted = before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Original named scalar getter class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if method in bridges:
                    if wanted["access"] != 0x1041:
                        raise RuntimeError("Original named scalar getter lost bridge/synthetic flags")
                    wanted["access"] = 0x1
                if after["methods"][method] != wanted:
                    raise RuntimeError("Original named scalar getter method metadata changed beyond reported flags: " + method)
        rebuilt_path = (rebuilt, harness, dependency) if args.dependency_root else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, rebuilt_path)),
                             harness_name, "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Direct, interface, reflected or MethodHandle scalar dispatch changed")

        for label in ("method-formal-contract-drift", "missing-synthetic-flag"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "method-formal-contract-drift":
                signature = b"<R:Ljava/lang/Object;S:Ljava/lang/Object;>(TR;TS;)TR;"
                receipt = change_signature(negative / "fixture/Core.class", signature,
                                           signature.replace(b"(TR;TS;)", b"(TS;TS;)"))
            else:
                receipt = change_method_flags(negative / "fixture/Core.class", b"()I",
                                              0x1041, 0x41, name=b"size")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Named scalar getter admitted a changed full contract or flags: " + label)
        negative_sources = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "Core.java":
                original = "R echo(R input, S tag) { return input; }"
                assert content.count(original) == 1
                content = content.replace(original, "R echo(S input, S tag) { return (R) input; }")
            destination = negative_sources / "fixture" / source.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Independent javac accepted the changed complete method-formal contract")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 4, "value_cases": counts[0], "identity_cases": counts[1],
                    "state_cases": counts[2], "failure_cases": counts[3],
                    "profile": "dependency-root" if args.dependency_root else "source-root",
                    "original_named_scalar_targets_and_effectful_bodies": "retained",
                    "original_full_interface_signature_and_all_other_metadata": "unchanged except reported flags and helpers",
                    "primitive_bits_aliases_state_and_ordered_failures": "matched",
                    "same_code_and_descriptor_controls": "full method contract drift and missing synthetic flag rejected",
                    "independent_javac_contract_control": "rejected", "original_and_harness_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
