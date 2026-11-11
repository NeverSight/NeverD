#!/usr/bin/env python3
"""Verify exact nominal witnesses for original intersection-bound void calls."""
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
    from scripts.test_mobile_android_nested_return import method_code, class_records
    from scripts.test_mobile_android_synthetic_method_names import compiler_facts
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import method_code, class_records
    from test_mobile_android_synthetic_method_names import compiler_facts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--kotlin-lib", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-intersection-void-invocations-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "intersection_void_invocations"
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
        verify.run([verify.java, "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (stdlib, original_java))),
                    "-jvm-target", "1.8", "-d", originals, *sorted((fixture / "kotlin").rglob("*.kt"))], "kotlinc-original")
        for path in sorted(original_java.rglob("*.class")):
            destination = originals / path.relative_to(original_java)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("IntersectionVoidHarness", "IntersectionVoidCaller")], harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 6 or len(all_methods) != 12:
            raise RuntimeError("Owned intersection-call inventory changed")
        classes = all_classes - ({"Lfixture/Provider;"} if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            jar.writestr("fixture/Provider.class", (originals / "fixture/Provider.class").read_bytes())
        if args.dependency_provider:
            dependencies.append(dependency)
        dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        classpath = ["--classpath", args.android_jar.resolve()]
        for path in dependencies:
            classpath += ["--classpath", path]

        def execute(runtime, label):
            result = []
            cases = [(0, route, mode) for route in range(4) for mode in (0, 9, 1, 4, 2, 5, 3)]
            cases += [(1, 0, mode) for mode in (0, 9, 1, 4, 2)]
            cases += [(2, 0, mode) for mode in (0, 9, 1)]
            for variant, route, mode in cases:
                output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                    "IntersectionVoidHarness", str(variant), str(route), str(mode)], f"{label}-{variant}-{route}-{mode}")
                if not re.fullmatch(f"variant={variant},route={route},mode={mode},trace=[0-9]+\\n", output):
                    raise RuntimeError("Original intersection argument matrix changed: " + output)
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
            raise RuntimeError("Owned intersection call fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original intersection-void-invocations runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-argument-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] == "Lfixture/Host;" and name and name[1] == "run":
                ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
                if ops != ["move-object", "check-cast", "invoke-static", "return-void"]:
                    raise RuntimeError("Kotlin/D8 changed the original erased-self call witness")
                witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original erased-self call witness")
        annotations_dump = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        visible = re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations_dump)
        if "VISIBILITY_BUILD" in annotations_dump or visible != ["Lkotlin/Metadata;"]:
            raise RuntimeError("D8 changed the owned Kotlin annotation retention")
        for owner, row in records.items():
            for declaration in [row, *row["fields"].values(), *row["methods"].values()]:
                if declaration.get("runtime_invisible_annotations") or declaration.get("runtime_invisible_parameter_annotations"):
                    raise RuntimeError("Owned intersection fixture acquired unverified CLASS annotations")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Intersection call recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != 8:
            raise RuntimeError("Intersection call unexpectedly raised its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=os.pathsep.join(map(str, dependencies)))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if owner not in {"Lfixture/First;", "Lfixture/Second;", "Lfixture/Third;"}}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Intersection call recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original intersection call class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original intersection call method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Intersection call changed an original effect or cast position")
        # Change only the implemented interface, retaining every method's Code.
        negative = work / "wrong-bound-originals"
        shutil.copytree(originals, negative)
        target = negative / "fixture/Host.class"
        before = target.read_bytes()
        old, new = b"fixture/Second", b"fixture/Third"
        pattern = b"\x01" + len(old).to_bytes(2, "big") + old
        replacement = b"\x01" + len(new).to_bytes(2, "big") + new
        if before.count(pattern) != 1:
            raise RuntimeError("Missing exact owned interface witness")
        after = before.replace(pattern, replacement, 1)
        if (method_code(before) != method_code(after)
                or before[class_records(before)["pool_end"]:] != after[class_records(after)["pool_end"]:]):
            raise RuntimeError("Negative interface edit changed Code or descriptors")
        target.write_bytes(after)
        negative_output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (negative, harness, *dependencies))),
                                     "IntersectionVoidHarness", "3", "0", "0"], "wrong-bound-original-runtime")
        if negative_output != "variant=3,route=0,mode=0,trace=9142142\n":
            raise RuntimeError("Wrong-bound original did not fail at its original later cast")
        negative_dex = work / "wrong-bound-dex"
        negative_dex.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", negative_dex,
                    *[negative / path.relative_to(originals) for path in paths]], "wrong-bound-d8")
        rejected = work / "wrong-bound-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex / "classes.dex"),
                                 *map(str, classpath), "-o", str(rejected)], text=True, capture_output=True,
                                timeout=300, env=verify.environment)
        (work / "wrong-bound-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "intersection generic invocation" not in result.stderr:
            raise RuntimeError("Unproved intersection argument was accepted")
        (work / "wrong-bound-encoding.json").write_text(json.dumps({"code_and_descriptors": "unchanged",
            "original_late_cast": "verified", "recovery": "rejected"}, indent=2) + "\n")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != value for p, value in input_hashes.items())
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != value for p, value in dependency_hashes.items())):
            raise RuntimeError("Original inputs, compiler dependencies or immutable callers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": 36, "dependency_provider": args.dependency_provider,
                    "original_metadata_and_exceptions": "unchanged", "intersection_self_witness_and_original_late_casts": "matched",
                    "original_exception_identity": "matched", "same_code_counterexamples": "rejected",
                    "wrong_bound_original_runtime": "late cast retained", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
