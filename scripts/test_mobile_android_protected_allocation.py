#!/usr/bin/env python3
"""Verify protected allocation order and committed exception-handler inputs."""
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
    parser.add_argument("--handler-effects", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-protected-allocation-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "protected_allocation/java"
        originals, harness = work / "originals", work / "harness"
        if args.handler_effects:
            profile = work / "handler-source"
            overrides = FIXTURES / "protected_allocation_handlers/java"
            for source in sorted(fixture.rglob("*.java")):
                relative = source.relative_to(fixture)
                selected = overrides / relative
                target = profile / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text((selected if selected.exists() else source).read_text())
            fixture = profile
        harness_name = "ProtectedAllocationHandlerHarness" if args.handler_effects else "ProtectedAllocationHarness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=17)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("ProtectedAllocationCaller", harness_name)], harness, classpath=originals, release=17)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 4 or len(all_methods) != (11 if args.handler_effects else 10):
            raise RuntimeError("Owned protected-allocation inventory changed")
        providers = {"Lfixture/Payload;"}
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

        def execute(runtime, label):
            result = []
            for variant in range(2):
                for route in range(4):
                    for mode in range(6 if args.handler_effects else 5):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            harness_name, str(variant), str(route), str(mode)], f"{label}-{variant}-{route}-{mode}")
                        expected = f"variant={variant},route={route},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected,line) for line in output.splitlines()):
                            raise RuntimeError("Original protected allocation matrix changed: " + output)
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
            raise RuntimeError("Owned protected allocation fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original protected-allocation runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-protected-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] == "Lfixture/Maker;" and name and name[1] in {"observed", "fallback"}:
                ops = [(int(pc,16),op,ref) for pc,op,ref in re.findall(r"\|([0-9a-fA-F]+):\s+(\S+)([^\n]*)", block[2])]
                allocation = next(pc for pc,op,_ in ops if op == "new-instance")
                preparation = next(pc for pc,op,ref in ops if op == "invoke-static" and "Lfixture/Trace;.prepare:()V" in ref)
                constructor = next(pc for pc,op,ref in ops if op == "invoke-direct" and "Lfixture/Payload;.<init>:(Ljava/lang/Object;)V" in ref)
                catches = re.search(r"(?ms)^\s+catches\s+: (\d+)\n(.*?)(?=^\s+positions\s+:)", block[2])
                if not catches:
                    raise RuntimeError("Missing original allocation catch table")
                intervals = [(int(a,16),int(b,16)) for a,b in re.findall(r"(?m)^\s+0x([0-9a-fA-F]+) - 0x([0-9a-fA-F]+)$", catches[2])]
                if (not allocation < preparation < constructor or len(intervals) != 1
                        or not intervals[0][0] <= allocation < constructor < intervals[0][1]
                        or "<any> ->" not in catches[2]
                        or not any(op == "move-exception" for _,op,_ in ops)):
                    raise RuntimeError("javac/D8 changed the protected allocation witness")
                if args.handler_effects:
                    handler = next(pc for pc, op, _ in ops if op == "move-exception")
                    tail = [(pc, op, ref) for pc, op, ref in ops if pc > handler]
                    if (sum(op.startswith("const-string") for _, op, _ in tail) != 2
                            or not any(op == "invoke-static" and "Lfixture/Trace;.logFailure:" in ref for _, op, ref in tail)
                            or not any(op == "iput" and ".code:I" in ref for _, op, ref in tail)
                            or not any(op == "iput-boolean" and ".checked:Z" in ref for _, op, ref in ops)):
                        raise RuntimeError("D8 omitted original handler calls, writes or shared continuation")
                witnessed.add(name[1])
        if witnessed != {"observed", "fallback"}:
            raise RuntimeError("Missing original protected allocation witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Protected allocation recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != 14:
            raise RuntimeError("Protected allocation unexpectedly raised its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Protected allocation recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original protected allocation class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original protected allocation method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Protected allocation call changed an original effect or result")
        # The separately compiled variant hoists preparation above allocation.
        # The unchanged observer must reject its class-initialization order.
        wrong = work / "hoisted/fixture/Maker.java"
        wrong.parent.mkdir(parents=True)
        text = (fixture / "fixture/Maker.java").read_text()
        for argument in ("input", "Trace.token"):
            statement = "result = " if args.handler_effects else "return "
            original_call = statement + "new Payload(switch (0) { default -> { Trace.prepare(); yield " + argument + "; } });"
            if text.count(original_call) != 1:
                raise RuntimeError("Original protected allocation witness changed")
            text = text.replace(original_call, "Trace.prepare(); " + statement + "new Payload(" + argument + ");")
        wrong.write_text(text)
        verify.compile_local([wrong], work / "hoisted-classes", classpath=originals, release=17)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (work / "hoisted-classes", originals, harness))),
                                 harness_name, "0", "1", "0"], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "hoisted-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle did not detect hoisted argument preparation")
        handler_counterexamples = []
        if args.handler_effects:
            original = (fixture / "fixture/Maker.java").read_text()
            ordered = 'Trace.logFailure("owned first", "owned second", failure);\n            observer.seen = failure;'
            if original.count(ordered) != 2:
                raise RuntimeError("Original handler effect order changed")
            variants = [
                ("late-receiver", original.replace("observer.", "Trace.observer."), "1"),
                ("early-write", original.replace(ordered,
                    'observer.seen = failure;\n            Trace.logFailure("owned first", "owned second", failure);'), "5"),
            ]
            for label, changed, mode in variants:
                wrong_source = work / label / "fixture/Maker.java"
                wrong_source.parent.mkdir(parents=True)
                wrong_source.write_text(changed)
                wrong_classes = work / (label + "-classes")
                verify.compile_local([wrong_source], wrong_classes, classpath=originals, release=17)
                result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong_classes, originals, harness))),
                                         harness_name, "0", "1", mode], text=True,
                                        capture_output=True, timeout=300, env=verify.environment)
                (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
                if result.returncode == 0 or "AssertionError" not in result.stderr:
                    raise RuntimeError("Independent oracle missed handler counterexample: " + label)
                handler_counterexamples.append(label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "handler_effects": args.handler_effects, "dependency_provider": args.dependency_provider,
                    "original_metadata_and_exceptions": "unchanged", "allocation_and_argument_order": "matched",
                    "original_exception_identity": "matched", "original_protected_allocation": "independently witnessed",
                    "independent_handler_counterexamples": handler_counterexamples,
                    "independent_hoisted_preparation_counterexample": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
