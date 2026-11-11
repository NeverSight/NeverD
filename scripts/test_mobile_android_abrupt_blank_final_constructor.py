#!/usr/bin/env python3
"""Verify abrupt constructors retain blank finals, original stores and exceptions."""
from __future__ import annotations

import argparse
import copy
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_visibility_generic_bridge import compiler_facts
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_visibility_generic_bridge import compiler_facts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-abrupt-blank-final-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "abrupt_blank_final_constructor/java"
        prefix = "AbruptBlankFinalConstructor"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (prefix + n + ".java") for n in ("Caller", "Harness")],
                             harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        inputs = {str(p): sha(p) for p in (args.android_jar.resolve(), args.neverd.resolve())}
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (4, 7):
            raise RuntimeError("Original abrupt blank final inventory changed")
        excluded = {"Lfixture/Base;", "Lfixture/Failure;"} if args.dependency_provider else set()
        classes = all_classes - excluded
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for owner in sorted(excluded):
                path = owner[1:-1] + ".class"
                jar.writestr(path, (originals / path).read_bytes())
        inputs[str(dependency)] = sha(dependency)
        classpath = ["--classpath", args.android_jar.resolve()]
        if excluded:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            outputs = []
            for route in range(4):
                for shape in range(3):
                    for mode in range(12):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), prefix + "Harness",
                                             str(route), str(shape), str(mode)], f"{label}-{route}-{shape}-{mode}")
                        pattern = f"route={route},shape={shape},mode={mode},attempt=[01],events=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                            raise RuntimeError("Original abrupt blank final observer changed: " + output)
                        outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        wrongs = (
            ("lost-publication", "Base", "Trace.owner = this;", "Trace.owner = null;", 0, 0),
            ("wrong-super-event", "Base", "Trace.hit(1);", "Trace.hit(9);", 0, 0),
            ("wrong-allocated-failure", "Core", "throw new Failure();", "throw new IllegalStateException();", 0, 0),
            ("lost-abrupt-event", "Core", "private Core() {\n        Trace.hit(2);", "private Core() {", 0, 0),
            ("lost-partial-store", "Core", "this.number = number;\n        Trace.hit(2);", "Trace.hit(2);", 1, 0),
            ("late-partial-store", "Core", "this.number = number;\n        Trace.hit(2);", "Trace.hit(2);\n        this.number = number;", 1, 0),
            ("wrapped-original-failure", "Core", "throw failure;", "throw new Exception(failure);", 1, 0),
            ("narrowed-partial-store", "Core", "this.number = number;\n        Trace.hit(2);", "this.number = (int) number;\n        Trace.hit(2);", 1, 0),
            ("lost-normal-value", "Core", "this.value = value;", "this.value = null;", 2, 0),
            ("lost-normal-number", "Core", "this.number = number;\n        this.enabled", "this.number = 0;\n        this.enabled", 2, 0),
            ("wrong-normal-boolean", "Core", "this.enabled = enabled;", "this.enabled = !enabled;", 2, 0),
            ("lost-completion-event", "Core", "Trace.hit(4);", ";", 2, 0),
        )
        for label, owner, anchor, replacement, shape, mode in wrongs:
            original = (fixture / "fixture" / (owner + ".java")).read_text()
            if original.count(anchor) != 1:
                raise RuntimeError("Original abrupt constructor counterexample anchor changed: " + label)
            changed = work / label / (owner + ".java")
            changed.parent.mkdir()
            changed.open("x").write(original.replace(anchor, replacement))
            compiled = work / (label + "-classes")
            verify.compile_local([changed], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     prefix + "Harness", "1", str(shape), str(mode)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned abrupt blank final constructor invariant" not in result.stderr:
                raise RuntimeError("Frozen observer missed incorrect abrupt constructor: " + label)

        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        inputs[str(source_dex)] = sha(source_dex)
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original abrupt constructor DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original abrupt constructor definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-constructor-disassembly")
        witnesses = {}
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            prototype = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] != "<init>" or not prototype:
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
            witnesses[prototype[1]] = {"stores": [op for op, unused in ops if op.startswith("iput")],
                                       "throws": sum(op == "throw" for op, unused in ops),
                                       "returns": sum(op.startswith("return") for op, unused in ops)}
        expected = {"()V": {"stores": [], "throws": 1, "returns": 0},
                    "(Ljava/lang/Throwable;J)V": {"stores": ["iput-wide"], "throws": 1, "returns": 0},
                    "(Ljava/lang/Object;JZ)V": {"stores": ["iput-object", "iput-wide", "iput-boolean"], "throws": 0, "returns": 1}}
        if witnesses != expected:
            raise RuntimeError("D8 changed original abrupt constructor paths: " + repr(witnesses))
        (work / "input-method-witness.json").open("x").write(json.dumps(witnesses, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        report = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in report["methods"]} != methods or report["class_count"] != len(classes)
                or report["method_count"] != len(methods) or report["unrecovered_method_count"]
                or report["recovered_method_count"] != len(methods) or any(r["status"] != "recovered" for r in report["methods"])
                or report.get("declaration_only_method_count", 0) or report.get("projected_method_count", 0)
                or report["status"] != "recovered" or report.get("minimum_java_release", 8) != 8
                or report.get("class_source_bindings") or report.get("generic_signature_projections")
                or report.get("field_source_bindings") or report.get("default_instance_final_initializers")):
            raise RuntimeError("Abrupt constructors changed original definitions, finals or source contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if excluded else None, release=8)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in report.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes != classes or set(actual_methods) != methods | helpers or helpers != expected_helpers:
            raise RuntimeError("Abrupt constructor source invented or omitted a runtime definition")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(records[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if actual != wanted or any(after_exceptions[m] != exceptions[m] for m in records[owner]["methods"]):
                raise RuntimeError("Original abrupt constructor Code, access, fields, Signature or Exceptions changed: " + owner)
        runtime = (rebuilt, harness, dependency) if excluded else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Abrupt constructors changed original stores, partial state, exceptions or effects")
        if hashes(originals) != frozen["originals"] or hashes(harness) != frozen["harness"] or any(sha(Path(p)) != h for p, h in inputs.items()):
            raise RuntimeError("Original abrupt constructor inputs or frozen observers changed")
        summary = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                   "all_java_units": len(list((output / "sources").rglob("*.java"))), "fresh_jvm_cases": 144,
                   "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                   "independent_wrong_programs": len(wrongs), "all_original_definitions_and_compiler_metadata": "unchanged",
                   "blank_finals_partial_stores_reflective_mutations_and_exact_failures": "matched",
                   "original_inputs_and_immutable_callers": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
