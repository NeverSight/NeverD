#!/usr/bin/env python3
"""Verify class/interface first-argument witnesses survive closed callback preparation."""
from __future__ import annotations

import argparse
import copy
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
    from scripts.test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
    from scripts.test_mobile_android_nested_return import method_code, class_records
    from scripts.test_mobile_android_synthetic_method_names import compiler_facts
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
    from test_mobile_android_nested_return import method_code, class_records
    from test_mobile_android_synthetic_method_names import compiler_facts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--kotlin-lib", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--kotlin-java-home", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--branch-entry", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-intersection-callback-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "intersection_callback"
        harness_sources = [FIXTURES / "harness" / (name + ".java") for name in
                           ("IntersectionCallbackHarness", "IntersectionCallbackCaller", "IntersectionCallbackWrongBoundHarness")]
        if args.branch_entry:
            copied = work / "fixture"
            shutil.copytree(fixture, copied)
            fixture = copied
            def replace_once(path, old, new):
                text = path.read_text()
                if text.count(old) != 1:
                    raise RuntimeError("Original branch-entry witness anchor changed")
                path.write_text(text.replace(old, new))
            replace_once(fixture / "kotlin/fixture/Host.kt",
                         "fun run() { Provider.observe(this, fetch()) }",
                         "fun run() { if (Trace.branch) Trace.hit(7); Provider.observe(this, fetch()) }")
            replace_once(fixture / "java/fixture/Trace.java", "    public static long events;",
                         "    public static boolean branch;\n    public static long events;")
            sources = work / "harness-source"
            sources.mkdir()
            harness_sources = [Path(shutil.copy2(path, sources / path.name)) for path in harness_sources]
            target = sources / "IntersectionCallbackHarness.java"
            for old, new in (
                ("Trace.failAt=mode>=1", "Trace.failAt=mode==11?7:mode>=1"),
                ("Trace.events=0;Trace.node", "Trace.branch=mode==11||(mode+attempt)%2!=0;\n            Trace.events=0;Trace.node"),
                ('String steps="5";', 'String steps=Trace.branch?"75":"5";'),
                ("if(mode!=5&&!poisoned)", "if(mode!=5&&mode!=11&&!poisoned)"),
                ("long expected=0;for(char digit", 'if(mode==11)steps="7";\n            long expected=0;for(char digit'),
                ("Trace.getterCalls==oldGetter+1", "Trace.getterCalls==oldGetter+(mode==11?0:1)"),
                ("if(mode>=1&&mode<=6)require", "if((mode>=1&&mode<=6)||mode==11)require"),
            ):
                replace_once(target, old, new)
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
        verify.run([choose_jdk(args.kotlin_java_home) / "bin/java", "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
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
        if len(all_classes) != 8 or len(all_methods) != 14:
            raise RuntimeError("Owned intersection-call inventory changed")
        classes = all_classes - ({"Lfixture/Provider;"} if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        def facts(root):
            return {r["name"]: r for path in sorted(root.rglob("*.class"))
                    if "L"+path.relative_to(root).as_posix()[:-6]+";" in classes
                    for r in [KotlinFixtureClassFile(path.read_bytes()).facts()]}
        records, exceptions = facts(originals), exception_inventory(originals)
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
            results=[]
            for route in range(4):
                for mode in range(12 if args.branch_entry else 11):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,(*runtime,*dependencies))),
                        "IntersectionCallbackHarness",str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original callback matrix changed: "+output)
                    results.append(output)
            return results
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
                expected_ops = ["move-object", "check-cast", "invoke-virtual", "move-result-object", "invoke-static", "return-void"]
                if (ops[-6:] != expected_ops or (not args.branch_entry and ops != expected_ops)
                        or (args.branch_entry and ops[:-6] != ["sget-boolean", "if-eqz", "const/4", "invoke-static"])):
                    raise RuntimeError("Kotlin/D8 changed the original erased-self call witness")
                if args.branch_entry:
                    instructions = re.findall(r"\|([0-9a-fA-F]+):\s+(.+)", block[2])
                    branch = re.fullmatch(r"if-eqz\s+v\d+,\s*([0-9a-fA-F]+)\s+//.*", instructions[1][1])
                    if not branch or int(branch[1], 16) != int(instructions[4][0], 16):
                        raise RuntimeError("Original branch does not enter the argument copy")
                witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original erased-self call witness")
        annotations_dump = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        visible = re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations_dump)
        if "VISIBILITY_BUILD" in annotations_dump or visible != ["Lkotlin/Metadata;"]:
            raise RuntimeError("D8 changed the owned Kotlin annotation retention")
        dex_records=copy.deepcopy(records)
        dropped={}
        for owner,record in dex_records.items():
            for identity,row in [(owner,record),*record["fields"].items(),*record["methods"].items()]:
                for key in ("runtime_invisible_annotations","runtime_invisible_parameter_annotations"):
                    if row.get(key):dropped.setdefault(identity,{})[key]=row[key]
                    row[key]=[]
        if len(dropped)!=3:raise RuntimeError("Original callback CLASS annotation sites changed")
        (work/"d8-annotation-differences.json").write_text(json.dumps(dropped,indent=2)+"\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
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
                            for owner in classes if owner not in {"Lfixture/Observer;", "Lfixture/Other;", "Lfixture/Callback;"}}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Intersection call recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = dex_records[owner], actual_records[owner]
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
        old, new = b"fixture/Observer", b"fixture/Other"
        pattern = b"\x01" + len(old).to_bytes(2, "big") + old
        replacement = b"\x01" + len(new).to_bytes(2, "big") + new
        if before.count(pattern) != 1:
            raise RuntimeError("Missing exact owned interface witness")
        after = before.replace(pattern, replacement, 1)
        if (method_code(before) != method_code(after)
                or before[class_records(before)["pool_end"]:] != after[class_records(after)["pool_end"]:]):
            raise RuntimeError("Negative interface edit changed Code or descriptors")
        target.write_bytes(after)
        result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(negative,harness,*dependencies))),
            "IntersectionCallbackWrongBoundHarness"],text=True,capture_output=True,timeout=300,env=verify.environment)
        (work/"wrong-bound-original-runtime.log").write_text(result.stdout+result.stderr)
        if result.returncode!=0 or result.stdout!="original-late-cast=591\n":
            raise RuntimeError("Changed class/interface bound did not fail at its original later cast")
        negative_dex = work / "wrong-bound-dex"
        negative_dex.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", negative_dex,
                    *[negative / path.relative_to(originals) for path in paths]], "wrong-bound-d8")
        rejected = work / "wrong-bound-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex / "classes.dex"),
                                 *map(str, classpath), "--timeout", "600", "-o", str(rejected)], text=True, capture_output=True,
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
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "branch_entry": args.branch_entry,
                    "original_metadata_and_exceptions": "unchanged after recorded D8 CLASS annotation loss", "class_interface_witness_callback_identity_and_original_late_casts": "matched",
                    "original_exception_identity": "matched", "same_code_counterexamples": "rejected",
                    "wrong_bound_original_runtime": "late cast retained", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
