#!/usr/bin/env python3
"""Verify pure wide argument copies inside conditional nested constructors."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
import re
from pathlib import Path
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

PROVIDERS = {"Lfixture/Outer;", "Lfixture/Inner;"}


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-conditional-nested-copies-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "conditional_nested_copies/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local(sorted((fixture.parent / "harness").glob("*.java")), harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 4 or len(all_methods) != 13:
            raise RuntimeError("Owned conditional nested copy inventory changed")
        classes = all_classes - (PROVIDERS if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)

        def provider_jar(root: Path, target: Path) -> None:
            with zipfile.ZipFile(target, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                for owner in sorted(PROVIDERS):
                    name = owner[1:-1] + ".class"
                    archive.writestr(name, (root / name).read_bytes())

        dependency = work / "owned-provider.jar"
        provider_jar(originals, dependency)
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]
        def execute(runtime, label):
            result = []
            for variant in range(2):
                for route in range(2):
                    for branch in range(2):
                        for mode in range(12):
                            output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                "ConditionalNestedCopiesHarness", str(variant), str(route), str(branch), str(mode)],
                                f"{label}-{variant}-{route}-{branch}-{mode}")
                            expected = f"variant={variant},route={route},branch={branch},mode={mode},attempt=[01],trace=[0-9]+,failure=(none|body|first-init|repeat-init)"
                            if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                                raise RuntimeError("Original conditional nested matrix changed: " + output)
                            result.append(output)
            return result

        baseline = execute((originals, harness), "baseline")

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned conditional-nested-copy fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original conditional-nested-copy runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-nested-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] not in {"make", "floating"}:
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            allocations = [i for i, op in enumerate(ops) if op == "new-instance"]
            calls = [i for i, op in enumerate(ops) if op.startswith("invoke-direct")]
            if len(allocations) != 2 or len(calls) != 2:
                raise RuntimeError("D8 changed original nested allocation inventory")
            copies = ops[allocations[1] + 1:calls[0]]
            if (not allocations[0] < allocations[1] < calls[0] < calls[1]
                    or sum(op.startswith("move-object") for op in copies) != 1
                    or sum(op.startswith("move-wide") for op in copies) != 2
                    or len(copies) != 3 or ops[calls[0]] != "invoke-direct/range"
                    or sum(ops.count(op) for op in ("if-eqz", "if-nez")) != 1):
                raise RuntimeError("D8 changed the original wide nested argument copy witness: " + name[1])
            witnessed.add(name[1])
        if witnessed != {"make", "floating"}:
            raise RuntimeError("Missing original long/double nested argument copy witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Conditional-nested-copy recovery changed runtime entries or original contracts")
        if coverage.get("minimum_java_release") != 14:
            raise RuntimeError("Nested conditional allocation did not report its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        helper_owners = {owner[9:-1] for owner in classes}
        expected_helpers = {"Lfixture/" + name + ";->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for name in helper_owners}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Conditional-nested-copy recovery invented a runtime entry or unexpected helper")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original conditional-nested-copy class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"][method] != metadata or actual_exceptions[method] != exceptions[method]:
                    raise RuntimeError("Original conditional-nested-copy method metadata or Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Nested initialization crossed the original conditional argument program")
        counterexamples = (
            ("hoisted-selection", "return new Outer(tag, first, second,\n            Trace.choose(choose)",
             "boolean early = Trace.choose(choose); return new Outer(tag, first, second,\n            early", 0, 1, 0),
            ("eager-inner", "return new Outer(tag, first, second,\n            Trace.choose(choose) ? new Inner(tag, first, second)",
             "Inner early = new Inner(tag, first, second); return new Outer(tag, first, second,\n            Trace.choose(choose) ? early", 0, 0, 0),
            ("swapped-wide", "? new Inner(tag, first, second)", "? new Inner(tag, second, first)", 0, 1, 0),
            ("lost-tag", "? new Inner(tag, first, second)", "? new Inner(null, first, second)", 0, 1, 0),
            ("canonicalized-nan", "? new Inner(tag, first, second)",
             "? new Inner(tag, Double.longBitsToDouble(Double.doubleToLongBits(first)), second)", 1, 1, 8),
        )
        for label, before, after, variant, branch, mode in counterexamples:
            wrong = work / label / "fixture/Core.java"
            wrong.parent.mkdir(parents=True)
            text = (fixture / "fixture/Core.java").read_text()
            # Check both overload anchors before selecting the double-only case.
            if text.count(before) != 2:
                raise RuntimeError("Independent nested-copy counterexample anchor changed")
            if label == "canonicalized-nan":
                head, marker, tail = text.partition("    public static Outer floating")
                text = head + marker + tail.replace(before, after)
            else:
                text = text.replace(before, after)
            wrong.write_text(text)
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     "ConditionalNestedCopiesHarness", str(variant), "0", str(branch), str(mode)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "conditional nested copies contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "dependency_provider": args.dependency_provider,
                    "original_metadata_and_exceptions": "unchanged", "runtime_inventory": "original entries and reported helpers only",
                    "selected_inner_initialization_and_outer_order": "matched", "first_and_repeated_initialization_failures": "matched",
                    "original_receiver_identity_and_constructor_failures": "matched", "exception_classes_and_body_identity": "matched",
                    "independent_counterexamples": len(counterexamples), "wide_bits_and_reference_identity": "matched", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
