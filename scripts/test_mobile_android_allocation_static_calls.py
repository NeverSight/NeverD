#!/usr/bin/env python3
"""Check allocation, static argument calls and original cast/exception order."""
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
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_protected_array_callbacks import change_method_flags


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--generic-provider", action="store_true")
    parser.add_argument("--call-access", choices=("public", "package", "protected"), default="public")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-allocation-static-calls-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "allocation_static_calls/java"
        originals, harness = work / "originals", work / "harness"
        inputs = sorted(fixture.rglob("*.java"))
        if args.generic_provider or args.call_access != "public":
            owned = work / "owned-input/fixture"
            owned.mkdir(parents=True)
            inputs = []
            for source in sorted(fixture.rglob("*.java")):
                text = source.read_text()
                if source.name == "Provider.java":
                    before = "public static Object check(Object input, long word, double bits)"
                    if text.count(before) != 1:
                        raise RuntimeError("Owned generic provider source changed")
                    prefix = "" if args.call_access == "package" else args.call_access + " "
                    replacement = prefix + ("static <T> T check(T input, long word, double bits)" if args.generic_provider
                                             else "static Object check(Object input, long word, double bits)")
                    text = text.replace(before, replacement)
                target = owned / source.name
                target.write_text(text)
                inputs.append(target)
        verify.compile_local(inputs, originals)
        caller = FIXTURES / "harness/AllocationStaticCallCaller.java"
        if args.call_access != "public":
            # The separately compiled original caller needs the same runtime
            # package to exercise its direct nonpublic call legally.
            owned_caller = work / "caller/AllocationStaticCallCaller.java"
            owned_caller.parent.mkdir()
            owned_caller.write_text("package fixture;\n" + caller.read_text())
            caller = owned_caller
        verify.compile_local([FIXTURES / "harness/AllocationStaticCallHarness.java", caller], harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 10:
            raise RuntimeError("Owned allocation-call inventory changed")
        classes = all_classes - ({"Lfixture/Provider;"} if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        provider = records["Lfixture/Provider;"]["methods"]["Lfixture/Provider;->check(Ljava/lang/Object;JD)Ljava/lang/Object;"]
        expected_signature = "<T:Ljava/lang/Object;>(TT;JD)TT;" if args.generic_provider else None
        if provider["signature"] != expected_signature:
            raise RuntimeError("Original provider did not retain the authored formal/input/result binding")

        def provider_jar(root: Path, target: Path) -> None:
            with zipfile.ZipFile(target, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("fixture/Provider.class", (root / "fixture/Provider.class").read_bytes())

        dependency = work / "owned-provider.jar"
        provider_jar(originals, dependency)
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime: tuple[Path, ...], prefix: str) -> list[str]:
            actual = []
            for mode in range(8):
                for route in range(4):
                    success = mode in (0, 1, 7)
                    trace = 1 if mode == 5 else 12 if mode == 6 else 1233 if mode in (2, 3) else 123434
                    expected = (f"mode={mode},route={route},successes={2 if success else 0},failures={0 if success else 2},"
                                f"calls={0 if mode in (5, 6) else 2},constructors={2 if success or mode == 4 else 0},trace={trace}")
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                         "AllocationStaticCallHarness", str(mode), str(route)], f"{prefix}-{mode}-{route}")
                    if output.splitlines() != [expected]:
                        raise RuntimeError("Owned allocation order matrix changed: " + output)
                    actual.append(output)
            return actual

        baseline = execute((originals, harness), "baseline")

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned allocation-call fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original allocation-call runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-call-disassembly")
        witnesses = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Creator;" or not name or name[1] not in {"checked", "raw"}:
                continue
            ops = [op.removesuffix("/range") for op in re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])]
            if not (ops.count("new-instance") == ops.count("invoke-static") == 1
                    and ops.index("new-instance") < ops.index("invoke-static") < ops.index("invoke-direct")):
                raise RuntimeError("D8 changed original allocation-before-call ordering")
            if ("check-cast" in ops) != (name[1] == "checked"):
                raise RuntimeError("D8 changed the original argument result cast")
            witnesses.add(name[1])
        if witnesses != {"checked", "raw"}:
            raise RuntimeError("Missing original allocation-call witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Allocation-call recovery changed runtime entries or original contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Allocation-call recovery invented a runtime entry or unexpected helper")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original allocation-call class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"][method] != metadata or actual_exceptions[method] != exceptions[method]:
                    raise RuntimeError("Original allocation-call method metadata or Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Allocation-call invocation changed initialization, casts or ordered failures")
        negative = work / "private-provider-originals"
        shutil.copytree(originals, negative)
        receipt = change_method_flags(negative / "fixture/Provider.class", b"(Ljava/lang/Object;JD)Ljava/lang/Object;",
                                      {"public": 0x9, "package": 0x8, "protected": 0xc}[args.call_access], 0xa, name=b"check")
        (work / "private-provider-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_classpath = list(classpath)
        if args.dependency_provider:
            negative_jar = work / "private-provider.jar"
            provider_jar(negative, negative_jar)
            negative_classpath[-1] = negative_jar
        negative_dex = dex(negative, work / "private-provider-dex")
        rejected = work / "private-provider-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, negative_classpath),
                                 "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "private-provider-recovery.log").write_text(result.stdout + result.stderr)
        expected_failure = "generic invocation requires" if args.generic_provider else "allocation argument call"
        if result.returncode == 0 or rejected.exists() or expected_failure not in result.stderr:
            raise RuntimeError("Allocation-call counterexample did not fail its declared-call contract")
        hoisted = work / "hoisted/fixture/Creator.java"
        hoisted.parent.mkdir(parents=True)
        text = (fixture / "fixture/Creator.java").read_text()
        needle = "Target<Value> result = new Target<Value>((Value) Provider.check(input, word, bits), input, word, bits);"
        if text.count(needle) != 1:
            raise RuntimeError("Owned hoisted-call counterexample no longer matches")
        hoisted.write_text(text.replace(needle, "Object prepared = Provider.check(input, word, bits);\n"
            "        Target<Value> result = new Target<Value>((Value) prepared, input, word, bits);"))
        verify.compile_local([hoisted], work / "hoisted-classes", classpath=originals)
        result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (work / "hoisted-classes", originals, harness))),
                                 "AllocationStaticCallHarness", "0", "0"], text=True, capture_output=True,
                                timeout=300, env=verify.environment)
        (work / "hoisted-call-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle failed to detect moving the call before allocation")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "successes": 24, "failures": 40, "fresh_jvm_cases": 32,
                    "dependency_provider": args.dependency_provider,
                    "generic_provider": args.generic_provider,
                    "call_access": args.call_access,
                    "original_class_and_method_metadata_and_exceptions": "unchanged",
                    "allocation_call_cast_constructor_order": "matched", "same_code_counterexample": "rejected",
                    "independent_hoisted_call_counterexample": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
