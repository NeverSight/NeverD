#!/usr/bin/env python3
"""Verify original Kotlin static accessors retain nearest-superclass dispatch."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
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
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_synthetic_method_names import compiler_facts
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_synthetic_method_names import compiler_facts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--kotlin-lib", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--kotlin-java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parent", action="store_true")
    parser.add_argument("--integer-argument", action="store_true")
    parser.add_argument("--object-result", action="store_true")
    parser.add_argument("--captured-member", action="store_true")
    args = parser.parse_args()
    if sum((args.integer_argument, args.object_result, args.captured_member)) > 1:
        parser.error("Choose one independently verified accessor profile")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-static-super-accessors-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        kotlin_java = (choose_jdk(args.kotlin_java_home) / "bin" / ("java.exe" if os.name == "nt" else "java")
                       if args.kotlin_java_home else Path(verify.java))
        fixture = FIXTURES / "static_super_accessors"
        host = "Lfixture/Outer$Host;" if args.captured_member else "Lfixture/Host;"
        host_path = Path("fixture/Outer$Host.class" if args.captured_member else "fixture/Host.class")
        harness_name = "CapturedStaticSuperHarness" if args.captured_member else "StaticSuperHarness"
        if args.captured_member:
            prepared = work / "fixture"
            shutil.copytree(fixture, prepared)
            (prepared / "kotlin/fixture/Host.kt").unlink()
            shutil.copy2(FIXTURES / "captured_static_super_accessors/kotlin/fixture/Outer.kt",
                         prepared / "kotlin/fixture/Outer.kt")
            fixture = prepared
        harness_sources = [FIXTURES / "harness" / (name + ".java") for name in
                           (("CapturedStaticSuperHarness", "CapturedStaticSuperCaller") if args.captured_member
                            else ("StaticSuperHarness", "StaticSuperCaller"))]
        target_call = "super.ping()"
        prototype = "(" + host + ")V"
        helper_prototype = "()V"
        if args.object_result:
            prepared = work / "fixture"
            shutil.copytree(fixture, prepared)
            fixture = prepared
            path = fixture / "java/fixture/Base.java"
            text = path.read_text().replace("Trace.hit(8);", "Trace.result = Trace.initializedResult; Trace.hit(8);")
            text = text.replace("void ping()", "Object ping()")
            path.write_text(text.replace("Trace.hit(2);", "Object value = Trace.result; Trace.hit(2); return value;"))
            path = fixture / "java/fixture/Trace.java"
            text = path.read_text().replace("Object receiver;", "Object receiver, result, initializedResult, replacement;")
            path.write_text(text.replace("if (event == failAt) throw failure;", "if (event == failAt) throw failure;\n        if (event == 2) result = replacement;"))
            path = fixture / "kotlin/fixture/Host.kt"
            text = path.read_text().replace("fun callBase() {", "fun callBase(): Any? { return")
            path.write_text(text.replace("fun ping() { Trace.overrides++; Trace.hit(4) }",
                                         "fun ping(): Any? { Trace.overrides++; Trace.hit(4); return null }"))
            prototype = "(Lfixture/Host;)Ljava/lang/Object;"
            helper_prototype = "()Ljava/lang/Object;"
            harness_sources = [FIXTURES / "harness" / (name + ".java") for name in
                               ("ObjectStaticSuperHarness", "ObjectStaticSuperCaller")]
        if args.integer_argument:
            prepared = work / "fixture"
            shutil.copytree(fixture, prepared)
            fixture = prepared
            path = fixture / "java/fixture/Base.java"
            text = path.read_text().replace("Trace.hit(8);", "Trace.input = 113; Trace.hit(8);")
            text = text.replace("void ping()", "void ping(int value)")
            path.write_text(text.replace("Trace.receiver = this;", "Trace.receiver = this; Trace.argument = value;"))
            path = fixture / "java/fixture/Trace.java"
            path.write_text(path.read_text().replace("failAt, calls, overrides", "failAt, calls, overrides, input, argument"))
            path = fixture / "kotlin/fixture/Host.kt"
            text = path.read_text().replace("fun callBase()", "fun callBase(value: Int)")
            text = text.replace("fun ping()", "fun ping(value: Int)").replace(target_call, "super.ping(value)")
            path.write_text(text)
            target_call = "super.ping(value)"
            prototype = "(Lfixture/Host;I)V"
            helper_prototype = "(I)V"
            prepared_harness = work / "harness-source"
            prepared_harness.mkdir()
            copied = []
            for path in harness_sources:
                text = path.read_text()
                text = text.replace("void ping()", "void ping(int value)")
                text = text.replace("String accessor) throws", "String accessor, int value) throws")
                text = text.replace("run(Host host)", "run(Host host, int value)")
                text = text.replace("host.callBase$main()", "host.callBase$main(value)")
                text = text.replace("StaticSuperCaller.run(host)", "StaticSuperCaller.run(host, value)")
                text = text.replace("getMethod(accessor, Host.class)", "getMethod(accessor, Host.class, int.class)")
                text = text.replace("invoke(null, host)", "invoke(null, host, value)")
                text = text.replace("methodType(void.class, Host.class)", "methodType(void.class, Host.class, int.class)")
                text = text.replace("invokeExact(host)", "invokeExact(host, value)")
                text = text.replace("if (attempt == 1) {", "int argument = attempt == 1 ? Integer.MIN_VALUE : Integer.MAX_VALUE; Trace.input = argument;\n            if (attempt == 1) {")
                text = text.replace("call(host, route, args[3])", "call(host, route, args[3], argument)")
                text = text.replace("if (!initializerFailure) require(Trace.receiver == host);", "if (!initializerFailure) require(Trace.receiver == host);\n            if (calls != 0) require(Trace.argument == argument);")
                dest = prepared_harness / path.name
                dest.write_text(text)
                copied.append(dest)
            harness_sources = copied
        original_java, originals, harness = work / "original-java", work / "originals", work / "harness"
        kotlin = args.kotlin_lib.resolve()
        compiler_jars = sorted(kotlin.glob("*.jar"))
        if not (kotlin / "kotlin-compiler.jar").is_file() or not (kotlin / "kotlin-stdlib.jar").is_file():
            raise RuntimeError("The original witness requires the unmodified Kotlin compiler distribution")
        inputs = [args.android_jar.resolve(), *compiler_jars]
        input_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        stdlib = kotlin / "kotlin-stdlib.jar"
        annotations = kotlin / "annotations-13.0.jar"
        dependencies = []
        # NeverD deliberately rejects multi-release jars. Select the Java 8
        # base declarations explicitly, retaining the original class bytes.
        for jar in (stdlib, annotations):
            output = work / jar.name
            with zipfile.ZipFile(jar) as source, zipfile.ZipFile(output, "x") as dest:
                for entry in source.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):
                            raise RuntimeError("Unsafe compiler dependency entry")
                        dest.writestr(entry.filename, source.read(entry))
            dependencies.append(output)
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), original_java, release=8)
        originals.mkdir()
        verify.run([kotlin_java, "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (stdlib, original_java))),
                    "-jvm-target", "1.8", "-d", originals, *sorted((fixture / "kotlin").rglob("*.kt"))], "kotlinc-original")
        for path in sorted(original_java.rglob("*.class")):
            destination = originals / path.relative_to(original_java)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
        verify.compile_local(harness_sources, harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != (5 if args.captured_member else 4) or len(all_methods) != (12 if args.captured_member else 11):
            raise RuntimeError("Owned superclass-accessor inventory changed")
        classes = all_classes - ({"Lfixture/Base;", "Lfixture/Root;"} if args.dependency_parent else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        dependency = work / "owned-parent.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for name in ("Base", "Root"):
                jar.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        if args.dependency_parent:
            dependencies.append(dependency)
        dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        classpath = ["--classpath", args.android_jar.resolve()]
        for path in dependencies:
            classpath += ["--classpath", path]

        accessors = {identity: row for identity, row in records[host]["methods"].items()
                     if row["access"] & 0x1000}
        if len(accessors) != 1:
            raise RuntimeError("Original Kotlin static accessor inventory changed")
        identity, accessor = next(iter(accessors.items()))
        if accessor["access"] != 0x1019 or accessor["prototype"] != prototype:
            raise RuntimeError("Original Kotlin accessor contract changed")

        def execute(runtime, label):
            results = []
            cases = [(variant, route, mode) for variant in (0, 1) for route in range(4) for mode in (0, 2, 8, 7)]
            cases += [(2, route, mode) for route in (2, 3) for mode in (0, 2, 8, 7)]
            cases = [(variant, route, mode, result_case) for variant, route, mode in cases
                     for result_case in (range(2) if args.object_result else range(1))]
            for variant, route, mode, result_case in cases:
                output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                    "ObjectStaticSuperHarness" if args.object_result else harness_name,
                    str(variant), str(route), str(mode), accessor["name"],
                    str(int(label == "rebuilt")) if args.captured_member else str(result_case)],
                    f"{label}-{variant}-{route}-{mode}-{result_case}")
                if not re.fullmatch(f"variant={variant},route={route},mode={mode},trace=[0-9]+\\n", output):
                    raise RuntimeError("Original superclass accessor matrix changed: " + output)
                results.append(output)
            return results

        baseline = execute((originals, harness), "baseline")
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]

        def dex(root, dest):
            dest.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dest,
                        *[root / path.relative_to(originals) for path in paths]], dest.name + "-d8")
            if list(dest.glob("classes*.dex")) != [dest / "classes.dex"]:
                raise RuntimeError("Owned accessor fixture changed DEX inventory")
            return dest / "classes.dex"

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original superclass accessor runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-super-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] == host and name and name[1] == accessor["name"]:
                expected = (["invoke-super", "move-result-object", "return-object"] if args.object_result
                            else ["invoke-super", "return-void"])
                if re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2]) != expected:
                    raise RuntimeError("Kotlin/D8 changed the original static superclass accessor")
                witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original static superclass accessor")
        annotations_dump = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        if ("VISIBILITY_BUILD" in annotations_dump
                or re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations_dump)
                != ["Lkotlin/Metadata;"] * (2 if args.captured_member else 1)):
            raise RuntimeError("D8 changed the owned Kotlin annotation retention")
        class_annotations = {}
        for owner, row in records.items():
            for name, declaration in [(owner, row), *row["fields"].items(), *row["methods"].items()]:
                if declaration.get("runtime_invisible_annotations") or declaration.get("runtime_invisible_parameter_annotations"):
                    class_annotations[name] = {key: declaration.get(key) for key in
                                               ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations")}
        expected_class_annotations = ({"Lfixture/Host;->callBase$main()Ljava/lang/Object;",
                                      "Lfixture/Host;->ping()Ljava/lang/Object;"} if args.object_result else set())
        if set(class_annotations) != expected_class_annotations:
            raise RuntimeError("Owned superclass accessor acquired unverified CLASS annotations: " + repr(class_annotations))
        if any(value != {"runtime_invisible_annotations": [{"type": "Lorg/jetbrains/annotations/Nullable;", "elements": {}}],
                         "runtime_invisible_parameter_annotations": []} for value in class_annotations.values()):
            raise RuntimeError("Owned nullable result annotations changed")
        (work / "d8-class-annotation-differences.json").write_text(json.dumps(class_annotations, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {key for key, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count") != (4 if args.captured_member else 1)
                or projected != ({key for key in methods if key.startswith(host + "->")} if args.captured_member else {identity}) or coverage["status"] != "partial"
                or (bool(coverage.get("class_source_bindings")) != args.captured_member)
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Superclass accessor recovery changed original inventory or contracts")
        row = rows[identity]
        if (row.get("projection_kind") != "static-super-accessor"
                or row.get("source_identity") != identity or len(row.get("metadata_differences", [])) != 2):
            raise RuntimeError("Static accessor lacks exact original entry and projection disclosure")
        if args.captured_member:
            binding, = coverage["class_source_bindings"]
            constructor = rows[host + "-><init>(Lfixture/Outer;)V"]
            if (binding.get("class") != host or binding.get("binding_kind") != "inner-to-static-member"
                    or binding.get("outer_capture_field") != host + "->this$0:Lfixture/Outer;"
                    or binding.get("source_enclosing_class") != "Lfixture/Outer;"
                    or len(binding.get("metadata_differences", [])) != 4
                    or constructor.get("projection_kind") != "inner-to-static-member"
                    or constructor.get("generic_signature") != "()V"
                    or constructor.get("generic_signature_omitted_parameters") != 1):
                raise RuntimeError("Missing exact original constructor and capture projection receipts")
        helper = row.get("super_dispatch_helper")
        super_helpers = [value for value in coverage.get("generated_source_helpers", []) if value["kind"] == "super-dispatch-helper"]
        if (len(super_helpers) != 1 or super_helpers[0]["class"] != host
                or super_helpers[0]["prototype"] != helper_prototype or super_helpers[0]["static"]
                or helper != host + "->" + super_helpers[0]["name"] + helper_prototype):
            raise RuntimeError("Missing private superclass dispatch helper accounting")
        if coverage.get("minimum_java_release", 8) != (25 if args.captured_member else 8):
            raise RuntimeError("Superclass accessor unexpectedly raised its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=25 if args.captured_member else 8,
                             classpath=os.pathsep.join(map(str, dependencies)))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {value["class"] + "->" + value["name"] + value["prototype"] for value in coverage["generated_source_helpers"]}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes} | {helper}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Superclass accessor invented a runtime entry or omitted helper accounting")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        if after[host]["methods"][helper]["access"] != 0x2:
            raise RuntimeError("Superclass helper is not private and nonvirtual")
        for owner in classes:
            original, actual = records[owner], after[owner]
            wanted_class = copy.deepcopy(original)
            if args.captured_member and owner == host:
                wanted_class["inner_class"]["access"] |= 0x8
                wanted_class["fields"][host + "->this$0:Lfixture/Outer;"]["access"] &= ~0x1000
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if wanted_class.get(key) != actual.get(key):
                    raise RuntimeError("Original superclass class/field metadata changed: " + owner + ":" + key)
            for method, metadata in original["methods"].items():
                wanted = copy.deepcopy(metadata)
                if method in class_annotations:
                    wanted["runtime_invisible_annotations"] = []
                if method == identity:
                    wanted["access"] &= ~0x1000
                if args.captured_member and method == host + "-><init>(Lfixture/Outer;)V":
                    wanted["signature"] = None
                if actual["methods"].get(method) != wanted or after_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original accessor metadata changed beyond disclosed synthetic flag: " + method)
        if execute((rebuilt, harness), "rebuilt") != baseline:
            raise RuntimeError("Superclass accessor changed receiver, effects or original failures")
        negative = work / "nonsynthetic-originals"
        shutil.copytree(originals, negative)
        receipt = change_method_flags(negative / host_path, prototype.encode(), 0x1019, 0x19,
                                      name=accessor["name"].encode())
        (work / "nonsynthetic-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = dex(negative, work / "nonsynthetic-dex")
        rejected = work / "nonsynthetic-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "nonsynthetic-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "static super accessor" not in result.stderr:
            raise RuntimeError("Unsupported nonsynthetic superclass invocation was accepted")
        wrong_source = work / ("wrong-dispatch-source/Outer.kt" if args.captured_member else "wrong-dispatch-source/Host.kt")
        wrong_source.parent.mkdir()
        original_source = (fixture / ("kotlin/fixture/Outer.kt" if args.captured_member else "kotlin/fixture/Host.kt")).read_text()
        if original_source.count(target_call) != 1:
            raise RuntimeError("Owned superclass call changed")
        wrong_source.write_text(original_source.replace(target_call, target_call.removeprefix("super.")))
        wrong = work / "wrong-dispatch-classes"
        wrong.mkdir()
        verify.run([kotlin_java, "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (stdlib, original_java))),
                    "-jvm-target", "1.8", "-d", wrong, wrong_source], "kotlinc-wrong-dispatch")
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness, *dependencies))),
                                 "ObjectStaticSuperHarness" if args.object_result else harness_name,
                                 "1", "0", "0", accessor["name"], "0"], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-dispatch-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle did not detect virtual dispatch")
        if args.integer_argument:
            changed_source = work / "wrong-argument-source/Host.kt"
            changed_source.parent.mkdir()
            changed_source.write_text(original_source.replace(target_call, "super.ping(value + 1)"))
            changed = work / "wrong-argument-classes"
            changed.mkdir()
            verify.run([kotlin_java, "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                        "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (stdlib, original_java))),
                        "-jvm-target", "1.8", "-d", changed, changed_source], "kotlinc-wrong-argument")
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (changed, originals, harness, *dependencies))),
                                     "StaticSuperHarness", "1", "0", "0", accessor["name"]], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / "wrong-argument-counterexample.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "AssertionError" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect changed integer forwarding")
        if args.object_result:
            def wrong_result(label, text):
                java_source = work / (label + "-source") / "Base.java"
                java_source.parent.mkdir()
                java_source.write_text(text)
                changed = work / (label + "-classes")
                verify.compile_local([java_source], changed, release=8, classpath=originals)
                result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (changed, originals, harness, *dependencies))),
                                         "ObjectStaticSuperHarness", "1", "0", "0", accessor["name"], "0"],
                                        text=True, capture_output=True, timeout=300, env=verify.environment)
                (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
                if result.returncode == 0 or "AssertionError" not in result.stderr:
                    raise RuntimeError("Independent oracle did not detect changed superclass result: " + label)
            original_base = (fixture / "java/fixture/Base.java").read_text()
            wrong_result("discarded-result", original_base.replace("return value;", "return null;"))
            wrong_result("late-result", original_base.replace("return value;", "return Trace.result;"))
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != value for p, value in input_hashes.items())
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != value for p, value in dependency_hashes.items())):
            raise RuntimeError("Original compiler, input, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "dependency_parent": args.dependency_parent,
                    "integer_argument": args.integer_argument,
                    "object_result": args.object_result, "captured_member": args.captured_member,
                    "d8_class_annotation_differences": sorted(class_annotations),
                    "original_static_entry_and_nearest_superclass_dispatch": "matched",
                    "synthetic_flag_and_private_helper": "differences reported",
                    "original_metadata_and_exceptions": "unchanged beyond reported accessor and capture projections",
                    "same_code_counterexample": "rejected", "independent_virtual_dispatch_counterexample": "detected",
                    "initialization_receiver_and_exception_identity": "matched", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
