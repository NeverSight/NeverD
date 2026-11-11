#!/usr/bin/env python3
"""Verify captured constructor arguments survive original incoming-register reuse."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
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


CAPTURED = {"Lfixture/Owner$Worker;", "Lfixture/Owner$WideWorker;"}
PROVIDERS = {"Lfixture/Base;", "Lfixture/Provider;", "Lfixture/Later;"}


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-captured-constructor-reads-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "captured_constructor_reads/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("CapturedConstructorReadCaller", "CapturedConstructorReadHarness")],
                             harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (8, 18):
            raise RuntimeError("Owned captured constructor inventory changed: " + repr((len(all_classes), len(all_methods))))
        classes = all_classes - (PROVIDERS if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for owner in sorted(PROVIDERS):
                relative = owner[1:-1] + ".class"
                archive.writestr(relative, (originals / relative).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            outputs = []
            for route in range(4):
                for mode in range(7):
                    for wide in range(2):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "CapturedConstructorReadHarness", str(route), str(mode), str(wide)],
                            f"{label}-{route}-{mode}-{wide}")
                        expected = f"route={route},mode={mode},wide={wide},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Original captured constructor oracle changed: " + output)
                        outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        dex = work / "dex"
        dex.mkdir()
        inputs = [path for path in sorted(originals.rglob("*.class"))
                  if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--release", "--min-api", "24", *classpath,
                    "--output", dex, *inputs], "d8-original")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned captured constructor changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original captured constructor runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-constructor-disassembly")
        witnessed, overwritten = {}, set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            prototype = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if block[1] not in CAPTURED or not name or name[1] != "<init>":
                continue
            registers = re.search(r"(?m)^\s+registers\s+: (\d+)", block[2])
            incoming = re.search(r"(?m)^\s+ins\s+: (\d+)", block[2])
            if not prototype or prototype[1] != "(Lfixture/Owner;Ljava/lang/Object;)V" or not registers or not incoming or int(incoming[1]) != 3:
                raise RuntimeError("Owned incoming constructor register inventory changed")
            outer = int(registers[1]) - int(incoming[1]) + 1
            ops = [(int(pc, 16), op, tail) for pc, op, tail in
                   re.findall(r"\|([0-9a-fA-F]+):\s+(\S+)([^\n]*)", block[2])]
            captures = [(pc, re.match(r"\s+v(\d+), v(\d+),", tail)) for pc, op, tail in ops
                        if op == "iput-object" and block[1] + ".this$0:" in tail]
            reads = [(pc, op, re.match(r"\s+v(\d+),", tail)) for pc, op, tail in ops
                     if op in {"sget", "sget-wide"} and "Lfixture/Provider;." in tail]
            calls = [pc for pc, op, tail in ops
                     if op in {"invoke-direct", "invoke-direct/range"} and "Lfixture/Base;.<init>:" in tail]
            if (len(captures) != 1 or len(reads) != 1 or len(calls) != 1 or not captures[0][1] or not reads[0][2]
                    or int(captures[0][1][1]) != outer or int(captures[0][1][2]) != outer - 1
                    or not captures[0][0] < reads[0][0] < calls[0]):
                raise RuntimeError("D8 lost the original capture-before-static-read and superclass call witness")
            destination = int(reads[0][2][1])
            reused = outer in range(destination, destination + (2 if reads[0][1] == "sget-wide" else 1))
            if reused:
                overwritten.add(block[1])
            witnessed[block[1]] = {"incoming_outer_register": outer, "static_read_register": destination,
                                   "static_read_opcode": reads[0][1], "outer_register_reused": reused}
        if set(witnessed) != CAPTURED or "Lfixture/Owner$Worker;" not in overwritten:
            raise RuntimeError("D8 did not produce the intended original outer-parameter reuse witness")
        (work / "capture-register-witnesses.json").write_text(json.dumps(witnessed, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {identity for identity in methods if identity.split("->", 1)[0] in CAPTURED}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage["status"] != "partial" or coverage["projected_method_count"] != 4
                or coverage.get("minimum_java_release") != 25 or coverage.get("generic_signature_projections")
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != projected
                or any(rows[identity].get("projection_kind") != "inner-to-static-member" for identity in projected)):
            raise RuntimeError("Captured constructor recovery lost original runtime entries or ownership")
        bindings = coverage.get("class_source_bindings", [])
        if (len(bindings) != 2 or {row["class"] for row in bindings} != CAPTURED
                or {row.get("outer_capture_field") for row in bindings} != {owner + "->this$0:Lfixture/Owner;" for owner in CAPTURED}
                or any(row.get("binding_kind") != "inner-to-static-member" or not row.get("metadata_differences") for row in bindings)):
            raise RuntimeError("Captured constructor source binding lost exact original outer fields")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=25,
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Captured constructor invented or lost an original runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = copy.deepcopy(records[owner]), actual_records[owner]
            if owner in CAPTURED:
                capture = before["fields"][owner + "->this$0:Lfixture/Owner;"]
                if capture["access"] != 0x1010 or not before["inner_class"]:
                    raise RuntimeError("Original capture metadata changed")
                capture["access"] &= ~0x1000
                before["inner_class"]["access"] |= 8
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original captured constructor class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after["methods"].get(identity) != metadata or actual_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Original captured constructor method metadata/Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Captured constructor changed snapshots, owner identity, effect order or failures")
        mutants = (
            ("wrong-owner", "Owner", "return Owner.this;", "return new Owner();", 2, 0),
            ("late-static-read", "Owner", "Trace.hit(4);", "Trace.hit(4); this.word = Provider.word; this.wide = Provider.wide;", 2, 0),
            ("missing-null", "Owner", ", null);", ", payload);", 2, 0),
            ("missing-postlude", "Owner", "Trace.hit(4);", "", 2, 0),
            ("changed-failure-identity", "Trace", "throw failure;", "throw new IllegalStateException();", 1, 3),
            ("delayed-capture", "Owner", "public Owner owner() { return Owner.this; }",
             "private Owner delayedOwner; public Owner owner() { return delayedOwner; }", 2, 0),
        )
        for label, name, anchor, replacement, count, mode in mutants:
            original = (fixture / "fixture" / (name + ".java")).read_text()
            if original.count(anchor) != count:
                raise RuntimeError("Owned captured constructor control anchor changed")
            source = work / label / "fixture" / (name + ".java")
            source.parent.mkdir(parents=True)
            changed = original.replace(anchor, replacement)
            if label == "delayed-capture":
                if changed.count("Trace.hit(4);") != 2:
                    raise RuntimeError("Owned delayed capture postlude anchor changed")
                changed = changed.replace("Trace.hit(4);", "this.delayedOwner = Owner.this; Trace.hit(4);")
            source.write_text(changed)
            compiled = work / (label + "-classes")
            verify.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                "CapturedConstructorReadHarness", "1", str(mode), "0"], text=True, capture_output=True,
                timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "captured constructor read contract" not in result.stderr:
                raise RuntimeError("Frozen observer missed the independent " + label + " control")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "coverage": "partial", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 4, "fresh_jvm_cases": 56, "attempts_per_case": 2,
                    "dependency_provider": args.dependency_provider, "incoming_outer_reuse": sorted(overwritten),
                    "captures_and_original_static_snapshots": "matched",
                    "capture_identity_before_superclass_callbacks_and_on_failed_objects": "matched",
                    "later_initialization_superclass_postlude_and_failure_order": "matched",
                    "original_metadata_Exceptions_and_inventories": "matched_except_reported_static_member_capture_flags",
                    "independent_counterexamples": len(mutants), "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
