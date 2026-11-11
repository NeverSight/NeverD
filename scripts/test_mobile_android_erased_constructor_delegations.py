#!/usr/bin/env python3
"""Verify genuine Kotlin erased access constructors preserve private targets."""
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
    parser.add_argument("--unary-owner", action="store_true")
    parser.add_argument("--nominal-owner", action="store_true")
    args = parser.parse_args()
    if args.nominal_owner:
        args.unary_owner = True
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-erased-constructor-delegation-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "erased_constructor_delegations"
        kotlin_sources = fixture / ("kotlin_nominal" if args.nominal_owner else "kotlin_unary" if args.unary_owner else "kotlin")
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
        java_sources = sorted((fixture / "java").rglob("*.java"))
        if args.nominal_owner:
            java_sources += sorted((fixture / "java_nominal").rglob("*.java"))
        verify.compile_local(java_sources, original_java, release=8)
        originals.mkdir()
        compiler = [choose_jdk(args.kotlin_java_home) / "bin/java", "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (stdlib, original_java))),
                    "-jvm-target", "1.8"]
        verify.run([*compiler, "-d", originals, *sorted(kotlin_sources.rglob("*.kt"))], "kotlinc-original")
        for path in sorted(original_java.rglob("*.class")):
            destination = originals / path.relative_to(original_java)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("ErasedConstructorDelegationHarness", "ErasedConstructorDelegationCaller")], harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}

        def facts(root):
            return {row["name"]: row for path in sorted(root.rglob("*.class"))
                    if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes
                    for row in [KotlinFixtureClassFile(path.read_bytes()).facts()]}

        records, exceptions = facts(originals), exception_inventory(originals)
        synthetic = {identity: row for owner, record in records.items()
                     for identity, row in record["methods"].items() if row["access"] & 0x1000}
        if (len(classes) != (5 if args.nominal_owner else 4 if args.unary_owner else 3) or len(methods) != (12 if args.nominal_owner else 11) or len(synthetic) != 1
                or any(row["name"] != "<init>" or row["signature"] is not None for row in synthetic.values())):
            raise RuntimeError("Kotlin did not produce the original raw synthetic access constructor")
        dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        classpath = ["--classpath", args.android_jar.resolve()]
        for path in dependencies:
            classpath += ["--classpath", path]

        def execute(runtime, synthetic_flag, label):
            outputs = []
            for route in range(5):
                for mode in range(8):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                        "ErasedConstructorDelegationHarness", str(route), str(mode), str(synthetic_flag).lower()],
                        f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Original access constructor behavior matrix changed: " + output)
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), True, "baseline")

        def dex(root, destination, included=classes, extra=()):
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in included]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, *extra,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned mangled interface changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original mangled interface runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "sdk-constructor-code")
        wrapper = next(iter(synthetic.values()))["prototype"]
        target = next(row["prototype"] for record in records.values() for row in record["methods"].values()
                      if row["name"] == "<init>" and row["access"] & 0x0002)
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            descriptor = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if block[1] == "Lfixture/Core;" and name and name[1] == "<init>" and descriptor and descriptor[1] == wrapper:
                ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
                if (len(ops) != 2 or not ops[0][0].startswith("invoke-direct") or ops[1][0] != "return-void"
                        or "Lfixture/Core;.<init>:" + target not in ops[0][1]):
                    raise RuntimeError("D8 changed the original private-constructor forwarding body")
                witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original raw constructor forwarding witness")
        annotations_dump = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        visible = re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations_dump)
        if "VISIBILITY_BUILD" in annotations_dump or visible != ["Lkotlin/Metadata;"] * (3 if args.unary_owner else 2):
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
        if not dropped:
            raise RuntimeError("Original Kotlin CLASS-retained annotation witness changed")
        (work / "d8-annotation-differences.json").write_text(json.dumps(dropped, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["projected_method_count"] != 1
                or projected != set(synthetic) or coverage["status"] != "partial"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Access constructor recovery changed original inventory or unrelated contracts")
        for identity in projected:
            row = rows[identity]
            if (row.get("projection_kind") != "synthetic-constructor" or row.get("source_identity") != identity
                    or len(row.get("metadata_differences", [])) != 1):
                raise RuntimeError("Original access constructor lacks its disclosed flag difference")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=os.pathsep.join(map(str, dependencies)))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if owner != "Lfixture/View;"}
        if (helpers != expected_helpers or actual_classes != classes
                or set(actual_methods) != methods | helpers):
            raise RuntimeError("Source compiler invented or omitted a runtime entry")
        after, after_exceptions = facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            original, actual = dex_records[owner], after[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if original.get(key) != actual.get(key):
                    raise RuntimeError("Original class/field metadata changed: " + owner + ":" + key)
            for identity, metadata in original["methods"].items():
                target, wanted = identity, copy.deepcopy(metadata)
                if identity in synthetic:
                    wanted["access"] &= ~0x1000
                if actual["methods"].get(target) != wanted or after_exceptions.get(target) != exceptions[identity]:
                    raise RuntimeError("Original method metadata changed beyond the disclosed name: " + identity)
        if execute((rebuilt, harness), False, "rebuilt") != baseline:
            raise RuntimeError("Erased constructor changed private target effects, aliases or failures")
        # Recompile an independent body that calls the callback twice. The
        # original immutable observer must detect the extra call and trace.
        changed = work / "wrong-callback/Core.kt"
        changed.parent.mkdir()
        text = (kotlin_sources / "fixture/Core.kt").read_text()
        if text.count("answer = callback(this)") != 1:
            raise RuntimeError("Original constructor callback witness changed")
        changed.write_text(text.replace("answer = callback(this)", "callback(this)\n        answer = callback(this)"))
        wrong = work / "wrong-callback-classes"
        wrong.mkdir()
        verify.run([*compiler, "-d", wrong, changed, *([kotlin_sources / "fixture/View.kt"] if args.unary_owner else [])], "kotlinc-wrong-callback")
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness, *dependencies))),
                                 "ErasedConstructorDelegationHarness", "1", "0", "true"], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-callback-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "erased constructor delegation changed" not in result.stderr:
            raise RuntimeError("Independent observer did not detect repeated constructor callback")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != value for p, value in input_hashes.items())
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != value for p, value in dependency_hashes.items())):
            raise RuntimeError("Original compiler, SDK, inputs or immutable callers changed")
        evidence = {"status": "success", "unary_owner": args.unary_owner, "nominal_owner": args.nominal_owner, "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "compiler_generated_access_constructor": "unchanged input",
                    "private_constructor_dispatch_and_full_contract": "matched",
                    "nulls_callback_casts_wide_bits_and_exception_identity": "matched",
                    "original_metadata_and_exceptions": "unchanged except disclosed synthetic flag",
                    "d8_class_retained_annotation_differences": "independently verified and recorded",
                    "independent_repeated_callback_counterexample": "detected",
                    "original_inputs_and_oracles": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
