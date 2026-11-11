#!/usr/bin/env python3
"""Verify reference array stores preserve runtime component checks and effect order."""
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

PROVIDERS = {"Lfixture/Target;"}


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-reference-array-stores-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "reference_array_stores/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local(sorted((fixture.parent / "harness").glob("*.java")), harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != 12:
            raise RuntimeError("Owned reference array store inventory changed")
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
            for variant in range(5):
                for route in range(2):
                    for mode in range(14):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "ReferenceArrayStoreHarness", str(variant), str(route), str(mode)],
                            f"{label}-{variant}-{route}-{mode}")
                        expected = f"variant={variant},route={route},mode={mode},attempt=[01],trace=[0-9]+,stores=[0-9]+,error=(none|[A-Za-z]+)"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Original reference array store matrix changed: " + output)
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
                raise RuntimeError("Owned reference-array-store fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original reference-array-store runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-nested-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] not in {"direct", "rows", "fresh", "nested", "loop"}:
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
            stores = [i for i, (op, _) in enumerate(ops) if op == "aput-object"]
            reads = [i for i, (op, ref) in enumerate(ops) if op.startswith("invoke-static") and "Lfixture/Trace;.read:" in ref]
            casts = [(i, ref) for i, (op, ref) in enumerate(ops) if op == "check-cast"]
            if len(stores) != 1 or len(reads) != 1 or not reads[0] < stores[0]:
                raise RuntimeError("D8 changed the original read/store witness: " + name[1])
            if name[1] in {"direct", "rows"}:
                component = "[Ljava/lang/String;" if name[1] == "direct" else "[[I"
                if len(casts) != 1 or component not in casts[0][1] or not casts[0][0] < reads[0]:
                    raise RuntimeError("D8 changed the original incoming array cast")
            elif casts:
                raise RuntimeError("D8 introduced a component value cast before the runtime array check")
            if name[1] == "nested":
                allocation = next(i for i, (op, ref) in enumerate(ops) if op == "new-instance" and "Lfixture/Target;" in ref)
                array = next(i for i, (op, _) in enumerate(ops) if op == "new-array")
                constructor = next(i for i, (op, ref) in enumerate(ops) if op.startswith("invoke-direct") and "Lfixture/Target;.<init>:" in ref)
                if not allocation < array < reads[0] < stores[0] < constructor:
                    raise RuntimeError("D8 changed the original nested store ordering")
            if name[1] == "loop" and not any(op.startswith("goto") and "-" in ref for op, ref in ops):
                raise RuntimeError("D8 removed the original store loop backedge")
            witnessed.add(name[1])
        if witnessed != {"direct", "rows", "fresh", "nested", "loop"}:
            raise RuntimeError("Missing original reference array store witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Reference-array-store recovery changed runtime entries or original contracts")
        if coverage.get("minimum_java_release") != 14:
            raise RuntimeError("Nested array store allocation did not report its source release")
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
            raise RuntimeError("Reference-array-store recovery invented a runtime entry or unexpected helper")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original reference-array-store class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"][method] != metadata or actual_exceptions[method] != exceptions[method]:
                    raise RuntimeError("Original reference-array-store method metadata or Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Reference array stores changed original runtime behavior")
        counterexamples = (
            ("narrowed-value", "view[index] = Trace.read(view, value)",
             "view[index] = (String) Trace.read(view, value)", 1, 2, 4),
            ("narrowed-before-bounds", "view[index] = Trace.read(view, value)",
             "view[index] = (String) Trace.read(view, value)", 1, 3, 4),
            ("copied-array", "Object[] view = (String[]) input;",
             "Object[] view = ((String[]) input).clone();", 0, 0, 1),
            ("missing-store", "view[index] = Trace.read(view, value); Trace.hit(4);",
             "Trace.read(view, value); Trace.hit(4);", 1, 0, 3),
            ("changed-index", "view[index] = Trace.read(view, value)",
             "view[index + 1] = Trace.read(view, value)", 0, 0, 4),
            ("hoisted-nested-read", "Target result = new Target(view = new String[Trace.size(length)],\n            view[index] = Trace.read(view, value), tag);",
             "view = new String[Trace.size(length)]; Object early = Trace.read(view, value);\n        Target result = new Target(view, view[index] = early, tag);", 2, 0, 1),
        )
        for label, before, after, variant, mode, count in counterexamples:
            wrong = work / label / "fixture/Core.java"
            wrong.parent.mkdir(parents=True)
            text = (fixture / "fixture/Core.java").read_text()
            if text.count(before) != count:
                raise RuntimeError("Independent array store counterexample anchor changed: " + label)
            wrong.write_text(text.replace(before, after))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     "ReferenceArrayStoreHarness", str(variant), "0", str(mode)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "reference array store contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "dependency_provider": args.dependency_provider,
                    "original_metadata_and_exceptions": "unchanged", "runtime_inventory": "original entries and reported helpers only",
                    "original_store_without_value_cast": "independently witnessed",
                    "class_initialization_and_read_store_order": "matched", "first_and_repeated_initialization_failures": "matched",
                    "array_identity_runtime_component_and_partial_stores": "matched", "null_bounds_component_failure_precedence": "matched",
                    "exception_classes_and_body_identity": "matched", "independent_counterexamples": len(counterexamples),
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
