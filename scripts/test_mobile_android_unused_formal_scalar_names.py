#!/usr/bin/env python3
"""Verify original Kotlin static integer slots with independent unused formals."""
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_synthetic_method_names import compiler_facts
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import compiler_facts
    from test_mobile_android_nested_return import change_signature


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--kotlin-lib", type=Path, required=True)
    parser.add_argument("--kotlin-java-home", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-unused-formal-scalar-names-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "unused_formal_scalar_names"
        original_java, originals, harness = work / "original-java", work / "originals", work / "harness"
        kotlin = args.kotlin_lib.resolve()
        compiler_jars = sorted(kotlin.glob("*.jar"))
        if not (kotlin / "kotlin-compiler.jar").is_file() or not (kotlin / "kotlin-stdlib.jar").is_file():
            raise RuntimeError("The witness requires the unmodified Kotlin compiler distribution")
        inputs = [args.android_jar.resolve(), *compiler_jars]
        input_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        stdlib = kotlin / "kotlin-stdlib.jar"
        dependencies = []
        for jar in (stdlib, kotlin / "annotations-13.0.jar"):
            output = work / jar.name
            with zipfile.ZipFile(jar) as source, zipfile.ZipFile(output, "x") as dest:
                for entry in source.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):
                            raise RuntimeError("Unsafe compiler dependency entry")
                        dest.writestr(entry.filename, source.read(entry))
            dependencies.append(output)
        java_fixture = fixture / "java/fixture"
        verify.compile_local([java_fixture / "Provider.java", java_fixture / "Trace.java"], original_java, release=8)
        compiler = [choose_jdk(args.kotlin_java_home) / "bin/java", "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (stdlib, original_java))),
                    "-jvm-target", "1.8"]
        originals.mkdir()
        verify.run([*compiler, "-d", originals, *sorted((fixture / "kotlin").rglob("*.kt"))], "kotlinc-original")
        for path in sorted(original_java.rglob("*.class")):
            destination = originals / path.relative_to(original_java)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("UnusedFormalScalarNameCaller", "UnusedFormalScalarNameHarness")], harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != 10:
            raise RuntimeError("Original static name fixture inventory changed")
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        classes = all_classes - ({"Lfixture/Provider;"} if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        renamed = {identity: row for owner, facts in records.items() if owner in classes
                   for identity, row in facts["methods"].items() if "-" in row["name"]}
        original_signature = b"<T:Ljava/lang/Object;>(I)I"
        if (len(renamed) != 1 or any(row["access"] != 0x19 or row["signature"] != original_signature.decode()
                                     for row in renamed.values())):
            raise RuntimeError("Original compiler did not produce the nonsynthetic unused-formal witness")
        if args.dependency_provider:
            provider = work / "owned-provider.jar"
            with zipfile.ZipFile(provider, "x") as jar:
                jar.writestr("fixture/Provider.class", (originals / "fixture/Provider.class").read_bytes())
            dependencies.append(provider)
        dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        classpath = ["--classpath", args.android_jar.resolve()]
        for path in dependencies:
            classpath += ["--classpath", path]

        def execute(runtime, name, label):
            outputs = []
            for route in range(4):
                for mode in range(10):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                        "UnusedFormalScalarNameHarness", str(route), str(mode), name], f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Original static name behavior matrix changed: " + output)
                    outputs.append(output)
            return outputs

        original_name = next(iter(renamed.values()))["name"]
        baseline = execute((originals, harness), original_name, "baseline")

        def dex(root, destination):
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", destination, *paths], "d8-" + destination.name)
            if sorted(destination.glob("classes*.dex")) != [destination / "classes.dex"]:
                raise RuntimeError("Owned static name fixture changed DEX inventory")
            return destination / "classes.dex"

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original static name runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        annotations_dump = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        visible = re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations_dump)
        if ("VISIBILITY_BUILD" in annotations_dump or visible.count("Lkotlin/Metadata;") != 1
                or visible.count("Lkotlin/jvm/JvmStatic;") != 3 or len(visible) != 4):
            raise RuntimeError("D8's independently read annotation retention changed")
        dex_records, dropped = copy.deepcopy(records), {}
        for owner in classes:
            facts = dex_records[owner]
            for identity, row in [(owner, facts), *facts["fields"].items(), *facts["methods"].items()]:
                for key in ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations"):
                    if row.get(key):
                        dropped.setdefault(identity, {})[key] = row[key]
                    row[key] = []
        expected_dropped = {"Lfixture/Entry;->INSTANCE:Lfixture/Entry;",
                            "Lfixture/Entry;->carry-value(I)I"}
        if set(dropped) != expected_dropped:
            raise RuntimeError("Original Kotlin CLASS-retained annotation witness changed: " + repr(sorted(dropped)))
        (work / "d8-annotation-differences.json").write_text(json.dumps(dropped, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["projected_method_count"] != 1
                or projected != set(renamed) or coverage["status"] != "partial"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Static name recovery changed original inventory or unrelated contracts")
        mappings = {}
        for identity in projected:
            row, original = rows[identity], renamed[identity]
            target = identity.split("->", 1)[0] + "->" + row["source_name"] + original["prototype"]
            if (row.get("projection_kind") != "static-method-identifier" or row.get("source_identity") != target
                    or row.get("source_name_declaration") != identity or row.get("conflicting_inherited_contracts") != []
                    or len(row.get("metadata_differences", [])) != 3 or target in methods
                    or "synthetic" in " ".join(row["metadata_differences"])
                    or row["source_name"] == "__neverdStaticMethod0"):
                raise RuntimeError("Static method lacks its exact source name and flag-preserving receipt")
            mappings[identity] = target
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=os.pathsep.join(map(str, dependencies)))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if (helpers != expected_helpers or actual_classes != classes
                or set(actual_methods) != {mappings.get(m, m) for m in methods} | helpers):
            raise RuntimeError("Source compiler invented or omitted a runtime entry")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            original, actual = dex_records[owner], after[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if original.get(key) != actual.get(key):
                    raise RuntimeError("Original class/field metadata changed: " + owner + ":" + key)
            for identity, metadata in original["methods"].items():
                target, wanted = mappings.get(identity, identity), copy.deepcopy(metadata)
                if identity in mappings:
                    wanted["name"] = rows[identity]["source_name"]
                if actual["methods"].get(target) != wanted or after_exceptions.get(target) != exceptions[identity]:
                    raise RuntimeError("Original method metadata changed beyond the disclosed name: " + identity)
        source_name = rows[next(iter(renamed))]["source_name"]
        if execute((rebuilt, harness), source_name, "rebuilt") != baseline:
            raise RuntimeError("Static source name changed effects, initialization or failures")
        for label, signature in (
                ("nonobject-bound", original_signature.replace(b"T:Ljava/lang/Object;>", b"T:Ljava/lang/Number;>")),
                ("used-formal", original_signature.replace(b"(I)", b"(TT;)")),
                ("extra-formal", original_signature.replace(b"T:Ljava/lang/Object;>", b"T:Ljava/lang/Object;U:Ljava/lang/Object;>"))):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Entry.class", original_signature, signature)
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if (result.returncode == 0 or rejected.exists()
                    or not any(message in result.stderr for message in ("identifier cannot be represented", "generic invocation", "erasure"))):
                raise RuntimeError("Changed static name contract was not rejected: " + label)
        original_source = (fixture / "kotlin/fixture/Entry.kt").read_text()
        marker = "val result = Provider.relay<T>(value)"
        if original_source.count(marker) != 1:
            raise RuntimeError("Owned static call witness changed")
        for label, replacement in (("bypassed-call", "val result = value"),
                                   ("repeated-call", "Provider.relay<T>(value)\n        " + marker)):
            changed_source = work / label / "Entry.kt"
            changed_source.parent.mkdir()
            changed_source.write_text(original_source.replace(marker, replacement))
            wrong = work / (label + "-classes")
            wrong.mkdir()
            verify.run([*compiler, "-d", wrong, changed_source], "kotlinc-" + label)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness, *dependencies))),
                                     "UnusedFormalScalarNameHarness", "1", "0", original_name], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "unused scalar formal name changed" not in result.stderr:
                raise RuntimeError("Independent observer did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs} != input_hashes
                or {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies} != dependency_hashes):
            raise RuntimeError("Original compiler, input, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "projected_methods": len(projected),
                    "dependency_provider": args.dependency_provider, "method_name": "difference reported",
                    "access_signatures_fields_runtime_annotations_and_exceptions": "unchanged",
                    "d8_class_retained_annotation_differences": "independently verified and recorded",
                    "same_code_counterexamples": "rejected", "independent_bypass_and_repeated_calls": "detected",
                    "initialization_order_exception_and_scalar_snapshot": "matched", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
