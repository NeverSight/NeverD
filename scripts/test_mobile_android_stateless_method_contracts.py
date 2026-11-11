#!/usr/bin/env python3
"""Verify original generic implementation bridges on stateless anonymous members."""
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_nested_return import change_signature

ANONYMOUS = "Lfixture/Owner$shared$1;"
ECHO = b"<R:Ljava/lang/Object;S:Ljava/lang/Object;>(Ljava/lang/Class<TR;>;TR;)TR;"
FIXED = b"(Lfixture/Carrier<Ljava/lang/Number;Ljava/lang/String;>;)Lfixture/Carrier<Ljava/lang/Number;Ljava/lang/String;>;"

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--kotlin-lib", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--kotlin-java-home", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-root", action="store_true")
    parser.add_argument("--synthetic", action="store_true")
    args = parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-stateless-method-contracts-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "stateless_method_contracts"
        kotlin = args.kotlin_lib.resolve()
        if not (kotlin / "kotlin-compiler.jar").is_file(): raise RuntimeError("Missing original Kotlin compiler")
        inputs = [args.android_jar.resolve(), *sorted(kotlin.glob("*.jar"))]
        input_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        dependencies = []
        for jar in (kotlin / "kotlin-stdlib.jar", kotlin / "annotations-13.0.jar"):
            output = work / jar.name
            with zipfile.ZipFile(jar) as source, zipfile.ZipFile(output, "x") as dest:
                for entry in source.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):
                            raise RuntimeError("Unsafe compiler dependency entry")
                        dest.writestr(entry.filename, source.read(entry))
            dependencies.append(output)
        java, originals, harness = work / "original-java", work / "originals", work / "harness"
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), java, release=8)
        originals.mkdir()
        verify.run([choose_jdk(args.kotlin_java_home) / "bin/java", "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
            "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (kotlin / "kotlin-stdlib.jar", java))),
            "-jvm-target", "1.8", "-d", originals, *sorted((fixture / "kotlin").rglob("*.kt"))], "kotlinc-original")
        for path in sorted(java.rglob("*.class")):
            target = originals / path.relative_to(java); target.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(path, target)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in ("StatelessMethodContractsCaller", "StatelessMethodContractsHarness")],
                             harness, classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        anonymous_path = Path("fixture/Owner$shared$1.class")
        receipts = [change_method_flags(originals / anonymous_path, prototype, 0x1, 0x1041 if args.synthetic else 0x41, name=name)
                    for name, prototype in ((b"echo", b"(Ljava/lang/Class;Ljava/lang/Object;)Ljava/lang/Object;"),
                                            (b"inspect", b"(Lfixture/Carrier;)Lfixture/Carrier;"))]
        (work / "original-bridge-flags.json").write_text(json.dumps(receipts, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Api;"} if args.dependency_root else set())
        methods = {i for i in all_methods if i.split("->", 1)[0] in classes}
        def facts(root):
            return {r["name"]: r for p in sorted(root.rglob("*.class")) for r in [KotlinFixtureClassFile(p.read_bytes()).facts()]}
        records, exceptions = facts(originals), exception_inventory(originals)
        if (records[ANONYMOUS]["signature"] or records[ANONYMOUS]["fields"]
                or records[ANONYMOUS]["superclass"] != "Ljava/lang/Object;"):
            raise RuntimeError("Original stateless scope changed")
        provider = work / "original-interface.jar"
        with zipfile.ZipFile(provider, "x") as jar: jar.writestr("fixture/Api.class", (originals / "fixture/Api.class").read_bytes())
        if args.dependency_root: dependencies.append(provider)
        dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        classpath = ["--classpath", args.android_jar.resolve()]
        for path in dependencies: classpath += ["--classpath", path]
        def execute(runtime, label):
            results = []
            for route in range(5):
                for fixed in ("false", "true"):
                    for mode in range(8):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                            "StatelessMethodContractsHarness", str(route), fixed, str(mode)], f"{label}-{route}-{fixed}-{mode}")
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(
                                f"route={route},fixed={fixed},mode={mode},attempt=[01],trace=[0-9]+", line) for line in output.splitlines()):
                            raise RuntimeError("Independent anonymous method observer changed: " + output)
                        results.append(output)
            return results
        baseline = execute((originals, harness), "baseline")
        def dex(root, destination):
            destination.mkdir()
            paths = [p for p in sorted(root.rglob("*.class")) if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]: raise RuntimeError("Original DEX inventory changed")
            return result
        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original anonymous implementation declarations")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        verify.run([verify.dexdump, "-d", source_dex], "original-code")
        annotations = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        visible = re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations)
        if "VISIBILITY_BUILD" in annotations or visible != ["Lkotlin/Metadata;"] * 2:
            raise RuntimeError("Original compiler annotation retention changed")
        expected = copy.deepcopy(records); dropped = {}
        for owner in classes:
            record = expected[owner]
            for identity, row in [(owner, record), *record["fields"].items(), *record["methods"].items()]:
                for key in ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations"):
                    if row.get(key): dropped.setdefault(identity, {})[key] = row[key]
                    row[key] = []
        (work / "d8-annotation-differences.json").write_text(json.dumps(dropped, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        bridges = {i for i in methods if i.startswith(ANONYMOUS + "->") and "-><init>" not in i}
        allocation_bindings = {"Lfixture/Owner;-><clinit>()V", ANONYMOUS + "-><init>()V"}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("generic_signature_projections")
                or coverage.get("projected_method_count") != 4 or coverage["status"] != "partial"
                or {i for i, row in rows.items() if row["status"] == "source-projected"} != bridges | allocation_bindings
                or any(rows[i].get("projection_kind") != "anonymous-to-named-member" for i in allocation_bindings)):
            raise RuntimeError("Anonymous implementation lost original inventory or complete contracts")
        for identity in bridges:
            row = rows[identity]
            kind = "method-formal-implementation-bridge" if "->echo(" in identity else "full-contract-implementation-bridge"
            if (row.get("projection_kind") != kind or not row.get("inherited_contract")
                    or row.get("bridge_target") or not row.get("metadata_differences")):
                raise RuntimeError("Original implementation lacks its complete contract receipt")
        bindings = coverage.get("class_source_bindings", [])
        if len(bindings) != 1 or bindings[0]["class"] != ANONYMOUS or bindings[0]["binding_kind"] != "anonymous-to-named-member":
            raise RuntimeError("Missing exact stateless source binding")
        binding = bindings[0]; mapped = binding["source_enclosing_class"][:-1] + "$" + binding["source_name"] + ";"
        def mapping(text): return text.replace(ANONYMOUS, mapped)
        rebuilt = work / "rebuilt"
        release = coverage.get("minimum_java_release", 8)
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=release,
                             classpath=os.pathsep.join(map(str, dependencies)))
        actual = facts(rebuilt); rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != {mapping(c) for c in classes} or set(rebuilt_methods) != {mapping(m) for m in methods | helpers}:
            raise RuntimeError("Recompiled anonymous source changed complete runtime inventory")
        actual_exceptions = exception_inventory(rebuilt)
        for owner in classes:
            wanted = copy.deepcopy(expected[owner]); after = copy.deepcopy(actual[mapping(owner)])
            wanted["name"] = mapping(owner); wanted["major"] = 44 + release
            wanted["source_file"] = owner[1:-1].split("$")[0].split("/")[-1] + ".java"
            if owner == ANONYMOUS:
                wanted["inner_class"].update(inner=mapped, outer=binding["source_enclosing_class"], name=binding["source_name"])
                if "EnclosingMethod" not in " ".join(binding["metadata_differences"]): raise RuntimeError("Undisclosed anonymous enclosure difference")
                wanted["enclosing_method"] = None
            wanted["fields"] = {mapping(k): v for k, v in wanted["fields"].items()}
            for identity, metadata in wanted["methods"].items():
                if identity in bridges: metadata["access"] &= ~0x1040
                if actual_exceptions.get(mapping(identity)) != exceptions[identity]: raise RuntimeError("Changed original exception declaration")
            wanted["methods"] = {mapping(k): v for k, v in wanted["methods"].items()}
            for identity in helpers:
                after["methods"].pop(mapping(identity), None)
            if wanted != after:
                (work / "metadata-mismatch.json").write_text(json.dumps({"expected": wanted, "actual": after}, indent=2) + "\n")
                raise RuntimeError("Complete anonymous metadata changed: " + owner)
        (work / "metadata-before.json").write_text(json.dumps(expected, indent=2) + "\n")
        (work / "metadata-after.json").write_text(json.dumps(actual, indent=2) + "\n")
        if execute((rebuilt, harness), "rebuilt") != baseline: raise RuntimeError("Original implementation behavior changed")
        for label, before, changed in (
                ("unused-bound", ECHO, ECHO.replace(b"S:Ljava/lang/Object;", b"S:TR;")),
                ("result-formal", ECHO, ECHO[:-3] + b"TS;"),
                ("fixed-arguments", FIXED, FIXED.replace(b"Ljava/lang/Number;Ljava/lang/String;", b"Ljava/lang/String;Ljava/lang/Number;"))):
            negative = work / label; shutil.copytree(originals, negative)
            receipt = change_signature(negative / anonymous_path, before, changed)
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            changed_dex = dex(negative, work / (label + "-dex")); rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(changed_dex), *map(str, classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr: raise RuntimeError("Accepted changed full contract: " + label)
        authored = work / "Invalid.java"
        authored.write_text("import fixture.*; public abstract class Invalid implements Api { public <R,S extends R> R echo(Class<R> t,R v){return v;} }\n")
        result = subprocess.run([str(verify.javac), "-cp", str(originals), str(authored)], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "invalid-javac.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0: raise RuntimeError("Independent javac accepted changed unused method bound")
        for label, old, new in (("lost-receiver", "receiver = self", "receiver = null"),
                                ("lost-input", "input = value", "input = null"),
                                ("lost-result", "return value;", "return null;")):
            # Compile a wrong first-party provider beside every recovered source;
            # the frozen caller and observer must reject it without rebuilding.
            wrong_source = work / ("wrong-" + label) / "fixture/Trace.java"
            wrong_source.parent.mkdir(parents=True)
            body = (fixture / "java/fixture/Trace.java").read_text()
            if body.count(old) != 2: raise RuntimeError("Missing exact wrong-provider anchor")
            wrong_source.write_text(body.replace(old, new))
            wrong = work / ("wrong-" + label + "-classes")
            verify.compile_local([wrong_source, *[p for p in sorted((output / "sources").rglob("*.java")) if p.name != "Trace.java"]],
                                 wrong, release=release, classpath=os.pathsep.join(map(str, dependencies)))
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, harness, *dependencies))),
                                     "StatelessMethodContractsHarness", "1", "false", "0"], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / ("wrong-" + label + "-observer.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned anonymous method contract" not in result.stderr: raise RuntimeError("Frozen observer accepted wrong body: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != v for p, v in input_hashes.items())
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != v for p, v in dependency_hashes.items())):
            raise RuntimeError("Original bytecode, frozen callers, dependencies or compiler changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "synthetic_flag": args.synthetic,
                    "dependency_root": args.dependency_root, "original_Code": "unchanged", "full_metadata": "checked after disclosed differences",
                    "signature_counterexamples": 3, "wrong_programs": 3, "independent_javac": "rejects bound drift"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n"); print(json.dumps(evidence, sort_keys=True))
    return 0
if __name__ == "__main__": raise SystemExit(main())
