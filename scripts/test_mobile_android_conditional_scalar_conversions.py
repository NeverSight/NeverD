#!/usr/bin/env python3
"""Check original conditional scalar conversion order inside allocation arguments."""
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-conditional-conversions-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "conditional_scalar_conversions/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("ConditionalScalarCaller", "ConditionalScalarHarness")],
                             harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (4, 10):
            raise RuntimeError("Owned conditional conversion inventory changed: " + repr((len(all_classes), len(all_methods))))
        providers = {"Lfixture/Provider;", "Lfixture/Target;"} if args.dependency_provider else set()
        classes = all_classes - providers
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if providers:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            results = []
            for route in range(4):
                for mode in range(18):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "ConditionalScalarHarness", str(route), str(mode)], f"{label}-{route}-{mode}")
                    if (len(output.splitlines()) != 2 or any(not re.fullmatch(
                            f"route={route},mode={mode},attempt={attempt},trace=[0-9]+", line)
                            for attempt, line in enumerate(output.splitlines()))):
                        raise RuntimeError("Original conditional conversion oracle changed: " + output)
                    results.append(output)
            return results

        baseline = execute((originals, harness), "baseline")
        dex = work / "dex"
        dex.mkdir()
        inputs = [p for p in sorted(originals.rglob("*.class"))
                  if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *inputs], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned conditional conversion DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes
                or {r["identity"] for r in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed original conditional conversion definitions")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Creator;" or not name or name[1] != "make":
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            if (ops[0] != "new-instance" or ops.count("new-instance") != 1
                    or sum(op.startswith("if-") for op in ops) != 1
                    or ops.count("float-to-double") != 1 or ops.count("double-to-int") != 1
                    or ops.count("move-result-wide") != 1
                    or "Ljava/lang/Math;.ceil:(D)D" not in block[2]):
                raise RuntimeError("D8 changed the original conditional conversion witness")
            witnessed = True
        if not witnessed:
            raise RuntimeError("Missing conditional conversion witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count", 0)
                or coverage["status"] != "recovered" or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release") != 14):
            raise RuntimeError("Conditional conversion recovery changed original contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=14,
                             classpath=dependency if providers else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Conditional conversion source gained or lost a runtime entry")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != after[owner].get(key):
                    raise RuntimeError("Original conditional conversion metadata changed: " + key)
            for identity, metadata in records[owner]["methods"].items():
                if after[owner]["methods"].get(identity) != metadata or after_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Original conditional conversion method metadata changed")
        runtime = (rebuilt, harness, dependency) if providers else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Conditional conversion changed argument values or effects")
        text = (fixture / "fixture/Creator.java").read_text()
        marker = "(int) Math.ceil((double) Provider.read(seed))"
        if text.count(marker) != 1:
            raise RuntimeError("Owned conversion source anchor changed")
        controls = [text.replace(marker, "(int) Provider.read(seed)"),
                    text.replace("Provider.after()", "0"),
                    text.replace("seed != null", "seed == null"),
                    text.replace("return new Target", "Provider.after(); return new Target")]
        for index, changed in enumerate(controls):
            source = work / f"control-{index}/fixture/Creator.java"
            source.parent.mkdir(parents=True)
            source.write_text(changed)
            target = work / f"control-{index}-classes"
            verify.compile_local([source], target, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (target, originals, harness))),
                                    "ConditionalScalarHarness", "1", "0"], capture_output=True, text=True,
                                    timeout=300, env=verify.environment)
            (work / f"control-{index}.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "AssertionError" not in result.stderr:
                raise RuntimeError("Oracle accepted an independent changed conversion/order control")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original conditional conversion inputs or oracle changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_provider": args.dependency_provider, "fresh_jvm_cases": len(baseline),
                    "attempts_per_case": 2, "independent_controls": len(controls),
                    "metadata_and_exceptions": "unchanged", "initialization_and_conversion_order": "matched",
                    "null_and_numeric_edges": "matched", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
