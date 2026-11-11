#!/usr/bin/env python3
"""Verify forward jumps between allocation and nullary initialization."""
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
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-inputs", action="store_true")
    parser.add_argument("--shared-cleanup", action="store_true")
    parser.add_argument("--argument-tail", action="store_true")
    args = parser.parse_args()
    if args.argument_tail and args.shared_cleanup:
        parser.error("--argument-tail uses its original D8 input")
    if args.shared_cleanup and args.dependency_inputs:
        parser.error("--shared-cleanup owns only Client and uses the other original declarations as dependencies")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-forward-allocation-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "forward_allocation/java"
        if args.argument_tail:
            staged = work / "argument-inputs"
            shutil.copytree(fixture, staged)
            variant = FIXTURES / "forward_allocation/variants/arguments/java"
            for source in variant.rglob("*.java"):
                shutil.copy2(source, staged / source.relative_to(variant))
            fixture = staged
        harness_name = "ForwardAllocationTailHarness" if args.argument_tail else "ForwardAllocationHarness"
        builder_name = "ForwardAllocationTailInputBuilder" if args.argument_tail else "ForwardAllocationInputBuilder"
        originals, harness = work / "originals", work / "harness"
        reference, builder = work / "reference", work / "builder"
        verify.compile_local(sorted(fixture.rglob("*.java")), reference, release=8)
        exports = ["--add-exports", "java.base/jdk.internal.org.objectweb.asm=ALL-UNNAMED"]
        builder.mkdir()
        builder_source = FIXTURES / "forward_allocation/builder" / (builder_name + ".java")
        if args.shared_cleanup:
            text = builder_source.read_text()
            before = "        method.visitInsn(ACONST_NULL); method.visitInsn(ARETURN);\n        method.visitMaxs"
            if text.count(before) != 1:
                raise RuntimeError("Authored shared cleanup builder anchor changed")
            target = work / "shared-builder" / builder_source.name
            target.parent.mkdir()
            target.open("x").write(text.replace(before, "        method.visitJumpInsn(GOTO, one);\n        method.visitMaxs"))
            builder_source = target
        verify.run([verify.javac, *exports, "-d", builder,
                    builder_source], "compile-builder")
        originals.mkdir()
        for path in reference.rglob("*.class"):
            relative = path.relative_to(reference)
            if relative.as_posix() == "fixture/Client.class":
                continue
            target = originals / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open("xb") as out:
                out.write(path.read_bytes())
        verify.run([verify.java, *exports, "-cp", builder, builder_name, originals], "build-input")
        reference_hashes, builder_hashes = hashes(reference), hashes(builder)
        if reference_hashes["fixture/Client.class"] == hashes(originals)["fixture/Client.class"]:
            raise RuntimeError("Authored bytecode must retain its distinct forward allocation placement")
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("ForwardAllocationCaller", harness_name)],
                             harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 4 or len(all_methods) != (10 if args.argument_tail else 8):
            raise RuntimeError("Owned forward allocation inventory changed")
        providers = all_classes - {"Lfixture/Client;"} if args.shared_cleanup else (
            {"Lfixture/Target;"} if args.dependency_inputs else set())
        classes = all_classes - providers
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
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
        cases = [(selector, 0, 0) for selector in (-1, 0, 1, 2, -2147483648, 2147483647)]
        cases += [(-1, mode, kind) for mode in (1, 2, 5, 6) for kind in (0, 1)]
        cases += [(0, mode, kind) for mode in (3, 4, 5) for kind in (0, 1)]

        if args.argument_tail:
            cases += [(-1, mode, kind) for mode in (7, 8) for kind in (0, 1)]
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({
            "reference": reference_hashes, "builder": builder_hashes,
            "originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")

        def execute(runtime, label):
            outputs = []
            for route in range(2):
                for selector, mode, kind in cases:
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        harness_name, str(route), str(selector), str(mode), str(kind)],
                        f"{label}-{route}-{selector}-{mode}-{kind}")
                    pattern = f"route={route},selector={selector},mode={mode},error={kind},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                        raise RuntimeError("Owned forward allocation observer changed: " + output)
                    outputs.append(output)
            return outputs

        baseline = execute((reference, harness), "reference")
        if execute((originals, harness), "baseline") != baseline:
            raise RuntimeError("Authored ASM input differs from its independent Java specification")
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned forward allocation DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an owned forward allocation entry")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-forward-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Client;" or not name or name[1] != "make":
                continue
            lines = [line for line in block[2].splitlines() if re.search(r"\|[0-9a-fA-F]+:", line)]
            news = [i for i, line in enumerate(lines) if "new-instance" in line and "Lfixture/Target;" in line]
            descriptor = "([Ljava/lang/Object;Ljava/lang/String;)V" if args.argument_tail else "()V"
            inits = [i for i, line in enumerate(lines) if "invoke-direct" in line and "Lfixture/Target;.<init>:" + descriptor in line]
            if (len(news) != 1 or len(inits) != 1 or inits[0] <= news[0] + 2
                    or "goto" not in lines[news[0] + 1]
                    or not any(": throw " in line for line in lines[news[0]:inits[0]])):
                raise RuntimeError("D8 changed the forward jump and physically separate sibling witness")
            if args.argument_tail:
                tail = lines[news[0]:inits[0]]
                if (sum("filled-new-array" in line for line in tail) != 1
                        or sum("new-instance" in line and "Ljava/lang/StringBuilder;" in line for line in tail) != 1
                        or not any("Lfixture/Trace;.read:" in line for line in tail)
                        or not any("Lfixture/Trace;.text:" in line for line in tail)):
                    raise RuntimeError("Original forward argument array and nested allocation witness changed")
            positions = [int(re.search(r"\|([0-9a-fA-F]+):", lines[i])[1], 16) for i in (news[0], inits[0])]
            ranges = re.findall(r"0x([0-9a-f]+) - 0x([0-9a-f]+)\s+Ljava/lang/RuntimeException; -> 0x([0-9a-f]+)", block[2])
            edges = [{target for lo, hi, target in ranges if int(lo, 16) <= pc < int(hi, 16)} for pc in positions]
            if not edges[0] or edges[0] != edges[1]:
                raise RuntimeError("Forward allocation and initializer lost their shared catch")
            witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original forward allocation witness")
        # D8 duplicates this return block during lowering. Keep the exact
        # shared cleanup layout in authored smali, alongside the executable
        # ASM input and independent Java specification checked above.
        source_input = FIXTURES / "forward_allocation/smali/Client.smali" if args.shared_cleanup else source_dex
        source_hash = hashlib.sha256(source_input.read_bytes()).hexdigest()
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_input, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != (14 if args.argument_tail else 8)):
            raise RuntimeError("Forward allocation recovery changed original entries or contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=14 if args.argument_tail else 8,
                             classpath=dependency if providers else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if owner != "Lfixture/Hook;"}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Forward allocation recovery invented or omitted a runtime entry")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != after[owner].get(key):
                    raise RuntimeError("Original forward allocation class metadata changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                if after[owner]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original forward allocation method metadata/Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if providers else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Forward allocation changed ordered effects or reference identities")
        mutations = [
            ("lost-allocation", "Client", "default: return new Target();", "default: return null;", -1, 0),
            ("lost-catch", "Client", "Trace.caught = failure;", "Trace.caught = null;", 0, 0),
            ("lost-cleanup", "Client", "Trace.hit(3);", "", 0, 0),
            ("lost-reference", "Target", "first = Trace.value;", "first = null;", -1, 0),
            ("late-partial-store", "Target", "first = Trace.value;\n        Trace.hit(2);", "Trace.hit(2);\n        first = Trace.value;", -1, 2),
        ]
        if args.argument_tail:
            expression = "new Target(new Object[] {Trace.read()}, new StringBuilder().append(Trace.text()).toString())"
            mutations[0] = ("lost-allocation", "Client", "default: return " + expression + ";", "default: return null;", -1, 0)
            mutations += [
                ("hoisted-arguments", "Client", "default: return " + expression + ";",
                 "default: Object early = Trace.read(); return new Target(new Object[] {early}, new StringBuilder().append(Trace.text()).toString());", -1, 0),
                ("lost-array-value", "Target", "arguments = values;", "arguments = new Object[] {null};", -1, 0),
                ("lost-text", "Target", "text = message;", 'text = "owned different";', -1, 0),
                ("repeated-read", "Trace", "hit(7); return value;", "hit(7); hit(7); return value;", -1, 0),
                ("wrapped-read-failure", "Trace", "hit(7); return value;",
                 "try { hit(7); } catch (RuntimeException failure) { throw new IllegalStateException(failure); } return value;", -1, 7)]
        for label, name, before, replacement, selector, mode in mutations:
            text = (fixture / ("fixture/" + name + ".java")).read_text()
            if text.count(before) != 1:
                raise RuntimeError("Original forward allocation counterexample anchor changed")
            path = work / label / (name + ".java")
            path.parent.mkdir()
            path.open("x").write(text.replace(before, replacement))
            wrong = work / (label + "-classes")
            verify.compile_local([path], wrong, classpath=reference, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, reference, harness))),
                harness_name, "1", str(selector), str(mode), "0"], text=True, capture_output=True,
                timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned forward allocation contract" not in result.stderr:
                raise RuntimeError("Independent observer missed wrong forward allocation: " + label)
        if (hashes(reference) != reference_hashes or hashes(builder) != builder_hashes
                or hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(source_input.read_bytes()).hexdigest() != source_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original forward allocation inputs or frozen observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_inputs": args.dependency_inputs,
                    "forward_allocation_catch_class_init_and_partial_stores": "matched",
                    "authored_asm_and_independent_java_specification": "matched",
                    "shared_cleanup": args.shared_cleanup, "argument_tail": args.argument_tail,
                    "input_method_bytecode": "authored shared-cleanup smali" if args.shared_cleanup else "authored ASM lowered by D8",
                    "all_original_entries_metadata_and_exceptions": "unchanged", "independent_wrong_programs": len(mutations),
                    "original_inputs_and_immutable_callers": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
