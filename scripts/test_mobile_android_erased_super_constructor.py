#!/usr/bin/env python3
"""Verify erased superclass constructor arguments retain original runtime contracts."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def verify_allocated_wrong_programs(verify, work, fixture, originals, harness):
    mutations = (
        ("late-allocation", "Child", "super(new Adapter());", "super(null); new Adapter();"),
        ("duplicate-allocation", "Child", "super(new Adapter());", "super(new Adapter()); new Adapter();"),
        ("lost-factory-identity", "Base", "this.factory = factory;", "this.factory = null;"),
        ("lost-callback", "Base", "value = factory.get();", "value = null;"),
        ("late-snapshot", "Adapter", "snapshot = Trace.seed;", "snapshot = Trace.late;"),
        ("changed-marker", "Child", "this.marker = marker;", "this.marker = new Object();"),
        ("changed-exception", "Trace", "throw failure;", "throw new IllegalStateException(failure);"),
    )
    for label, owner, before, after in mutations:
        text = (fixture / ("fixture/" + owner + ".java")).read_text()
        if text.count(before) != 1:
            raise RuntimeError("Owned allocated callback counterexample anchor changed")
        wrong = work / label / (owner + ".java")
        wrong.parent.mkdir()
        wrong.open("x").write(text.replace(before, after, 1))
        compiled = work / (label + "-classes")
        verify.compile_local([wrong], compiled, classpath=originals, release=8)
        mode = "5" if label == "changed-exception" else "0"
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
            "AllocatedScopedCallbackHarness", "1", mode], text=True, capture_output=True,
            timeout=300, env=verify.environment)
        (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
        if result.returncode == 0 or "owned allocated scoped callback" not in result.stderr:
            raise RuntimeError("Independent observer missed allocated callback change: " + label)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--null-tail", choices=("nominal", "carrier"))
    parser.add_argument("--allocated-callback", action="store_true")
    args = parser.parse_args()
    if args.allocated_callback and args.null_tail:
        parser.error("allocated callbacks use their own independent constructor program")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-erased-super-constructor-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / ("allocated_scoped_callback/java" if args.allocated_callback else "erased_super_constructor/java")
        harness_class = "AllocatedScopedCallbackHarness" if args.allocated_callback else "ErasedSuperConstructorHarness"
        harness_sources = [FIXTURES / "harness" / (name + ".java") for name in
                           (("AllocatedScopedCallbackCaller", harness_class) if args.allocated_callback else
                            ("ErasedSuperConstructorCaller", harness_class))]
        if args.null_tail:
            authored = work / "authored-fixture"
            shutil.copytree(fixture, authored)
            fixture = authored
            base = fixture / "fixture/Base.java"
            tail_type = "String" if args.null_tail == "nominal" else "Sink<?>"
            base.write_text(base.read_text().replace("public Sink<Object> sink;",
                "public Sink<Object> sink;\n    public " + tail_type + " tail;")
                .replace("Base(Sink<Object> value)", "Base(Sink<Object> value, " + tail_type + " tail)")
                .replace("sink = value;", "sink = value;\n        this.tail = tail;")
                .replace("Base(Object value)", "Base(Object value, Object tail)"))
            child = fixture / "fixture/Child.java"
            child.write_text(child.read_text().replace("super((Sink<Object>) (Sink<?>) completion);",
                "super((Sink<Object>) (Sink<?>) completion, null);"))
            adapted = work / "authored-harness"
            adapted.mkdir()
            source = harness_sources[1]
            target = adapted / source.name
            target.write_text(source.read_text().replace("Trace.seen == value", "Trace.seen == value && value.tail == null")
                .replace("require((Object) seen.sink == sink);", "require((Object) seen.sink == sink && seen.tail == null);"))
            harness_sources[1] = target
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local(harness_sources, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 4 or len(all_methods) != (9 if args.allocated_callback else 8):
            raise RuntimeError("Owned erased-super-constructor inventory changed")
        providers = {"Lfixture/Base;"} if args.allocated_callback else {"Lfixture/Base;", "Lfixture/Sink;"}
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
            for route in range(4):
                for mode in range(9 if args.allocated_callback else 7):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        harness_class, str(route), str(mode)], f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected,line) for line in output.splitlines()):
                        raise RuntimeError("Original constructor behavior matrix changed: " + output)
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
            raise RuntimeError("Owned erased constructor fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original erased-super-constructor runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-constructor-disassembly")
        if args.allocated_callback:
            patterns = (r"new-instance[^\n]*Lfixture/Adapter;",
                        r"invoke-direct[^\n]*Lfixture/Adapter;.<init>:\(\)V",
                        r"invoke-direct[^\n]*Lfixture/Base;.<init>:\(Ljava/util/function/Supplier;\)V")
            witnessed = False
            for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
                if block.group(1) != "Lfixture/Child;":
                    continue
                matches = [re.search(pattern, block.group(2)) for pattern in patterns]
                if all(matches) and [m.start() for m in matches] == sorted(m.start() for m in matches):
                    witnessed = True
            if not witnessed:
                raise RuntimeError("D8 changed the original allocated callback before super")
        else:
            witnessed = False
            for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
                name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
                if block[1] == "Lfixture/Child;" and name and name[1] == "<init>":
                    ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
                    tail = "Ljava/lang/String;" if args.null_tail == "nominal" else "Lfixture/Sink;" if args.null_tail else ""
                    call = 1 if args.null_tail else 0
                    if (len(ops) <= call or ops[call][0] != "invoke-direct"
                            or "Lfixture/Base;.<init>:(Lfixture/Sink;" + tail + ")V" not in ops[call][1]
                            or "check-cast" in [op for op,_ in ops]
                            or (args.null_tail and (ops[0][0] != "const/4" or "#int 0" not in ops[0][1]))):
                        raise RuntimeError("javac/D8 changed the original erased forwarding witness")
                    witnessed = True
            if not witnessed:
                raise RuntimeError("Missing original erased constructor forwarding witness")
            if args.null_tail:
                (work / "null-tail-witness.json").write_text(json.dumps({"original_code": "unchanged javac/D8 output",
                    "closed_tail": args.null_tail, "incoming_generic_and_zero_literal": "independently witnessed"}, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Erased constructor recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != (25 if args.allocated_callback else 8):
            raise RuntimeError("Erased constructor unexpectedly raised its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if owner != "Lfixture/Sink;"}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Erased constructor recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original erased constructor class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original erased constructor method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Erased constructor call changed an original effect or result")
        if args.allocated_callback:
            verify_allocated_wrong_programs(verify, work, fixture, originals, harness)
        else:
            # This separately compiled body chooses the other one-argument overload.
            # The immutable observer must detect the missing original target effects.
            wrong = work / "wrong-overload/fixture/Child.java"
            wrong.parent.mkdir(parents=True)
            text = (fixture / "fixture/Child.java").read_text()
            original_call = "super((Sink<Object>) (Sink<?>) completion" + (", null" if args.null_tail else "") + ");"
            if text.count(original_call) != 1:
                raise RuntimeError("Original constructor witness changed")
            wrong.write_text(text.replace(original_call, "super((Object) completion" + (", null" if args.null_tail else "") + ");"))
            verify.compile_local([wrong], work / "wrong-overload-classes", classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (work / "wrong-overload-classes", originals, harness))),
                                     "ErasedSuperConstructorHarness", "1", "0"], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / "wrong-overload-counterexample.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "AssertionError" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect a changed constructor target")
            if args.null_tail:
                wrong = work / "wrong-null/fixture/Child.java"
                wrong.parent.mkdir(parents=True)
                value = '"owned nonnull"' if args.null_tail == "nominal" else "completion"
                wrong.write_text(text.replace(original_call, original_call.replace(", null);", ", " + value + ");")))
                wrong_classes = work / "wrong-null-classes"
                verify.compile_local([wrong], wrong_classes, classpath=originals, release=8)
                result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong_classes, originals, harness))),
                    "ErasedSuperConstructorHarness", "1", "0"], text=True, capture_output=True, timeout=300, env=verify.environment)
                (work / "wrong-null-counterexample.log").write_text(result.stdout + result.stderr)
                if result.returncode == 0 or "erased superclass constructor contract" not in result.stderr:
                    raise RuntimeError("Independent observer missed a non-null trailing argument")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "null_tail": args.null_tail,
                    "allocated_callback": args.allocated_callback,
                    "original_metadata_and_exceptions": "unchanged", "constructor_identity_and_initialization_order": "matched",
                    "original_exception_identity": "matched", "original_erased_forwarding": "independently witnessed",
                    "independent_wrong_overload_counterexample": "not-applicable" if args.allocated_callback else "detected", "input_and_oracle_bytes": "unchanged"}
        if args.allocated_callback:
            evidence["independent_wrong_programs"] = 7
        if args.null_tail:
            evidence["independent_wrong_null_counterexample"] = "detected"
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
