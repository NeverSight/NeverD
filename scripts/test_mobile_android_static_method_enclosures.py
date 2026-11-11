#!/usr/bin/env python3
"""Verify genuine Kotlin default helper names and their owned anonymous EnclosingMethod."""
from __future__ import annotations

import argparse
from collections import Counter
from contextlib import nullcontext
from copy import deepcopy
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import compiler_facts


OWNER = "Lfixture/Provider;"
HELPER_NAME = "read-value$default"
HELPER = OWNER + "->" + HELPER_NAME + "(Ljava/lang/Object;Ljava/lang/Object;ILjava/lang/Object;)Ljava/lang/Object;"
NESTED = "Lfixture/Provider$read$1;"
CONSTRUCTOR = NESTED + "-><init>()V"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "kotlin-lib", "kotlin-java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-static-enclosures-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "static_method_enclosures"
        kotlin = args.kotlin_lib.resolve()
        compiler_jars = sorted(kotlin.glob("*.jar"))
        if not (kotlin / "kotlin-compiler.jar").is_file() or not (kotlin / "kotlin-stdlib.jar").is_file():
            raise RuntimeError("The original witness requires the unmodified Kotlin compiler distribution")
        inputs = [args.android_jar.resolve(), *compiler_jars]
        input_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        stdlib = kotlin / "kotlin-stdlib.jar"
        dependencies = []
        for source in (stdlib, kotlin / "annotations-13.0.jar"):
            dest = work / source.name
            with zipfile.ZipFile(source) as before, zipfile.ZipFile(dest, "x") as after:
                for entry in before.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):
                            raise RuntimeError("Unsafe original compiler dependency entry")
                        after.writestr(entry.filename, before.read(entry))
            dependencies.append(dest)
        dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        original_java, originals, harness = work / "original-java", work / "originals", work / "harness"
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), original_java, release=8)
        originals.mkdir()
        compiler = [choose_jdk(args.kotlin_java_home) / "bin/java", "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (stdlib, original_java))), "-jvm-target", "1.8"]
        verify.run([*compiler, "-d", originals, *sorted((fixture / "kotlin").rglob("*.kt"))], "kotlinc-original")
        for source in sorted(original_java.rglob("*.class")):
            dest = originals / source.relative_to(original_java)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, dest)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("StaticMethodEnclosureHarness", "StaticMethodEnclosureCaller")], harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        classes, methods = compiler_inventory(verify, originals)
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        if len(classes) != 3 or len(methods) != 10 or NESTED not in classes or HELPER not in methods:
            raise RuntimeError("Original static enclosure inventory changed: " + repr((len(classes), len(methods))))
        mangled = {identity for identity in methods if "->read-value" in identity}
        if (len(mangled) != 2 or records[NESTED]["enclosing_method"] != {
                "owner": OWNER, "name": HELPER_NAME,
                "prototype": "(Ljava/lang/Object;Ljava/lang/Object;ILjava/lang/Object;)Ljava/lang/Object;"}):
            raise RuntimeError("Compiler did not provide the exact synthetic default-method enclosure")

        def execute(runtime, name, metadata, label):
            outputs = []
            for route in range(4):
                for variant in range(2):
                    for mode in range(8):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                            "StaticMethodEnclosureHarness", str(route), str(variant), str(mode), name, metadata], f"{label}-{route}-{variant}-{mode}")
                        expected = f"route={route},variant={variant},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Original static enclosure matrix changed: " + output)
                        outputs.append(output)
            return outputs

        baseline = execute((originals, harness), HELPER_NAME, "input", "baseline")
        classpath = ["--classpath", args.android_jar.resolve()]
        for path in dependencies:
            classpath += ["--classpath", path]
        dex_dir = work / "dex"
        dex_dir.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex_dir,
                    *sorted(originals.rglob("*.class"))], "d8-original")
        source_dex = dex_dir / "classes.dex"
        if list(dex_dir.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned static enclosure changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original static enclosure runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        annotations = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        visible = Counter(re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations))
        if "VISIBILITY_BUILD" in annotations or visible != Counter({"Lkotlin/Metadata;": 2, "Lkotlin/jvm/JvmStatic;": 3}):
            raise RuntimeError("D8 changed independently read compiler annotation retention")
        dex_records, dropped = deepcopy(records), {}
        for c in classes:
            facts = dex_records[c]
            for identity, row in [(c, facts), *facts["fields"].items(), *facts["methods"].items()]:
                for key in ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations"):
                    if row.get(key):
                        dropped.setdefault(identity, {})[key] = row[key]
                    row[key] = []
        expected_drops = {OWNER + "->INSTANCE:Lfixture/Provider;", OWNER + "->read-value(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
                          OWNER + "->relay(Ljava/lang/Object;)Ljava/lang/Object;", OWNER + "->provided(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"}
        if set(dropped) != expected_drops:
            raise RuntimeError("Original compiler CLASS annotation loss changed its exact sites")
        (work / "d8-class-annotation-loss.json").write_text(json.dumps(dropped, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial"
                or projected != mangled | {CONSTRUCTOR} or coverage["projected_method_count"] != 3
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Static enclosure recovery changed original inventory or unrelated contracts")
        for identity in mangled:
            if rows[identity].get("projection_kind") != "synthetic-static-method-identifier" or not rows[identity].get("metadata_differences"):
                raise RuntimeError("Static method source identity lacks its original name/flag disclosure")
        bindings = coverage.get("class_source_bindings", [])
        if (len(bindings) != 1 or bindings[0]["class"] != NESTED or bindings[0]["binding_kind"] != "anonymous-to-named-local"
                or bindings[0]["enclosing_method"]["identity"] != HELPER
                or bindings[0].get("source_enclosing_method", {}).get("name") != rows[HELPER]["source_name"]
                or not any("InnerClass flags" in value for value in bindings[0]["metadata_differences"])):
            raise RuntimeError("Missing exact original/source default helper enclosure binding")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=os.pathsep.join(map(str, dependencies)), release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        candidates = [c for c in actual_classes if c.endswith(bindings[0]["source_name"] + ";")]
        if len(candidates) != 1:
            raise RuntimeError("Recompiled local class name is ambiguous")
        replacement = candidates[0]
        mappings = {identity: rows[identity]["source_identity"] for identity in mangled}

        def mapped(identity):
            return mappings.get(identity, identity).replace(NESTED, replacement)

        helpers = {mapped(row["class"] + "->" + row["name"] + row["prototype"]) for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {mapped(c) + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if (actual_classes != {mapped(c) for c in classes} or set(actual_methods) != {mapped(m) for m in methods} | expected_helpers
                or helpers != expected_helpers):
            raise RuntimeError("Static enclosure recovery invented or omitted a runtime entry")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for c in classes:
            before, actual = dex_records[c], after[mapped(c)]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                expected = deepcopy(before.get(key))
                if c == NESTED:
                    if key == "access":
                        expected &= ~1
                    elif key == "inner_class":
                        expected.update(inner=mapped(NESTED), name=bindings[0]["source_name"], access=0x10)
                    elif key == "enclosing_method":
                        expected["name"] = rows[HELPER]["source_name"]
                if expected != actual.get(key):
                    raise RuntimeError("Undisclosed static enclosure class metadata changed: " + c + ":" + key)
            for identity, metadata in before["methods"].items():
                selected, expected = mapped(identity), deepcopy(metadata)
                if identity in mappings:
                    expected["name"] = rows[identity]["source_name"]
                    expected["access"] &= ~0x1000
                if actual["methods"].get(selected) != expected or after_exceptions.get(selected) != exceptions[identity]:
                    raise RuntimeError("Undisclosed static enclosure method metadata or Exceptions changed: " + identity)
        if execute((rebuilt, harness), rows[HELPER]["source_name"], "source", "rebuilt") != baseline:
            raise RuntimeError("Static default helper changed aliases, initialization, effects, failures or enclosure reflection")
        text = (fixture / "kotlin/fixture/Provider.kt").read_text()
        mutations = (("repeated-callback", "Trace.event(3)", "Trace.apply(value)\n        Trace.event(3)"),
                     ("discarded-result", "Trace.result = result", "Trace.result = null"),
                     ("wrong-enclosure", "marker: Any? = object {}", "marker: Any? = Any()"))
        for label, old, new in mutations:
            if text.count(old) != 1:
                raise RuntimeError("Independent static enclosure counterexample anchor changed")
            source = work / label / "Provider.kt"
            source.parent.mkdir()
            source.write_text(text.replace(old, new))
            compiled = work / (label + "-classes")
            compiled.mkdir()
            verify.run([*compiler, "-d", compiled, source], "kotlinc-" + label)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness, *dependencies))),
                                    "StaticMethodEnclosureHarness", "1", "0", "0", HELPER_NAME, "input"], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned static enclosure contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies} != dependency_hashes
                or {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs} != input_hashes):
            raise RuntimeError("Original inputs, immutable callers or compiler bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases_per_stage": len(baseline), "stages": 2, "attempts_per_case": 2,
                    "genuine_kotlin_default_helper_enclosure": "matched", "independent_compiler_counterexamples": len(mutations),
                    "mutable_aliases_initialization_and_failures": "matched", "source_name_flags_and_local_identity_changes": "disclosed",
                    "other_metadata_and_exceptions": "unchanged", "compiler_CLASS_annotation_loss_sites": len(dropped),
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
