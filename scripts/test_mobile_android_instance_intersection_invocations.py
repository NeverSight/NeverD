#!/usr/bin/env python3
"""Verify inherited virtual and interface calls with exact nominal intersection witnesses."""
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
    from scripts.test_mobile_android_nested_return import method_code, class_records
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import method_code, class_records


PROVIDERS = {"Lfixture/Base;", "Lfixture/Api;"}
INTERFACES = {"Lfixture/Api;", "Lfixture/First;", "Lfixture/Second;", "Lfixture/Third;"}


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-instance-intersection-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "instance_intersection_invocations/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("InstanceIntersectionCaller", "InstanceIntersectionDerived", "InstanceIntersectionHarness")],
                             harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (9, 15):
            raise RuntimeError("Owned instance intersection inventory changed: " + repr((len(all_classes), len(all_methods))))
        classes = all_classes - (PROVIDERS if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        for owner in PROVIDERS:
            method = records[owner]["methods"][owner + "->accept(Lfixture/First;)V"]
            if method["signature"] != "<T::Lfixture/First;:Lfixture/Second;>(TT;)V":
                raise RuntimeError("Original complete instance intersection method contract changed")
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for owner in sorted(PROVIDERS):
                relative = owner[1:-1] + ".class"
                archive.writestr(relative, (originals / relative).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label, wrong=False):
            output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                "InstanceIntersectionHarness", "wrong-bound" if wrong else "complete"], label)
            bound = "missing" if wrong else "complete"
            if output.splitlines() != [f"calls=320,bounds={bound},snapshots-dispatch-and-failures=matched"]:
                raise RuntimeError("Original instance intersection matrix changed: " + output)
            return output

        def dex(root, label):
            destination = work / label
            destination.mkdir()
            inputs = [path for path in sorted(root.rglob("*.class"))
                      if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *inputs], "d8-" + label)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned instance intersection changed DEX inventory")
            return result

        baseline = execute((originals, harness), "baseline")
        source_dex = dex(originals, "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original instance intersection runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-invocation-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] not in {"runVirtual", "runInterface"}:
                continue
            ops = [(int(pc, 16), op, tail) for pc, op, tail in
                   re.findall(r"\|([0-9a-fA-F]+):\s+(\S+)([^\n]*)", block[2])]
            reads = [pc for pc, op, tail in ops if op == "iget-object" and "Lfixture/Core;.value:" in tail]
            effects = [pc for pc, op, tail in ops if op in {"invoke-static", "invoke-static/range"} and "Lfixture/Trace;.hit:" in tail]
            owner, invoke = ("Child", "invoke-virtual") if name[1] == "runVirtual" else ("Api", "invoke-interface")
            calls = [pc for pc, op, tail in ops if op in {invoke, invoke + "/range"}
                     and "Lfixture/" + owner + ";.accept:(Lfixture/First;)V" in tail]
            if len(reads) != 1 or len(effects) != 1 or len(calls) != 1 or not reads[0] < effects[0] < calls[0]:
                raise RuntimeError("D8 lost the original field snapshot, callback or inherited invocation owner")
            witnessed.add(name[1])
        if witnessed != {"runVirtual", "runInterface"}:
            raise RuntimeError("Missing original virtual/interface intersection witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Instance intersection changed original inventory or signatures")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=8,
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes - INTERFACES}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Instance intersection invented or omitted a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original instance intersection metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after["methods"].get(identity) != metadata or actual_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Original instance intersection method metadata/Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Instance intersection changed snapshots, virtual dispatch or failure order")

        negative = work / "wrong-bound-originals"
        shutil.copytree(originals, negative)
        target = negative / "fixture/Value.class"
        before = target.read_bytes()
        old, new = b"fixture/Second", b"fixture/Third"
        pattern = b"\x01" + len(old).to_bytes(2, "big") + old
        replacement = b"\x01" + len(new).to_bytes(2, "big") + new
        if before.count(pattern) != 1:
            raise RuntimeError("Missing exact owned intersection interface witness")
        after = before.replace(pattern, replacement, 1)
        if (method_code(before) != method_code(after)
                or before[class_records(before)["pool_end"]:] != after[class_records(after)["pool_end"]:]):
            raise RuntimeError("Wrong-bound edit changed original Code or descriptors")
        target.write_bytes(after)
        execute((negative, harness), "wrong-bound-original-runtime", True)
        negative_dex = dex(negative, "wrong-bound-dex")
        rejected = work / "wrong-bound-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath),
            "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-bound-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "intersection" not in result.stderr:
            raise RuntimeError("Instance intersection accepted an unproved original nominal argument")
        (work / "wrong-bound-encoding.json").write_text(json.dumps(
            {"original_Code_and_descriptors": "unchanged", "late_callee_cast": "verified", "recovery": "rejected"}, indent=2) + "\n")

        overload = work / "competing-overload-originals"
        shutil.copytree(originals, overload)
        source = work / "competing-overload/fixture/Child.java"
        source.parent.mkdir(parents=True)
        source.write_text("package fixture;\npublic class Child extends Base { public void accept(Value value) { Trace.hit(8); } }\n")
        compiled = work / "competing-overload-compiled"
        verify.compile_local([source], compiled, classpath=originals, release=8)
        shutil.copyfile(compiled / "fixture/Child.class", overload / "fixture/Child.class")
        if (overload / "fixture/Core.class").read_bytes() != (originals / "fixture/Core.class").read_bytes():
            raise RuntimeError("Competing overload changed an original call site")
        execute((overload, harness), "competing-overload-original-runtime")
        overload_dex = dex(overload, "competing-overload-dex")
        rejected = work / "competing-overload-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(overload_dex), *map(str, classpath),
            "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "competing-overload-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "intersection" not in result.stderr:
            raise RuntimeError("Instance intersection accepted a conflicting source overload")
        mutants = (
            ("late-field-read", "Core", "Value selected = value;\n        Trace.hit(1);", "Trace.hit(1);\n        Value selected = value;", 2),
            ("missing-evaluation", "Core", "Trace.hit(1);", "", 2),
            ("duplicate-evaluation", "Core", "Trace.hit(1);", "Trace.hit(1); Trace.hit(1);", 2),
            ("lost-override", "Core", "receiver.accept(selected);", "new Base().accept(selected);", 1),
            ("changed-failure-identity", "Trace", "throw failure;", "throw new IllegalStateException();", 1),
        )
        for label, name, anchor, replacement, count in mutants:
            original = (fixture / "fixture" / (name + ".java")).read_text()
            if original.count(anchor) != count:
                raise RuntimeError("Owned instance intersection control anchor changed")
            source = work / label / "fixture" / (name + ".java")
            source.parent.mkdir(parents=True)
            source.write_text(original.replace(anchor, replacement))
            compiled = work / (label + "-classes")
            verify.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                "InstanceIntersectionHarness"], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "instance intersection contract" not in result.stderr:
                raise RuntimeError("Frozen observer missed the independent " + label + " control")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "runtime_calls": 320, "dependency_provider": args.dependency_provider,
                    "inherited_virtual_interface_and_frozen_override_dispatch": "matched",
                    "typed_field_snapshot_before_callback_and_null_receiver_failure": "matched",
                    "complete_signatures_original_metadata_Exceptions_and_inventories": "unchanged",
                    "same_Code_wrong_bound_late_cast": "verified_and_rejected",
                    "competing_overload_with_unchanged_call_sites": "rejected",
                    "independent_counterexamples": len(mutants), "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
