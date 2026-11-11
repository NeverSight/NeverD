#!/usr/bin/env python3
"""Verify scoped generic constructor upcasts retain checks, overloads and partial fields."""
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-generic-constructor-upcasts-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "generic_constructor_upcasts/java"
        provider_name = "Parent"
        originals, harness = work / "originals", work / "harness"
        # Keep the frozen parent's classfile in the supported declaration
        # format; only the authored flexible constructor needs Java 25.
        support = sorted(path for path in fixture.rglob("*.java") if path.name != "Core.java")
        support_classes, flexible_classes = work / "support-classes", work / "flexible-classes"
        verify.compile_local(support, support_classes, release=8)
        verify.compile_local([fixture / "fixture/Core.java"], flexible_classes, release=25, classpath=support_classes)
        compiler_hashes = (hashes(support_classes), hashes(flexible_classes))
        originals.mkdir()
        for root in (support_classes, flexible_classes):
            for path in root.rglob("*.class"):
                target = originals / path.relative_to(root)
                target.parent.mkdir(parents=True, exist_ok=True)
                with target.open("xb") as output:
                    output.write(path.read_bytes())
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("GenericConstructorUpcastsHarness", "GenericConstructorUpcastsCaller")], harness,
                             classpath=originals, release=25)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 10:
            raise RuntimeError("Owned generic constructor upcast inventory changed")
        classes = all_classes - ({"Lfixture/" + provider_name + ";"} if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x") as archive:
            archive.writestr("fixture/" + provider_name + ".class", (originals / ("fixture/" + provider_name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            outputs = []
            for route in range(4):
                for mode in range(12):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "fixture.GenericConstructorUpcastsHarness", str(route), str(mode)], f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Original generic constructor upcast matrix changed: " + output)
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        dex_dir = work / "dex"
        dex_dir.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex_dir, *paths], "d8-original")
        source_dex = dex_dir / "classes.dex"
        if list(dex_dir.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned generic constructor upcast fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original generic constructor upcast runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-transfer-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] != "<init>":
                continue
            rows = [(int(pc, 16), op, tail) for pc, op, tail in
                    re.findall(r"\|([0-9a-fA-F]+):\s+(\S+)([^\n]*)", block[2])]
            casts = [pc for pc, op, tail in rows if op == "check-cast" and "Lfixture/Base;" in tail]
            calls = [pc for pc, op, tail in rows if op.startswith("invoke-direct") and "Lfixture/Parent;.<init>:(Lfixture/Base;)V" in tail]
            checks = [pc for pc, op, tail in rows if op.startswith("invoke-static") and "Lfixture/Trace;.before:" in tail]
            if len(casts) != 1 or len(calls) != 1 or len(checks) != 1 or not checks[0] < casts[0] < calls[0]:
                raise RuntimeError("D8 lost original checked generic constructor upcast")
            witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original constructor upcast witness")
        identity = "Lfixture/Core;-><init>(Lfixture/Derived;)V"
        if records["Lfixture/Core;"]["methods"][identity]["signature"] != "(Lfixture/Derived<TK;TV;>;)V":
            raise RuntimeError("Compiler changed original constructor signature")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count", 0)
                or coverage["status"] != "recovered" or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release", 8) != 25):
            raise RuntimeError("Generic constructor upcast recovery changed original inventory or contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=25, classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Generic constructor upcast recovery invented or omitted a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original writer class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after["methods"].get(identity) != metadata or actual_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Original writer method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Generic constructor upcast changed values, ordered effects, aliases or failure identity")
        mutations = (
            ("wrong-overload", "Core", "super((Base<K,V>)(Object)value);", "super(value);", "0"),
            ("copied-input", "Core", "super((Base<K,V>)(Object)value);", "super(new Base<K,V>(value.key, value.value));", "0"),
            ("missing-check", "Core", "Trace.before(value, \"owned constructor upcast\");", "", "0"),
            ("missing-capture", "Core", "original = value;", "", "0"),
            ("early-capture", "Core", "super((Base<K,V>)(Object)value);\n        original = value;", "original = value;\n        super((Base<K,V>)(Object)value);", "4"),
        )
        for label, owner, old, new, mode in mutations:
            text = (fixture / ("fixture/" + owner + ".java")).read_text()
            if text.count(old) != 1:
                raise RuntimeError("Independent transfer counterexample anchor changed")
            wrong = work / label / ("fixture/" + owner + ".java")
            wrong.parent.mkdir(parents=True)
            wrong.write_text(text.replace(old, new))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=25)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                    "fixture.GenericConstructorUpcastsHarness", "1", mode], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned generic constructor upcast contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if ((hashes(support_classes), hashes(flexible_classes)) != compiler_hashes
                or hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_provider": args.dependency_provider, "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "original_inventory_metadata_and_exceptions": "unchanged",
                    "original_input_overload_and_field_identity": "matched",
                    "initializer_prefix_parent_and_partial_capture_failure_identity": "matched",
                    "independent_compiler_counterexamples": len(mutations), "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
