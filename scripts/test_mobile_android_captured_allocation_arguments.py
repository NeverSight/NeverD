#!/usr/bin/env python3
"""Verify argument effects during allocation of a proven captured member class."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-captured-allocation-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "captured_allocation_arguments/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("CapturedAllocationCaller", "CapturedAllocationHarness")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 12:
            raise RuntimeError("Owned captured allocation inventory changed")
        providers = {"Lfixture/Base;", "Lfixture/Input;"}
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        if any(record["signature"] for record in records.values()):
            raise RuntimeError("Original captured allocation acquired a class Signature")
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
            result = []
            for variant in range(2):
                for route in range(4):
                    for branch in range(2):
                        for mode in range(7):
                            output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                "CapturedAllocationHarness", str(variant), str(route), str(branch), str(mode)],
                                f"{label}-{variant}-{route}-{branch}-{mode}")
                            expected = f"variant={variant},route={route},branch={branch},mode={mode},attempt=[01],trace=[0-9]+"
                            if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                                raise RuntimeError("Original captured allocation matrix changed: " + output)
                            result.append(output)
            return result

        baseline = execute((originals, harness), "baseline")
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned captured allocation fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed original captured allocation identities")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-allocation-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if not name:
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            if block[1] == "Lfixture/Owner;" and name[1] in {"make", "conditional"}:
                if (ops[0] != "new-instance" or ops.count("new-instance") != 1
                        or ops.count("invoke-direct") != 1 or ops.count("invoke-virtual") != 1
                        or ops[-2:] != ["invoke-direct", "return-object"]
                        or sum(ops.count(op) for op in ("if-eqz", "if-nez")) != (name[1] == "conditional")):
                    raise RuntimeError("D8 changed the allocation argument witness: " + name[1])
                witnessed.add(name[1])
            elif block[1] == "Lfixture/Owner$Child;" and name[1] == "<init>":
                if ops[:2] != ["iput-object", "invoke-direct"]:
                    raise RuntimeError("D8 moved the original outer capture after superclass delegation")
                witnessed.add("capture-before-super")
        if witnessed != {"make", "conditional", "capture-before-super"}:
            raise RuntimeError("Missing original captured allocation witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if (coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or {row["identity"] for row in coverage["methods"]} != methods
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage.get("minimum_java_release") != 25 or coverage.get("generic_signature_projections")):
            raise RuntimeError("Captured allocation recovery lost entries or invented generic metadata")
        bindings = coverage.get("class_source_bindings", [])
        capture = "Lfixture/Owner$Child;->this$0:Lfixture/Owner;"
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
            raise RuntimeError("Captured allocation recovery invented or lost runtime entries")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            expected_fields = json.loads(json.dumps(before["fields"]))
            expected_inner = dict(before["inner_class"]) if before["inner_class"] else None
            if owner == "Lfixture/Owner$Child;":
                if expected_fields[capture]["access"] != 0x1010 or not expected_inner:
                    raise RuntimeError("Original captured field/ownership metadata changed")
                expected_fields[capture]["access"] &= ~0x1000
                expected_inner["access"] |= 8
            if after["fields"] != expected_fields or after["inner_class"] != expected_inner:
                raise RuntimeError("Captured scope changed beyond disclosed source metadata differences")
            for key in ("access", "signature", "superclass", "interfaces", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original captured allocation metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original captured allocation method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Outer capture, allocation initialization or ordered argument effects changed")
        for label, before, after in (
            ("changed-capture", "return Owner.this;", "return new Owner();"),
            ("hoisted-read", "return new Child(input.read(), tag);",
             "Object early = input.read(); return new Child(early, tag);"),
        ):
            wrong = work / label / "fixture/Owner.java"
            wrong.parent.mkdir(parents=True)
            text = (fixture / "fixture/Owner.java").read_text()
            if text.count(before) != 1:
                raise RuntimeError("Independent captured allocation counterexample anchor changed")
            wrong.write_text(text.replace(before, after))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     "CapturedAllocationHarness", "0", "1", "1", "0"], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "captured allocation contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "dependency_provider": args.dependency_provider,
                    "coverage": "partial",
                    "class_signature_and_method_metadata_and_exceptions": "unchanged",
                    "captured_ownership_projection": "precisely disclosed",
                    "capture_visible_to_superclass_callback": "matched",
                    "ordered_effects_exceptions_and_mutable_capture_identity": "matched",
                    "independent_capture_and_hoist_counterexamples": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
