#!/usr/bin/env python3
"""Verify genuine raw Kotlin local covariance and static singleton source placement."""
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
    from scripts.test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import KotlinFixtureClassFile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--kotlin-lib", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--kotlin-java-home", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--class-lambdas", action="store_true")
    parser.add_argument("--input-runtime", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-raw-local-covariance-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "raw_local_covariance"
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
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), original_java, release=8)
        originals.mkdir()
        compiler = [choose_jdk(args.kotlin_java_home) / "bin/java", "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (stdlib, original_java))),
                    "-jvm-target", "1.8", *( ["-Xlambdas=class"] if args.class_lambdas else [])]
        verify.run([*compiler, "-d", originals, *sorted((fixture / "kotlin").rglob("*.kt"))], "kotlinc-original")
        for path in sorted(original_java.rglob("*.class")):
            destination = originals / path.relative_to(original_java)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
        if args.input_runtime:
            # Include exact compiler-distributed classes as DEX input. Their
            # method bodies and metadata are never patched for this profile.
            with zipfile.ZipFile(stdlib) as runtime:
                for name in ("kotlin/Function", "kotlin/jvm/functions/Function1",
                             "kotlin/jvm/internal/FunctionBase", "kotlin/jvm/internal/Lambda"):
                    destination = originals / (name + ".class")
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    with destination.open("xb") as output:
                        output.write(runtime.read(name + ".class"))
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("RawLocalCovarianceHarness", "RawLocalCovarianceCaller")], harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=17)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}

        def facts(root):
            return {row["name"]: row for path in sorted(root.rglob("*.class"))
                    if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes
                    for row in [KotlinFixtureClassFile(path.read_bytes()).facts()]}

        records, exceptions = facts(originals), exception_inventory(originals)
        lambda_name = next((owner for owner in classes if owner.startswith("Lfixture/Owner$")), None)
        if (len(all_classes) != (7 if args.input_runtime else 3)
                or len(all_methods) != (14 if args.input_runtime else 9) or lambda_name is None
                or records[lambda_name]["signature"] is not None):
            raise RuntimeError("Kotlin changed original stateless raw local covariance")
        bridge_id = lambda_name + "->invoke(Ljava/lang/Object;)Ljava/lang/Object;"
        target_id = lambda_name + "->invoke(Ljava/lang/Object;)Ljava/lang/Void;"
        mask_id = "Lfixture/Owner;-><init>(Lkotlin/jvm/functions/Function1;ILkotlin/jvm/internal/DefaultConstructorMarker;)V"
        if (records[lambda_name]["methods"][target_id]["signature"] != "(TK;)Ljava/lang/Void;"
                or records[lambda_name]["methods"][bridge_id]["access"] != 0x1041
                or records["Lfixture/Owner;"]["methods"][mask_id]["signature"] is not None):
            raise RuntimeError("Kotlin changed exact raw bridge or mask constructor metadata")
        dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        classpath = ["--classpath", args.android_jar.resolve()]
        for path in dependencies:
            classpath += ["--classpath", path]

        def execute(runtime, label):
            outputs = []
            for route in range(4):
                for mode in range(8):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                        "RawLocalCovarianceHarness", str(route), str(mode), str(args.class_lambdas).lower()],
                        f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Original raw local covariance observer changed")
                    outputs.append(output)
            return outputs
        baseline = execute((originals, harness), "baseline")

        def dex(root, destination, included=classes, extra=()):
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in included]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, *extra,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned raw local covariance changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original raw local runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "sdk-constructor-code")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            descriptor = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if block[1] == lambda_name and name and name[1] == "invoke" and descriptor and descriptor[1] == "(Ljava/lang/Object;)Ljava/lang/Object;":
                ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
                if len(ops) != 3 or not ops[0][0].startswith("invoke-virtual") or ops[1][0] != "move-result-object" or ops[2][0] != "return-object":
                    raise RuntimeError("D8 changed original exact covariant bridge body")
                witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original raw local bridge witness")
        annotations_dump = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        visible = re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations_dump)
        if "VISIBILITY_BUILD" in annotations_dump or visible != ["Lkotlin/Metadata;"] * (6 if args.input_runtime else 2):
            raise RuntimeError("D8's independently read annotation retention changed")
        # Compare the DEX contract only after independently confirming every
        # CLASS-retained annotation was removed by D8. Keep their full original
        # payloads as evidence; all runtime compiler annotations stay exact.
        dex_records = copy.deepcopy(records)
        dropped = {}
        for owner in classes:
            record = dex_records[owner]
            for identity, row in [(owner, record), *record["fields"].items(), *record["methods"].items()]:
                for key in ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations"):
                    if row.get(key):
                        dropped.setdefault(identity, {})[key] = row[key]
                    row[key] = []
        not_null = {"type": "Lorg/jetbrains/annotations/NotNull;", "elements": {}}
        expected_dropped = {
            "Lfixture/Owner;->callback:Lkotlin/jvm/functions/Function1;": {
                "runtime_invisible_annotations": [
                    {"type": "Lkotlin/jvm/JvmField;", "elements": {}}, not_null]},
            "Lfixture/Owner;-><init>(Lkotlin/jvm/functions/Function1;)V": {
                "runtime_invisible_parameter_annotations": [[not_null]]},
        }
        if args.input_runtime:
            expected_dropped["Lkotlin/jvm/internal/Lambda;->toString()Ljava/lang/String;"] = {
                "runtime_invisible_annotations": [not_null]}
        if dropped != expected_dropped:
            raise RuntimeError("Exact CLASS annotation loss sites or marker payloads changed")
        (work / "d8-annotation-differences.json").write_text(json.dumps(dropped, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        expected_projected = {identity for identity in methods if identity.startswith(lambda_name + "->")} | {mask_id}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        bindings = coverage.get("class_source_bindings", [])
        projections = coverage.get("generic_signature_projections", [])
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or projected != expected_projected
                or coverage["projected_method_count"] != len(expected_projected) or coverage["minimum_java_release"] != 25
                or len(bindings) != 1 or bindings[0]["class"] != lambda_name
                or bindings[0]["binding_kind"] != "anonymous-to-shared-member"
                or len(projections) != 1 or projections[0]["declaration"] != target_id
                or projections[0]["original_signature"] != "(TK;)Ljava/lang/Void;"
                or projections[0]["changes"] != [{
                    "kind": "raw-local-covariant-parameter-erasure", "offset": 1,
                    "original_type": "TK;", "source_type": "Ljava/lang/Object;",
                    "scope_origin": "Lkotlin/jvm/functions/Function1;->invoke(Ljava/lang/Object;)Ljava/lang/Object;"}]):
            raise RuntimeError("Raw local source projection lacks its exact class/method receipts")
        if rows[bridge_id].get("bridge_target") != target_id or rows[bridge_id].get("projection_kind") != "generic-interface-bridge":
            raise RuntimeError("Raw local bridge lost exact target mapping")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=os.pathsep.join(map(str, dependencies)))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        def all_facts(root):
            return {r["name"]: r for path in root.rglob("*.class") for r in [KotlinFixtureClassFile(path.read_bytes()).facts()]}
        after, after_exceptions = all_facts(rebuilt), exception_inventory(rebuilt)
        binding = bindings[0]
        candidates = [owner for owner, row in after.items() if (row.get("inner_class") or {}).get("name") == binding["source_name"]
                      and (row.get("inner_class") or {}).get("outer") == "Lfixture/Owner;"]
        if len(candidates) != 1:
            raise RuntimeError("Source binding does not identify one static member")
        renamed = candidates[0]
        def mapped(text): return text.replace(lambda_name, renamed)
        helpers = {mapped(r["class"] + "->" + r["name"] + r["prototype"]) for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {mapped(owner) + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if any(row["code"] for row in records[owner]["methods"].values())}
        if actual_classes != {mapped(owner) for owner in classes} or set(actual_methods) != {mapped(identity) for identity in methods} | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Raw local covariance invented or lost an original runtime entry")
        for owner in classes:
            before, current = dex_records[owner], after[mapped(owner)]
            for key in ("access", "signature", "superclass", "interfaces", "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != current.get(key):
                    raise RuntimeError("Original raw local class metadata changed: " + owner + ":" + key)
            wanted_inner, wanted_enclosing = copy.deepcopy(before["inner_class"]), before["enclosing_method"]
            if owner == lambda_name:
                wanted_inner = {"inner": renamed, "outer": "Lfixture/Owner;", "name": binding["source_name"], "access": 0x18}
                wanted_enclosing = None
            if current["inner_class"] != wanted_inner or current["enclosing_method"] != wanted_enclosing:
                raise RuntimeError("Raw local class identity changes exceed the shared member receipt")
            fields = {mapped(identity): dict(metadata) for identity, metadata in before["fields"].items()}
            for metadata in fields.values(): metadata["descriptor"] = mapped(metadata["descriptor"])
            if current["fields"] != fields:
                raise RuntimeError("Original raw local fields changed beyond declared identity")
            for identity, metadata in before["methods"].items():
                wanted = copy.deepcopy(metadata)
                wanted["prototype"] = mapped(wanted["prototype"])
                if identity == mask_id: wanted["access"] &= ~0x1000
                if identity == target_id: wanted["signature"] = None
                if current["methods"].get(mapped(identity)) != wanted or after_exceptions[mapped(identity)] != exceptions[identity]:
                    raise RuntimeError("Raw local method metadata changed beyond declared projections: " + identity)
        if execute((rebuilt, harness), "rebuilt") != baseline:
            raise RuntimeError("Raw local callback values, singleton, arity or failures changed")
        text = (fixture / "kotlin/fixture/Owner.kt").read_text()
        mutations = (
            ("wrong-result", text.replace("Trace.seen = it; null", "Trace.seen = it; it"), "0"),
            ("lost-effect", text.replace("Trace.hit(3);", ""), "0"),
            ("wrong-identity", text.replace("Trace.seen = it;", "Trace.seen = Any();"), "0"),
        )
        for label, source, mode in mutations:
            if source == text: raise RuntimeError("Raw local mutation anchor changed")
            changed = work / label / "Owner.kt"; changed.parent.mkdir(); changed.write_text(source)
            wrong = work / (label + "-classes"); wrong.mkdir()
            verify.run([*compiler, "-d", wrong, changed], "kotlinc-" + label)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness, *dependencies))),
                                     "RawLocalCovarianceHarness", "1", mode, str(args.class_lambdas).lower()], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "raw local covariance contract" not in result.stderr:
                raise RuntimeError("Raw local observer missed " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != value for p, value in input_hashes.items())
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != value for p, value in dependency_hashes.items())):
            raise RuntimeError("Original compiler, SDK, inputs or immutable callers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "class_lambdas": args.class_lambdas,
                    "input_runtime": args.input_runtime,
                    "compiler_generated_raw_mask_constructor": "unchanged input",
                    "mask_selected_same_owner_target_and_full_contract": "matched",
                    "default_identity_lazy_initialization_partial_fields_and_exception_identity": "matched",
                    "original_metadata_and_exceptions": "unchanged except disclosed identity, synthetic flag and parameter erasure",
                    "d8_class_retained_annotation_differences": "independently verified and recorded",
                    "independent_compiler_counterexamples": len(mutations),
                    "original_inputs_and_oracles": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
