#!/usr/bin/env python3
"""Verify original void parameter bridges through literal superclass edges."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parents", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-literal-superclass-bridges-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "literal_superclass_bridges/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("LiteralSuperclassBridgesCaller", "LiteralSuperclassBridgesHarness")],
                             harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 7 or len(all_methods) != 15:
            raise RuntimeError("Owned literal bridge inventory changed: " + repr((len(all_classes), len(all_methods))))
        dependencies = all_classes - {"Lfixture/Core;", "Lfixture/Trace;"} if args.dependency_parents else set()
        classes = all_classes - dependencies
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-parents.jar"
        with zipfile.ZipFile(dependency, "x") as archive:
            for owner in sorted(dependencies):
                relative = owner[1:-1] + ".class"
                archive.writestr(relative, (originals / relative).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if dependencies:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            outputs = []
            for route in range(4):
                for receiver in range(3):
                    for value in range(3):
                        for mode in range(6):
                            output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                "LiteralSuperclassBridgesHarness", str(route), str(receiver), str(value), str(mode)],
                                f"{label}-{route}-{receiver}-{value}-{mode}")
                            if output != "attempts=2,dispatch-casts-wide-effects-failures=matched\n":
                                raise RuntimeError("Frozen literal bridge oracle missed its cases: " + output)
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
            raise RuntimeError("Owned literal bridge changed its DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original literal bridge runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        original_bridge = "Lfixture/Core;->accept(Lfixture/Wide;JLjava/lang/Object;)V"
        if all_methods.get(original_bridge) != "body" or records["Lfixture/Core;"].get("signature"):
            raise RuntimeError("Missing original compiler bridge or literal child edge")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count") != (1 if dependencies else 2)
                or coverage["status"] != "partial" or coverage.get("generic_signature_projections")
                or coverage.get("class_source_bindings")):
            raise RuntimeError("Literal bridge recovery changed original declarations or scope")
        row = rows[original_bridge]
        if (row.get("projection_kind") != "generic-superclass-bridge"
                or row.get("bridge_target") != "Lfixture/Core;->accept(Lfixture/Narrow;JLjava/lang/Object;)V"
                or row.get("bridge_dispatch") != "Lfixture/Root;->accept(Lfixture/Wide;JLjava/lang/Object;)V"
                or not row.get("reason")):
            raise RuntimeError("Literal bridge lost its full root contract or forwarding receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=8, classpath=dependency if dependencies else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != methods | helpers:
            raise RuntimeError("javac changed original literal bridge inventory beyond reported helpers")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original literal bridge class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after["methods"][identity] != metadata or actual_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original literal bridge method metadata changed: " + identity)
        actual = execute((rebuilt, harness, dependency), "recovered")
        if actual != baseline:
            raise RuntimeError("Original literal bridge behavior changed")
        mutants = {
            "skip-effect": ("Trace.hit(1);", ""),
            "wrong-wide": ("Trace.number = stamp;", "Trace.number = (int) stamp;"),
            "wrong-identity": ("Trace.last = value;", "Trace.last = new Narrow();"),
            "repeated-consumer": ("Trace.consume(note);", "Trace.consume(note); Trace.consume(note);"),
        }
        for label, (old, new) in mutants.items():
            mutated = work / (label + "-source")
            paths = []
            for original in inputs:
                text = original.read_text()
                if original.name == "Core.java":
                    if text.count(old) != 1:
                        raise RuntimeError("Missing exact authored bridge mutant site")
                    text = text.replace(old, new)
                path = mutated / original.relative_to(fixture)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text); paths.append(path)
            mutant = work / (label + "-classes")
            verify.compile_local(paths, mutant)
            result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (mutant, harness))),
                "LiteralSuperclassBridgesHarness", "1", "1", "0", "0"], capture_output=True, text=True,
                timeout=300, env=verify.environment)
            (work / (label + "-execution.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "AssertionError" not in result.stderr:
                raise RuntimeError("Independent oracle accepted a changed bridge effect, width or identity: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original compiler, frozen caller or dependency bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
            "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "compiled_behavior_mutants": len(mutants),
            "complete_inventory_and_metadata": "retained apart from reported helpers",
            "original_casts_dispatch_full_width_effects_and_failures": "matched", "coverage": "partial"}
        (work / "oracle-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
