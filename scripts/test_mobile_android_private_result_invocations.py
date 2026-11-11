#!/usr/bin/env python3
"""Verify same-owner private result-only generic invocations through captured scopes."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-private-result-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "private_result_invocations/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("PrivateResultCaller", "PrivateResultDerived", "PrivateResultHarness")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 4 or len(all_methods) != 9:
            raise RuntimeError("Owned private result invocation inventory changed")
        providers = {"Lfixture/Provider;"}
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        if any(record["signature"] for record in records.values()):
            raise RuntimeError("Original private result invocation acquired a class Signature")
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                "PrivateResultHarness"], label)
            if output.splitlines() != ["calls=288,failures=248,private-dispatch-and-result-checks=matched"]:
                raise RuntimeError("Original private result matrix changed: " + output)
            return output

        baseline = execute((originals, harness), "baseline")
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned private result invocation fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed original private result invocation identities")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-private-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Owner$Worker;" or not name or name[1] not in {"raw", "narrow"}:
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            calls = [op for op in ops if op.startswith("invoke-")]
            if (calls != ["invoke-direct/range"] or ops.count("check-cast") != (name[1] == "narrow")
                    or "Lfixture/Owner$Worker;.read:" not in block[2]):
                raise RuntimeError("D8 changed the private dispatch or original result check")
            witnessed.add(name[1])
        if witnessed != {"raw", "narrow"}:
            raise RuntimeError("Missing original private invocation witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if (coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or {row["identity"] for row in coverage["methods"]} != methods
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage.get("minimum_java_release") != 25 or coverage.get("generic_signature_projections")):
            raise RuntimeError("Private result invocation recovery lost entries or invented generic metadata")
        bindings = coverage.get("class_source_bindings", [])
        capture = "Lfixture/Owner$Worker;->this$0:Lfixture/Owner;"
        if (len(bindings) != 1 or bindings[0]["binding_kind"] != "inner-to-static-member"
                or bindings[0].get("outer_capture_field") != capture
                or not any("synthetic capture-field" in text for text in bindings[0].get("metadata_differences", []))):
            raise RuntimeError("Missing precise captured ownership disclosure")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=25,
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Private result invocation recovery invented or lost runtime entries")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            expected_fields = json.loads(json.dumps(before["fields"]))
            expected_inner = dict(before["inner_class"]) if before["inner_class"] else None
            if owner == "Lfixture/Owner$Worker;":
                if expected_fields[capture]["access"] != 0x1010 or not expected_inner:
                    raise RuntimeError("Original captured field/ownership metadata changed")
                expected_fields[capture]["access"] &= ~0x1000
                expected_inner["access"] |= 8
            if after["fields"] != expected_fields or after["inner_class"] != expected_inner:
                raise RuntimeError("Captured scope changed beyond disclosed source metadata differences")
            for key in ("access", "signature", "superclass", "interfaces", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original private result invocation metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original private result invocation method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Private dispatch, actual receiver, argument bits or result checks changed")
        original_signature = b"<T:Ljava/lang/Object;>(Ljava/lang/String;[Ljava/lang/Class<*>;[Ljava/lang/Object;JD)TT;"
        for label, changed in (
            ("extra-formal", original_signature.replace(b"<T:", b"<U:Ljava/lang/Object;T:")),
            ("nonreifiable-input", original_signature.replace(b"Class<*>", b"Class<Ljava/lang/String;>")),
            ("input-formal", original_signature.replace(b"[Ljava/lang/Object;JD", b"[TT;JD")),
        ):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Owner$Worker.class", original_signature, changed)
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            encoded = work / (label + "-dex"); encoded.mkdir()
            inputs = [p for p in sorted(negative.rglob("*.class")) if "L" + p.relative_to(negative).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", encoded, *inputs], "d8-" + label)
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(encoded / "classes.dex"),
                *map(str, classpath), "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic invocation requires" not in result.stderr:
                raise RuntimeError("Private result proof admitted changed constraints: " + label)
        for label, before, after in (
            ("changed-receiver", "return other.", "return this."),
            ("skipped-result-check", "return other.<String>read(label, kinds, values, wide, real);",
             "other.<Object>read(label, kinds, values, wide, real); return null;"),
        ):
            wrong = work / label / "fixture/Owner.java"
            wrong.parent.mkdir(parents=True)
            text = (fixture / "fixture/Owner.java").read_text()
            if text.count(before) != (2 if label == "changed-receiver" else 1):
                raise RuntimeError("Independent private call counterexample anchor changed")
            wrong.write_text(text.replace(before, after))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     "PrivateResultHarness"], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "private result contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "runtime_calls": 288, "expected_failures": 248,
                    "dependency_provider": args.dependency_provider,
                    "coverage": "partial",
                    "class_signature_and_method_metadata_and_exceptions": "unchanged",
                    "captured_ownership_projection": "precisely disclosed",
                    "original_private_dispatch_and_result_checks": "matched",
                    "ordered_effects_exceptions_and_mutable_capture_identity": "matched",
                    "independent_receiver_and_check_counterexamples": "detected", "same_code_constraint_counterexamples": "rejected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
