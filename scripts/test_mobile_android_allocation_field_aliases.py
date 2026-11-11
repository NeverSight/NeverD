#!/usr/bin/env python3
"""Verify allocation field aliases and scalar copies preserve effects and raw bits."""
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
    parser.add_argument("--scalar-fields", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-allocation-field-alias-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / ("allocation_scalar_field_copies/java" if args.scalar_fields else "allocation_field_aliases/java")
        harness_name = "ScalarFieldCopy" if args.scalar_fields else "AllocationFieldAlias"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              (harness_name + "Caller", harness_name + "Harness")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != (4 if args.scalar_fields else 5) or len(all_methods) != (8 if args.scalar_fields else 9):
            raise RuntimeError("Owned allocation-field-alias inventory changed")
        providers = {"Lfixture/Target;", "Lfixture/Provider;", "Lfixture/Value;"}
        if args.scalar_fields:providers.remove("Lfixture/Value;")
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
            for route in range(8 if args.scalar_fields else 4):
                for mode in range(10 if args.scalar_fields else 8):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        harness_name + "Harness", str(route), str(mode)], f"{label}-{route}-{mode}")
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
            raise RuntimeError("Owned allocation alias fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original allocation-field-alias runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-constructor-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] == "Lfixture/Core;" and name and name[1] in ({"make", "makeStatic"} if args.scalar_fields else {"make"}):
                ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
                new = next(i for i,(op,ref) in enumerate(ops) if op == "new-instance" and "Lfixture/Target;" in ref)
                if args.scalar_fields:
                    constructor = next(i for i,(op,ref) in enumerate(ops) if op == "invoke-direct/range" and "Lfixture/Target;.<init>:" in ref)
                    field_kind = "iget-boolean" if name[1] == "make" else "sget-boolean"
                    fields = [i for i,(op,ref) in enumerate(ops) if op == field_kind and "Lfixture/Provider;." in ref]
                    if len(fields) != 1 or not new < fields[0] < constructor:
                        raise RuntimeError("Missing original scalar field allocation order")
                    destination = re.match(r"\s+v([0-9]+)", ops[fields[0]][1])
                    if destination is None or not any(op.split('/')[0] == "move" and
                        re.match(r"\s+v[0-9]+, v" + destination[1] + r"\b", ref)
                        for op,ref in ops[fields[0]+1:constructor]):
                        raise RuntimeError("D8 changed the scalar field copy witness")
                    witnessed.add(name[1]);continue
                fields = [i for i,(op,ref) in enumerate(ops) if op == "sget-object" and "Lfixture/Provider;.value:" in ref]
                cast = next(i for i,(op,ref) in enumerate(ops) if op == "check-cast" and "Lfixture/Value;" in ref)
                constructor = next(i for i,(op,ref) in enumerate(ops) if op == "invoke-direct/range" and "Lfixture/Target;.<init>:" in ref)
                kinds = {op.split("/")[0] for op,_ in ops[new:constructor]}
                if (len(fields) != 1 or not new < fields[0] < cast < constructor
                        or not {"move-object", "move", "move-wide"} <= kinds):
                    raise RuntimeError("D8 changed the field alias and raw-copy witness")
                witnessed.add(name[1])
        if witnessed != ({"make", "makeStatic"} if args.scalar_fields else {"make"}):
            raise RuntimeError("Missing original field-alias allocation witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Allocation alias recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != 8:
            raise RuntimeError("Allocation alias unexpectedly raised its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Allocation alias recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original allocation alias class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original allocation alias method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Allocation alias call changed an original effect or result")
        text = (fixture / "fixture/Core.java").read_text()
        mutations = {
            "hoisted-read": [("Value saved;", "Value saved = (Value) Provider.value;"),
                             ("saved = (Value) Provider.value, saved", "saved, saved")],
            "late-reread": [("Trace.after = saved;", "Trace.after = (Value) Provider.value;")],
            "numeric-float-copy": [("saved, bits, floating, fraction);",
                                     "saved, bits, (float) Float.floatToRawIntBits(floating), fraction);")],
        }
        if args.scalar_fields:
            mutations = {
                "hoisted-read": [("boolean saved;", "boolean saved = owner.value;"),
                                 ("saved = owner.value, saved", "saved, saved"),
                                 ("saved = Provider.staticFlag, saved", "saved, saved")],
                "late-reread": [("Trace.afterFlag = saved;", "Trace.afterFlag = owner.value;")],
                "changed-alias": [("owner.value, saved, input", "owner.value, !saved, input")],
                "numeric-float-copy": [("bits, floating, fraction);", "bits, (float) Float.floatToRawIntBits(floating), fraction);")],
                "changed-live-wide": [("Trace.tail = bits;", "Trace.tail = bits ^ 1L;")],
            }
        for label, replacements in mutations.items():
            wrong = work / label / "fixture/Core.java"
            wrong.parent.mkdir(parents=True)
            changed = text
            for before, after in replacements:
                if (not args.scalar_fields and changed.count(before) != 1) or before not in changed:
                    raise RuntimeError("Original allocation counterexample anchor changed")
                changed = changed.replace(before, after)
            wrong.write_text(changed)
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     harness_name + "Harness", "1", "0"], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or ("scalar field copy changed" if args.scalar_fields else "allocation field alias changed") not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "scalar_fields": args.scalar_fields, "independent_wrong_programs": len(mutations),
                    "original_metadata_and_exceptions": "unchanged", "original_field_read_and_cast": "independently witnessed",
                    "class_initialization_order_and_exception_identity": "matched",
                    "repeated_aliases_and_live_registers": "matched", "float_double_and_wide_raw_bits": "matched",
                    "independent_hoist_reread_and_numeric_copy_counterexamples": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
