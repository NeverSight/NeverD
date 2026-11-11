#!/usr/bin/env python3
"""Verify linear constructor prefixes and continuations retain primary final initialization."""
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
    parser.add_argument("--same-package-only", action="store_true", help="Run only the two nonpublic package-provider paths")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-delegation-continuation-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "delegation_continuations/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("DelegationContinuationCaller", "DelegationContinuationHarness")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 23:
            raise RuntimeError("Owned delegation-continuation inventory changed")
        providers = {"Lfixture/Base;", "Lfixture/Prefix;"}
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
            for variant in ((5, 6) if args.same_package_only else range(7)):
                for route in range(4):
                    for mode in range(21):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "DelegationContinuationHarness", str(variant), str(route), str(mode)], f"{label}-{variant}-{route}-{mode}")
                        expected = f"variant={variant},route={route},mode={mode},attempt=[01],trace=[0-9]+"
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
            raise RuntimeError("Owned delegation continuation fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original delegation-continuation runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-constructor-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            prototype = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] != "<init>" or not prototype:
                continue
            wanted = {"(Ljava/lang/Object;Z)V", "(Ljava/lang/Object;ZLfixture/Holder;)V",
                      "(Ljava/lang/Object;ZLfixture/Holder;B)V", "(Ljava/lang/Object;ZLfixture/Holder;C)V",
                      "(Ljava/lang/Object;ZLfixture/Holder;S)V", "(Ljava/lang/Object;ZLfixture/Holder;I)V",
                      "(Ljava/lang/Object;ZLfixture/Holder;J)V"}
            if prototype[1] not in wanted:
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
            delegate = next(i for i, (op, ref) in enumerate(ops)
                if op.split("/")[0] == "invoke-direct" and "Lfixture/Core;.<init>:" in ref)
            calls = [i for i, (op, ref) in enumerate(ops) if op.startswith("invoke-") and i > delegate]
            if delegate == 0 or not calls or ops[-1][0] != "return-void" or any(op.startswith(("if-", "goto")) for op, ref in ops):
                raise RuntimeError("D8 changed the linear prefix/delegation/continuation witness")
            if "Holder;" in prototype[1]:
                callee = {"B)V": "Lfixture/Core;.readPrivate:", "C)V": "Lfixture/Core;.readPackage:",
                          "S)V": "Lfixture/Core;.readProtected:", "I)V": "Lfixture/Prefix;.readPackage:",
                          "J)V": "Lfixture/Prefix;.readProtected:"}.get(prototype[1][-3:], "Lfixture/Trace;.read:")
                read = next(i for i, (op, ref) in enumerate(ops) if op.split("/")[0] == "invoke-static" and callee in ref)
                if read >= delegate:
                    raise RuntimeError("D8 moved the original delegation argument call")
            elif not any(op.startswith("const") for op, ref in ops[:delegate]):
                raise RuntimeError("D8 removed the original delegation constant prefix")
            witnessed.add(prototype[1])
        if len(witnessed) != 7:
            raise RuntimeError("Missing original constructor delegation continuation witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Delegation continuation recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != 25:
            raise RuntimeError("Delegation continuation unexpectedly raised its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Delegation continuation recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original delegation continuation class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original delegation continuation method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Delegation continuation call changed an original effect or result")
        text = (fixture / "fixture/Core.java").read_text()
        constant = "this(token, 17, 0x8000000000000023L);"
        computed = "this(Trace.read(holder, token), flag);"
        mutations = {
            "wrong-constant": (0, 0, [(constant, "this(token, 18, 0x8000000000000023L);", 1)]),
            "wrong-flag": (0, 0, [("setFlag(flag);", "setFlag(false);", 1)]),
            "repeated-read": (1, 0, [(computed, "this(Trace.read(holder, token) == null ? Trace.read(holder, token) : Trace.read(holder, token), flag);", 1)]),
            "late-observation": (0, 0, [("Trace.observe(this, 7);\n        setFlag(flag);", "setFlag(flag);\n        Trace.observe(this, 7);", 1)]),
            "wrong-live-input": (1, 0, [("tail = holder.value;", "tail = token;", 6)]),
        }
        for label, (variant, mode, replacements) in mutations.items():
            wrong = work / label / "fixture/Core.java"
            wrong.parent.mkdir(parents=True)
            changed = text
            for before, after, count in replacements:
                if changed.count(before) != count:
                    raise RuntimeError("Original delegation counterexample anchor changed")
                changed = changed.replace(before, after)
            wrong.write_text(changed)
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     "DelegationContinuationHarness", str(variant), "1", str(mode)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "delegation continuation changed" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "original_metadata_and_exceptions": "unchanged", "original_delegation_prefix_and_continuations": "independently witnessed",
                    "class_initialization_order_and_exception_identity": "matched",
                    "constructor_variants": [5, 6] if args.same_package_only else list(range(7)),
                    "nonpublic_static_call_results_and_effects": "matched",
                    "partial_finals_virtual_calls_and_retained_inputs": "matched", "null_and_original_callback_failures": "matched",
                    "independent_constant_call_order_and_live_input_counterexamples": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
