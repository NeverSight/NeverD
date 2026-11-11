#!/usr/bin/env python3
"""Verify raw scalar bridges retain the original getter and complete generic tail."""
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
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags


BRIDGE = "Lfixture/Core;->emit(Ljava/lang/Object;Lfixture/Sink;)Ljava/lang/Object;"
CONTRACT = "Lfixture/Api;->emit(Ljava/lang/Object;Lfixture/Sink;)Ljava/lang/Object;"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--narrow-scalar", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-raw-scalar-tail-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        scalar = "I" if args.narrow_scalar else "J"
        target = f"Lfixture/Core;->emitTyped({scalar}Lfixture/Sink;)Ljava/lang/Object;"
        typed_signature = f"({scalar}Lfixture/Sink<-Lfixture/Token;>;)Ljava/lang/Object;".encode()
        fixture = FIXTURES / "raw_scalar_tail_bridges/java"

        def text(path):
            source = path.read_text()
            if args.narrow_scalar:
                source = source.replace("0x7fedcba987654321L", "0x76543210").replace("Long.MIN_VALUE", "Integer.MIN_VALUE")
                source = re.sub(r"\blong\b", "int", source)
                source = re.sub(r"\b([0-9]+)L\b", r"\1", source)
            return source

        inputs, observers = [], []
        for source in sorted(fixture.rglob("*.java")):
            destination = work / "input-java" / source.relative_to(fixture)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.open("x").write(text(source))
            inputs.append(destination)
        for name in ("RawScalarTailCaller", "RawScalarTailHarness"):
            destination = work / (name + ".java")
            destination.open("x").write(text(FIXTURES / "harness" / (name + ".java")))
            observers.append(destination)
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals, release=8)
        verify.compile_local(observers, harness, classpath=originals, release=8)
        harness_hashes = hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()

        def execute(runtime, label, bridge):
            outputs = []
            for route in range(5):
                for mode in range(16):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "RawScalarTailHarness", str(route), str(mode), str(bridge).lower()], f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Original scalar bridge behavior matrix changed: " + output)
                    outputs.append(output)
            return outputs

        compiler_baseline = execute((originals, harness), "compiler-baseline", False)
        # The independent source already implements the raw API. Encode only
        # the original entry's flags; retain every actual Code byte/attribute.
        receipt = change_method_flags(originals / "fixture/Core.class",
            b"(Ljava/lang/Object;Lfixture/Sink;)Ljava/lang/Object;", 0x1, 0x1041, name=b"emit")
        (work / "input-access-change.json").open("x").write(json.dumps(receipt, indent=2) + "\n")
        original_hashes = hashes(originals)
        baseline = execute((originals, harness), "encoded-baseline", True)
        if baseline != compiler_baseline:
            raise RuntimeError("Bridge access encoding changed original execution")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 6 or len(all_methods) != 10:
            raise RuntimeError("Owned raw scalar bridge fixture inventory changed")
        interfaces = {"Lfixture/Api;", "Lfixture/Sink;"}
        providers = interfaces | {"Lfixture/Scalar;"}
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def dex(root, destination):
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned raw scalar bridge changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original raw scalar runtime entry")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-bridge-disassembly")
        witnessed = []
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            descriptor = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if not name or not descriptor or block[1] + "->" + name[1] + descriptor[1] != BRIDGE:
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
            flags = re.search(r"(?m)^\s+access\s+: 0x([0-9a-fA-F]+)", block[2])
            body = ops[1:] if ops and ops[0][0] == "move-object" else ops
            if (not flags or int(flags[1], 16) != 0x1041
                    or [op for op, _ in body] != ["check-cast", "invoke-virtual",
                        "move-result" if args.narrow_scalar else "move-result-wide",
                        "invoke-virtual", "move-result-object", "return-object"]
                    or "Lfixture/Scalar;" not in body[0][1]
                    or f"Lfixture/Scalar;.decode:(){scalar}" not in body[1][1]
                    or f"Lfixture/Core;.emitTyped:({scalar}Lfixture/Sink;)Ljava/lang/Object;" not in body[3][1]):
                raise RuntimeError("javac/D8 changed the original scalar bridge witness")
            witnessed.append(ops)
        if len(witnessed) != 1:
            raise RuntimeError("Missing original raw scalar bridge opcode witness")
        (work / "original-bridge-opcodes.json").open("x").write(json.dumps(witnessed, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 1 or coverage["status"] != "partial"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != {BRIDGE}
                or rows[BRIDGE].get("projection_kind") != "raw-parent-parameter-forwarding-bridge"
                or rows[BRIDGE].get("inherited_class_contract") != CONTRACT
                or rows[BRIDGE].get("bridge_target") != target
                or rows[BRIDGE].get("source_identity") != BRIDGE
                or not rows[BRIDGE].get("metadata_differences")):
            raise RuntimeError("Scalar bridge recovery changed original inventory or full contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes - interfaces}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Scalar bridge recovery invented an entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original scalar bridge class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if method == BRIDGE:
                    if wanted["access"] != 0x1041:
                        raise RuntimeError("Original scalar bridge flags changed")
                    wanted["access"] = 0x1
                if after["methods"].get(method) != wanted or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original scalar bridge method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt", False) != baseline:
            raise RuntimeError("Scalar bridge changed an original effect or result")
        for label, replacement in (("tail-variance", b"+Lfixture/Token;"), ("tail-argument", b"-Lfixture/Scalar;")):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Core.class", typed_signature,
                                       typed_signature.replace(b"-Lfixture/Token;", replacement))
            (work / (label + "-metadata.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Scalar bridge proof admitted changed complete tail metadata: " + label)
        source = text(fixture / "fixture/Core.java")
        forward = "return emitTyped(((Scalar)value).decode(), sink);"
        if source.count(forward) != 1:
            raise RuntimeError("Owned scalar bridge counterexample source changed")
        wrong_bodies = {
            "missing-getter": "return emitTyped(Trace.bits, sink);",
            "duplicate-getter": "((Scalar)value).decode(); " + forward,
            "wrong-cast": "return emitTyped(((Scalar)(Object)(Token)value).decode(), sink);",
            "duplicate-target": "emitTyped(((Scalar)value).decode(), sink); " + forward,
            "missing-tail": "return emitTyped(((Scalar)value).decode(), null);",
            "changed-scalar": "return emitTyped(((Scalar)value).decode() + 1, sink);",
            "missing-target": "((Scalar)value).decode(); return Trace.result;",
            "early-result": "Object result = Trace.result; emitTyped(((Scalar)value).decode(), sink); return result;",
        }
        for label, body in wrong_bodies.items():
            wrong = work / label / "fixture/Core.java"
            wrong.parent.mkdir(parents=True)
            wrong.open("x").write(source.replace(forward, body))
            wrong_classes = work / (label + "-classes")
            verify.compile_local([wrong], wrong_classes, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong_classes, originals, harness))),
                "RawScalarTailHarness", "1", "12" if label == "early-result" else "0", "false"],
                text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "AssertionError" not in result.stderr:
                raise RuntimeError("Immutable scalar bridge oracle missed a wrong program: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "dependency_provider": args.dependency_provider, "narrow_scalar": args.narrow_scalar,
                    "raw_class_signature_and_typed_tail": "unchanged", "compiler_code_bytes": "unchanged",
                    "original_getter_effects_scalar_bits_and_failure_order": "matched",
                    "original_return_and_exception_identity": "matched", "bridge_flags": "reported",
                    "complete_tail_mutations": "rejected", "independent_wrong_programs_detected": len(wrong_bodies),
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
