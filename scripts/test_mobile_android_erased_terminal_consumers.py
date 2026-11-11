#!/usr/bin/env python3
"""Verify parameter-independent erased throws, raw source members and complete original contracts."""
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
        prefix="neverd-erased-terminal-consumers-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture_name = "erased_terminal_consumers"
        harness_name = "ErasedTerminalConsumersHarness"
        fixture = FIXTURES / fixture_name / "java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness" / (harness_name + ".java")], harness,
                             classpath=originals)
        owner = "Lfixture/Core;"
        runtime_method = owner + "->add(Ljava/lang/Object;)Z"
        source_signature = b"Ljava/lang/Object;Lfixture/Api<Ljava/lang/Object;>;Lfixture/Marker;"
        input_signature = b"Ljava/lang/Object;Lfixture/Api<Lfixture/Box;>;Lfixture/Marker;"
        receipts = [change_signature(originals / "fixture/Core.class", source_signature, input_signature),
                    change_signature(originals / "fixture/Core.class", b"(Ljava/util/Collection<+Ljava/lang/Object;>;)Z", b"(Ljava/util/Collection<+Lfixture/Box;>;)Z"),
                    change_signature(originals / "fixture/Core.class", b"()Ljava/util/Iterator<Ljava/lang/Object;>;", b"()Ljava/util/Iterator<Lfixture/Box;>;"),
                    change_method_flags(originals / "fixture/Core.class", b"(Ljava/lang/Object;)Z", 0x1, 0x1041, name=b"add")]
        source_members = {owner + "->bulk(Ljava/util/Collection;)Z": ("(Ljava/util/Collection<+Lfixture/Box;>;)Z", "(Ljava/util/Collection;)Z", "Lfixture/Api;->bulk(Ljava/util/Collection;)Z"),
                          owner + "->containsAll(Ljava/util/Collection;)Z": ("(Ljava/util/Collection<*>;)Z", "(Ljava/util/Collection;)Z", "Lfixture/Api;->containsAll(Ljava/util/Collection;)Z"),
                          owner + "->echo(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;": ("<R:Ljava/lang/Object;S:Ljava/lang/Object;>(TR;TS;)TR;", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", "Lfixture/Api;->echo(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"),
                          owner + "->array([Ljava/lang/Object;)[Ljava/lang/Object;": ("<R:Ljava/lang/Object;>([TR;)[TR;", "([Ljava/lang/Object;)[Ljava/lang/Object;", "Lfixture/Api;->array([Ljava/lang/Object;)[Ljava/lang/Object;")}
        (work / "original-erased-terminal-encoding.json").write_text(json.dumps(receipts, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Api;", "Lfixture/Parent;"} if args.dependency_root else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        dependency = work / "owned-root.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Api", "Parent"):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_root:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_name, "original"], "baseline")
        suffix = "parameter-independent-throw-and-original-runtime-slots=retained,full-other-contracts-effects-and-return-metadata=checked,raw-interface-and-member-metadata=reported"
        lines = baseline.splitlines()
        matched = re.fullmatch(r"values=(\d+),identities=(\d+),states=(\d+),failures=(\d+)", lines[0]) if len(lines) == 2 else None
        if not matched or lines[1] != suffix:
            raise RuntimeError("Owned erased terminal baseline missed a result, effect or failure case: " + baseline)
        counts = tuple(map(int, matched.groups()))
        if counts != (174, 282, 426, 102):
            raise RuntimeError("Owned erased terminal consumer baseline changed its independent case matrix: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if not result.is_file() or list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned erased terminal consumer fixture did not produce one DEX")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the original erased terminal consumer runtime inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-erased-terminal-bridge-disassembly")
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
            if (identity in witnesses or not flags or int(flags[1], 16) != 0x1041
                    or opcodes != ["new-instance", "const-string", "invoke-direct", "throw"]
                    or "Ljava/lang/UnsupportedOperationException;" not in declaration
                    or "Ljava/lang/UnsupportedOperationException;.<init>:(Ljava/lang/String;)V" not in declaration):
                raise RuntimeError("D8 changed the parameter-independent original allocation and throw")
            witnesses[identity] = opcodes
        if set(witnesses) != {runtime_method}:
            raise RuntimeError("Missing complete original erased terminal opcode witnesses")
        (work / "original-erased-terminal-bridge-opcodes.json").write_text(json.dumps(witnesses, indent=2) + "\n")

        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projections = coverage.get("generic_signature_projections", [])
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 5 or coverage["status"] != "partial"
                or coverage.get("class_source_bindings") or len(projections) != 5
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != {runtime_method, *source_members}):
            raise RuntimeError("Erased terminal consumer lost original runtime inventory or projection scope")
        by_declaration = {row["declaration"]: row for row in projections}
        if set(by_declaration) != {owner, *source_members}:
            raise RuntimeError("Unexpected source Signature erasure declarations")
        projection = by_declaration[owner]
        changes = projection.get("changes", [])
        if (projection.get("declaration") != owner or projection.get("original_signature") != input_signature.decode()
                or len(changes) != 1 or changes[0].get("kind") != "erased-terminal-consumer-raw-interface"
                or changes[0].get("source_type") != "Lfixture/Api;" or changes[0].get("scope_origin") != runtime_method
                or rows[runtime_method].get("projection_kind") != "erased-terminal-consumer-bridge"
                or rows[runtime_method].get("inherited_contract") != "Lfixture/Api;->add(Ljava/lang/Object;)Z"
                or rows[runtime_method].get("source_identity") != runtime_method
                or rows[runtime_method].get("bridge_target") or not rows[runtime_method].get("metadata_differences")):
            raise RuntimeError("Erased terminal consumer lost its original complete edge, throw body or contract")
        for identity, (original, source, inherited) in source_members.items():
            row = rows[identity]
            metadata = by_declaration[identity]
            if (metadata.get("original_signature") != original or len(metadata.get("changes", [])) != 1
                    or metadata["changes"][0].get("kind") != "erased-terminal-raw-member"
                    or metadata["changes"][0].get("source_type") != source):
                raise RuntimeError("Raw member source and body do not share one Signature receipt")
            if (row.get("projection_kind") != "erased-terminal-raw-member"
                    or row.get("inherited_class_contract") != inherited
                    or row.get("original_method_signature") != original
                    or row.get("source_generic_signature") != source
                    or row.get("source_identity") != identity or not row.get("metadata_differences")):
                raise RuntimeError("Raw source member omitted its original full Signature or disclosed source erasure: " + identity)
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_root else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(methods) | helpers:
            raise RuntimeError("Recovered erased terminal consumer invented a bridge or runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                wanted = None if identity == owner and key == "signature" else before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Original erased terminal consumer class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if method == runtime_method:
                    if wanted["access"] != 0x1041:
                        raise RuntimeError("Original erased terminal consumer lost bridge/synthetic flags")
                    wanted["access"] = 0x1
                if method in source_members:
                    wanted["signature"] = None
                if after["methods"][method] != wanted:
                    raise RuntimeError("Original erased terminal consumer method metadata changed beyond reported flags: " + method)
        rebuilt_path = (rebuilt, harness, dependency) if args.dependency_root else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, rebuilt_path)),
                             harness_name, "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Direct, interface, reflected or MethodHandle scalar dispatch changed")

        for label in ("full-bulk-contract-drift", "missing-synthetic-flag"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "full-bulk-contract-drift":
                receipt = change_signature(negative / "fixture/Core.class", b"(Ljava/util/Collection<+Lfixture/Box;>;)Z", b"(Ljava/util/Collection<*>;)Z")
            else:
                receipt = change_method_flags(negative / "fixture/Core.class", b"(Ljava/lang/Object;)Z", 0x1041, 0x41, name=b"add")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Erased terminal consumer admitted a changed full contract or flags: " + label)
        negative_sources = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "Core.java":
                content = content.replace("implements Api<Object>", "implements Api<Box>")
                content = content.replace("Iterator<Object>", "Iterator<Box>")
                content = content.replace("add(Object input)", "add(Box input)")
                content = content.replace("bulk(Collection<? extends Object> input)", "bulk(Collection<?> input)")
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
                    "projected_methods": 5, "value_cases": counts[0], "identity_cases": counts[1],
                    "state_cases": counts[2], "failure_cases": counts[3],
                    "profile": "dependency-root" if args.dependency_root else "source-root",
                    "parameter_independent_original_allocation_and_throw": "retained without a new typed entry or cast",
                    "original_full_inheritance_and_member_contracts": "checked independently of raw source metadata",
                    "source_metadata_differences": "raw interface, four raw member Signatures and bridge flags",
                    "closed_iterator_result_and_other_class_metadata": "unchanged",
                    "wrong_type_null_array_aliases_effects_and_ordered_failures": "matched",
                    "same_code_and_descriptor_controls": "full bulk contract drift and missing synthetic flag rejected",
                    "independent_javac_contract_control": "rejected", "original_and_harness_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
