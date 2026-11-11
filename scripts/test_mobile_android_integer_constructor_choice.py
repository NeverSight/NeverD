#!/usr/bin/env python3
"""Verify one allocation with two integer-selected original constructor overloads."""
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
    parser.add_argument("--dependency-widget", action="store_true")
    parser.add_argument("--argument-on-nonpositive", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-integer-constructor-choice-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "integer_constructor_choice"
        reference, originals, harness, builder = (work / n for n in ("reference", "originals", "harness", "builder"))
        inputs = []
        for source in sorted((fixture / "java").rglob("*.java")):
            if args.argument_on_nonpositive and source.name == "Core.java":
                text = source.read_text()
                if text.count("source <= 0") != 1:
                    raise RuntimeError("Original integer constructor choice branch anchor changed")
                source = work / "flipped-java/fixture/Core.java"
                source.parent.mkdir(parents=True)
                source.open("x").write(text.replace("source <= 0", "source > 0", 1))
            inputs.append(source)
        verify.compile_local(inputs, reference, release=8)
        verify.compile_local([FIXTURES / "harness" / ("IntegerConstructorChoice" + suffix + ".java")
                              for suffix in ("Caller", "Harness")], harness, classpath=reference, release=8)
        exports = ["--add-exports", "java.base/jdk.internal.org.objectweb.asm=ALL-UNNAMED"]
        builder.mkdir()
        verify.run([verify.javac, *exports, "-d", builder,
                    fixture / "builder/IntegerConstructorChoiceInputBuilder.java"], "build-input-builder")
        originals.mkdir()
        for path in reference.rglob("*.class"):
            relative = path.relative_to(reference)
            if relative.as_posix() == "fixture/Core.class":
                continue
            target = originals / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.open("xb").write(path.read_bytes())
        verify.run([verify.java, *exports, "-cp", builder, "IntegerConstructorChoiceInputBuilder",
                    originals, str(args.argument_on_nonpositive).lower()], "build-bytecode-input")
        frozen = {name: hashes(path) for name, path in (("reference", reference), ("originals", originals),
                                                       ("harness", harness), ("builder", builder))}
        if frozen["reference"]["fixture/Core.class"] == frozen["originals"]["fixture/Core.class"]:
            raise RuntimeError("The independent bytecode input must retain its own allocation placement")
        for name in ("Widget", "Trace"):
            key = "fixture/" + name + ".class"
            if frozen["reference"][key] != frozen["originals"][key]:
                raise RuntimeError("Owned bytecode builder changed an observer or constructor")
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != 7:
            raise RuntimeError("Original integer constructor choice inventory changed")
        classes = all_classes - ({"Lfixture/Widget;"} if args.dependency_widget else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        dependency = work / "owned-widget.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            jar.writestr("fixture/Widget.class", (originals / "fixture/Widget.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_widget:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            outputs = []
            for route in range(4):
                for mode in range(16):
                    for warm in (False, True):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "IntegerConstructorChoiceHarness", str(route), str(mode), str(warm).lower(),
                            str(args.argument_on_nonpositive).lower()], f"{label}-{route}-{mode}-{warm}")
                        pattern = f"route={route},mode={mode},warm={str(warm).lower()},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                            raise RuntimeError("Original integer constructor choice matrix changed: " + output)
                        outputs.append(output)
            return outputs

        baseline = execute((reference, harness), "java-reference")
        if execute((originals, harness), "bytecode-original") != baseline:
            raise RuntimeError("Independent original programs disagree on allocation and constructor effects")
        text = next(p for p in inputs if p.name == "Core.java").read_text()
        selected = "source > 0" if args.argument_on_nonpositive else "source <= 0"
        reversed_test = "source <= 0" if args.argument_on_nonpositive else "source > 0"
        argument_mode = 3 if args.argument_on_nonpositive else 4
        default_mode = 0 if args.argument_on_nonpositive else 3
        mutations = (
            ("always-empty", "if(" + selected + ")return new Widget();", "if(true)return new Widget();", argument_mode, True),
            ("always-argument", "if(" + selected + ")return new Widget();", "if(false)return new Widget();", default_mode, True),
            ("reversed-choice", selected, reversed_test, 0, False),
            ("changed-argument", "new Widget(source)", "new Widget(source^1)", argument_mode, True),
            ("narrowed-argument", "new Widget(source)", "new Widget((byte)source)", argument_mode, True),
            ("late-argument", "new Widget(source)", "new Widget(Trace.argument)", argument_mode, False),
            ("zero-boundary", selected, "source >= 0" if args.argument_on_nonpositive else "source < 0", 2, True),
            ("duplicate-allocation", "try {", "try { new Widget();", 0, True),
            ("skipped-handler", "Trace.caught=failure;Trace.hit(8);return null;", "throw new IllegalStateException(failure);", 8, True),
            ("wrong-caught-identity", "Trace.caught=failure;", "Trace.caught=new IllegalArgumentException();", 8, True))
        for label, before, after, mode, warm in mutations:
            if text.count(before) != 1:
                raise RuntimeError("Original integer constructor choice counterexample anchor changed: " + label)
            source = work / label / "Core.java"
            source.parent.mkdir()
            source.open("x").write(text.replace(before, after, 1))
            wrong = work / (label + "-classes")
            verify.compile_local([source], wrong, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness))),
                "IntegerConstructorChoiceHarness", "1", str(mode), str(warm).lower(), str(args.argument_on_nonpositive).lower()],
                text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned integer constructor choice" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect constructor selection: " + label)
        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original integer constructor choice DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original integer constructor choice entry")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        # The independent builder must survive D8 as one allocation before its
        # integer branch and two distinct initializers, rather than two Java news.
        pattern = r"name\s*:\s*'make'([\s\S]*?)(?:catches\s*:)"
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "sdk-disassembly")
        match = re.search(pattern, disassembly)
        if not match:
            raise RuntimeError("Missing integer constructor choice DEX body")
        body = match.group(1)
        news = re.findall(r"new-instance[^\n]*Lfixture/Widget;", body)
        initializers = re.findall(r"invoke-direct[^\n]*Lfixture/Widget;\.<init>:[^\n]*", body)
        if len(news) != 1 or len(initializers) != 2 or not all(p in " ".join(initializers) for p in ("()V", "(I)V")):
            raise RuntimeError("D8 did not preserve the original integer constructor choice diamond")
        if not re.search(r"new-instance[\s\S]*if-(?:lez|gtz)[\s\S]*invoke-direct", body):
            raise RuntimeError("D8 moved the integer constructor choice branch ahead of its original allocation")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Constructor choice recovery changed original entries or source contracts")
        for row in coverage["methods"]:
            original = records[row["class"]]["methods"][row["identity"]]
            if row["status"] != ("recovered" if original["code"] else "declaration-only"):
                raise RuntimeError("Original integer constructor choice Code role changed")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
            classpath=dependency if args.dependency_widget else None, release=8)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Constructor choice recovery invented or omitted an original entry")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for c in classes:
            if {key: value for key, value in records[c].items() if key != "methods"} != {
                    key: value for key, value in after[c].items() if key != "methods"}:
                raise RuntimeError("Original integer constructor choice class metadata changed: " + c)
            for identity, metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original integer constructor choice method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_widget else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Constructor choice recovery changed effects or failure identities")
        for name, path in (("reference", reference), ("originals", originals), ("harness", harness), ("builder", builder)):
            if hashes(path) != frozen[name]:
                raise RuntimeError("An original input or frozen observer changed")
        if (hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("An original SDK declaration changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
            "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "original_programs": 2,
            "dependency_widget": args.dependency_widget, "argument_on_nonpositive": args.argument_on_nonpositive,
            "allocation_before_integer_choice_and_both_original_initializers": "matched",
            "class_initialization_partial_final_fields_and_handler_identity": "matched",
            "all_original_entries_metadata_and_exceptions": "unchanged",
            "independent_wrong_programs": len(mutations), "original_inputs_and_frozen_callers": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
