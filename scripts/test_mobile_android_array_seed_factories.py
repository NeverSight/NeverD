#!/usr/bin/env python3
"""Verify generic array seed factories retain array identity, stores and returned carriers."""
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
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--element-result", action="store_true")
    parser.add_argument("--closed-context", action="store_true")
    args = parser.parse_args()
    if args.closed_context and args.element_result:
        parser.error("closed-context and element-result are separate original contracts")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-array-seed-factories-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "array_seed_factories/java"
        harness_prefix = "ArraySeed"
        harness_class = "ArrayFormalHarness" if args.element_result else harness_prefix + "Harness"
        harness_sources = [FIXTURES / "harness" / (name + ".java") for name in
                           (harness_prefix + "Caller", harness_class)]
        if args.closed_context:
            authored = work / "context-source"
            shutil.copytree(fixture, authored)
            fixture = authored
            def replace_file(path, replacements):
                text = path.read_text()
                for before, after in replacements:
                    if text.count(before) != 1:
                        raise RuntimeError("Owned array context source anchor changed: " + before)
                    text = text.replace(before, after)
                path.write_text(text)
            replace_file(fixture / "fixture/Trace.java", ((
                "public static Object seen, replacement, result, after;",
                "public static Object seen, replacement, result, after, context;\n"
                "    public static Trace selected;\n    public static int stamp;\n    public int marker;"),))
            replace_file(fixture / "fixture/api/Provider.java", (
                ("make(T[] values)", "make(T[] values, Trace context)"),
                ("Trace.seen = values; Trace.hit(2);", "Trace.seen = values; Trace.context = context; Trace.hit(2);"),
                ("Trace.length = values.length;", "Trace.stamp = context.marker; Trace.length = values.length;")))
            client = fixture / "fixture/Client.java"
            text = client.read_text()
            if text.count("Facade.make(values)") != 2:
                raise RuntimeError("Owned array context caller changed")
            client.write_text(text.replace("Facade.make(values)", "Facade.make(values, Trace.selected)"))
            copied = work / "context-harness-source"
            copied.mkdir()
            for original in harness_sources:
                shutil.copy2(original, copied / original.name)
            harness_sources = [copied / p.name for p in harness_sources]
            replace_file(copied / "ArraySeedHarness.java", (
                ('owner.getDeclaredMethod("make", Object[].class)', 'owner.getDeclaredMethod("make", Object[].class, Trace.class)'),
                ("parameters.length == 1", "parameters.length == 2 && parameters[1] == Trace.class"),
                ("Trace.events = 0; Trace.after = sentinel; Trace.seen = sentinel; Trace.length = -1;",
                 "Trace.events = 0; Trace.after = sentinel; Trace.seen = sentinel; Trace.length = -1;\n"
                 "            Trace.selected = mode == 17 ? null : new Trace();\n"
                 "            if (Trace.selected != null) Trace.selected.marker = 71 + attempt;\n"
                 "            Trace.context = sentinel; Trace.stamp = -1;"),
                ("boolean length = body && mode != 2 && mode != 5;", "boolean length = body && mode != 2 && mode != 5 && mode != 17;"),
                ("if (values != null && values.length != 0) require(values[0]",
                 "require(Trace.context == (body ? Trace.selected : sentinel)\n"
                 "                && Trace.stamp == (body && mode != 2 && mode != 17 ? 71 + attempt : -1));\n"
                 "            if (values != null && values.length != 0) require(values[0]"),
                ("mode == 5 || (checked && mode == 10)", "mode == 5 || mode == 17 || (checked && mode == 10)")))
        if args.element_result:
            authored = work / "element-source";shutil.copytree(fixture, authored);fixture = authored
            provider = fixture / "fixture/api/Provider.java";text = provider.read_text()
            for before,after in (("List<T> make", "T make"), ("(List<T>) Trace.result", "(T) Trace.result")):
                if text.count(before)!=1:raise RuntimeError("Array formal provider source anchor changed")
                text=text.replace(before,after)
            provider.write_text(text)
            client = fixture / "fixture/Client.java";text = client.read_text()
            for before,after in (("List<?> result = (List)", "Object result ="),
                                 ("List<String> result = (List)", "Object result ="),
                                 ("return result.get(0);", "return (String) result;")):
                if text.count(before)!=1:raise RuntimeError("Array formal caller source anchor changed")
                text=text.replace(before,after)
            client.write_text(text)
        operations, modes = (2, 18 if args.closed_context else 17)
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local(harness_sources, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 12:
            raise RuntimeError("Owned array-seed-factories inventory changed")
        providers = {"Lfixture/api/Provider;", "Lfixture/api/Middle;", "Lfixture/api/Facade;"}
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
            for route in range(4):
                for operation in range(operations):
                    for mode in range(modes):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            harness_class, str(route), str(operation), str(mode)], f"{label}-{route}-{operation}-{mode}")
                        expected = f"route={route},operation={operation},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Original array seed factory behavior matrix changed: " + output)
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
            raise RuntimeError("Owned array-seed-factory fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original array-seed-factories runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-argument-disassembly")
        if "Lfixture/api/Facade;.make:" not in disassembly:
            raise RuntimeError("D8 lost the inherited array seed factory call witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Array seed factory invocation recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != 8:
            raise RuntimeError("Array seed factory invocation unexpectedly raised its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Array seed factory invocation recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original array-seed-factory class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original array-seed-factory method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Array seed factory call changed an original effect or result")
        original_signature = b"<T:Ljava/lang/Object;>([TT;)Ljava/util/List<TT;>;"
        negatives = (
                ("unknown-array-formal", original_signature.replace(b"([TT;)", b"([TU;)")),
                ("variant-result", original_signature.replace(b"List<TT;>", b"List<+TT;>")),
                ("closed-result", original_signature.replace(b"List<TT;>", b"List<Ljava/lang/Object;>")),
                ("additional-bound", original_signature.replace(b"T:Ljava/lang/Object;", b"T:Ljava/lang/Object;:Ljava/io/Serializable;")),
                ("extra-formal", original_signature.replace(b"T:Ljava/lang/Object;>", b"T:Ljava/lang/Object;U:Ljava/lang/Object;>")))
        if args.element_result:
            original_signature=b"<T:Ljava/lang/Object;>([TT;)TT;"
            negatives=(
                ("unknown-array-formal",original_signature.replace(b"[TT;",b"[TU;")),
                ("additional-bound",original_signature.replace(b"T:Ljava/lang/Object;",b"T:Ljava/lang/Object;:Ljava/io/Serializable;")),
                ("extra-formal",original_signature.replace(b"T:Ljava/lang/Object;>",b"T:Ljava/lang/Object;U:Ljava/lang/Object;>")))
        if args.closed_context:
            original_signature = original_signature.replace(b"[TT;)", b"[TT;Lfixture/Trace;)")
            negatives = tuple((label, changed.replace(b"[TT;)", b"[TT;Lfixture/Trace;)"))
                              if b"[TT;)" in changed else
                              (label, changed.replace(b"[TU;)", b"[TU;Lfixture/Trace;)"))
                              for label, changed in negatives)
            negatives += tuple((label, original_signature.replace(before, after))
                               for label, before, after in (
                ("open-context", b"Lfixture/Trace;", b"TT;"),
                ("unknown-context", b"Lfixture/Trace;", b"Lfixture/Absent;"),
                ("contravariant-result", b"List<TT;>", b"List<-TT;>"),
                ("changed-array-rank", b"[TT;", b"[[TT;")))
        for label, changed in negatives:
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/api/Provider.class", original_signature, changed)
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_classpath = list(classpath)
            if args.dependency_provider:
                negative_jar = work / (label + "-provider.jar")
                with zipfile.ZipFile(negative_jar, "x", compression=zipfile.ZIP_DEFLATED) as jar:
                    for owner in sorted(providers):
                        name = owner[1:-1] + ".class"
                        jar.writestr(name, (negative / name).read_bytes())
                negative_classpath[-1] = negative_jar
            negative_dex = work / (label + "-dex")
            negative_dex.mkdir()
            negative_paths = [negative / path.relative_to(originals) for path in paths]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *negative_classpath,
                        "--output", negative_dex, *negative_paths], label + "-d8")
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex / "classes.dex"),
                                     *map(str, negative_classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            diagnostic = "generic" if args.element_result or args.closed_context or label == "unknown-array-formal" else "generic invocation requires"
            if result.returncode == 0 or rejected.exists() or diagnostic not in result.stderr:
                raise RuntimeError("Array seed factory invocation accepted an unproved method contract: " + label)
        # Independent implementations that bypass or repeat the original
        # invocation must fail the unchanged side-effect and identity oracle.
        body = (fixture / "fixture/Client.java").read_text()
        result_type = "Object" if args.element_result else "List<?>"
        invocation = "Facade.make(values" + (", Trace.selected" if args.closed_context else "") + ");"
        marker = result_type + " result = " + ("" if args.element_result else "(List) ") + invocation
        if body.count(marker) != (2 if args.element_result else 1):
            raise RuntimeError("Owned carrier call witness changed")
        wrong_programs = [
            ("bypass", result_type + " result = (" + result_type + ") Trace.result;", 0),
            ("duplicate", invocation + " " + marker, 0),
            ("copied-array", marker.replace("make(values", "make(values.clone()"), 0)]
        if args.closed_context:
            wrong_programs += [
                ("changed-context", marker.replace("Trace.selected", "new Trace()"), 0),
                ("guarded-context", marker.replace("Trace.selected", "Trace.selected == null ? new Trace() : Trace.selected"), 17)]
        for label, replacement, wrong_mode in wrong_programs:
            wrong = work / label / "fixture/Client.java"
            wrong.parent.mkdir(parents=True)
            wrong.write_text(body.replace(marker, replacement))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     harness_class, "1", "0", str(wrong_mode)], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "array seed factory changed" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if args.element_result:
            before="Object result = Facade.make(values);\n        Trace.after = result; Trace.hit(5);\n        return (String) result;"
            after="String result = (String) Facade.make(values);\n        Trace.after = result; Trace.hit(5);\n        return result;"
            if body.count(before)!=1:raise RuntimeError("Original delayed result cast anchor changed")
            wrong=work/"early-result-cast/fixture/Client.java";wrong.parent.mkdir(parents=True);wrong.open("x").write(body.replace(before,after))
            compiled=work/"early-result-cast-classes";verify.compile_local([wrong],compiled,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),
                harness_class,"1","1","9"],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/"early-result-cast-counterexample.log").open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "array seed factory changed" not in result.stderr:raise RuntimeError("Frozen observer missed early result cast")
            wrong=work/"signature-drift/fixture/api/Provider.class";wrong.parent.mkdir(parents=True);shutil.copy2(originals/"fixture/api/Provider.class",wrong)
            receipt=change_signature(wrong,original_signature,original_signature.replace(b")TT;",b")Ljava/lang/Object;"))
            (work/"signature-drift-encoding.json").open("x").write(json.dumps(receipt,indent=2)+"\n")
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong.parents[2],originals,harness))),
                harness_class,"1","0","0"],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/"signature-drift-counterexample.log").open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "array seed factory changed" not in result.stderr:raise RuntimeError("Frozen observer missed unchanged-Code formal result drift")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "element_result": args.element_result, "closed_context": args.closed_context,
                    "independent_wrong_programs": 5 if args.element_result else len(wrong_programs),
                    "same_code_signature_mutations": len(negatives),
                    "original_metadata_and_exceptions": "unchanged", "array_seed_identity_stores_and_original_mutable_or_immutable_result": "matched",
                    "original_exception_identity": "matched", "same_code_counterexamples": "rejected",
                    "independent_bypass_repeat_and_copied_array_counterexamples": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
