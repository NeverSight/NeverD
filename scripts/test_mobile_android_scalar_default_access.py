#!/usr/bin/env python3
"""Verify genuine scalar compatibility accessors keep boxed default dispatch."""
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
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_interface_default_access import compiler_facts
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_interface_default_access import compiler_facts

SCALARS = {
    "D": ("double", "Double", "Double", "doubleValue", "0.0,-0.0,Double.longBitsToDouble(0x7ff8000000000031L),Double.longBitsToDouble(0xfff8000000000042L),Double.POSITIVE_INFINITY,Double.NEGATIVE_INFINITY,Double.MIN_VALUE,-Double.MAX_VALUE"),
    "F": ("float", "Float", "Float", "floatValue", "0.0f,-0.0f,Float.intBitsToFloat(0x7fc00031),Float.intBitsToFloat(0xffc00042),Float.POSITIVE_INFINITY,Float.NEGATIVE_INFINITY,Float.MIN_VALUE,-Float.MAX_VALUE"),
    "J": ("long", "Long", "Long", "longValue", "0L,-1L,Long.MIN_VALUE,Long.MAX_VALUE,0x0123456789abcdefL,-0x0123456789abcdefL,0x0000000100000000L,-0x0000000100000000L"),
    "I": ("int", "Int", "Integer", "intValue", "0,-1,Integer.MIN_VALUE,Integer.MAX_VALUE,0x12345678,0x80000001,0x00010000,-34567"),
    "S": ("short", "Short", "Short", "shortValue", "0,-1,Short.MIN_VALUE,Short.MAX_VALUE,12345,-23456,256,-257"),
    "B": ("byte", "Byte", "Byte", "byteValue", "0,-1,Byte.MIN_VALUE,Byte.MAX_VALUE,63,-64,1,5"),
    "C": ("char", "Char", "Character", "charValue", "(char)0,Character.MAX_VALUE,(char)0x8000,(char)0x1234,(char)1,(char)255,(char)256,(char)0xfffe"),
    "Z": ("boolean", "Boolean", "Boolean", "booleanValue", "false,true,false,true,false,true,false,true"),
}
OWNER = "Lfixture/ScalarApi;"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "kotlin-lib", "kotlin-java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--scalar", choices=SCALARS, default="D")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-scalar-default-access-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        java, kotlin_type, boxed, unbox, samples = SCALARS[args.scalar]
        wrapper = "Ljava/lang/" + boxed + ";"
        target = OWNER + "->getValue()" + wrapper
        accessor = OWNER + "->access$getValue$jd(" + OWNER + ")" + args.scalar
        fixture = FIXTURES / "scalar_default_access/templates"
        sources, observers = work / "original-sources", work / "observer-sources"
        sources.mkdir(); observers.mkdir()
        replacements = {"@JAVA@": java, "@KOTLIN@": kotlin_type, "@BOXED@": boxed, "@VALUES@": samples}
        for source in sorted(fixture.iterdir()):
            text = source.read_text()
            for before, after in replacements.items():
                text = text.replace(before, after)
            if re.search(r"@[A-Z]+@", text):
                raise RuntimeError("Unresolved owned scalar source template")
            dest = (sources if source.name in ("Trace.java.in", "ScalarApi.kt.in") else observers) / source.name[:-3]
            dest.open("x").write(text)
        authored_hashes = {"originals": hashes(sources), "observers": hashes(observers)}
        kotlin = args.kotlin_lib.resolve()
        inputs = [args.android_jar.resolve(), *sorted(kotlin.glob("*.jar"))]
        if not (kotlin / "kotlin-compiler.jar").is_file():
            raise RuntimeError("Original Kotlin compiler unavailable")
        input_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        dependencies = []
        for source in (kotlin / "kotlin-stdlib.jar", kotlin / "annotations-13.0.jar"):
            dest = work / source.name
            with zipfile.ZipFile(source) as before, zipfile.ZipFile(dest, "x") as after:
                for entry in before.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):
                            raise RuntimeError("Unsafe owned compiler dependency entry")
                        after.writestr(entry.filename, before.read(entry))
            dependencies.append(dest)
        dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        original_java, originals, harness = work / "original-java", work / "originals", work / "harness"
        verify.compile_local([sources / "Trace.java"], original_java, release=8)
        compiler = [choose_jdk(args.kotlin_java_home) / "bin/java", "-cp", kotlin / "*",
            "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler", "-no-stdlib", "-no-reflect", "-classpath",
            os.pathsep.join(map(str, (kotlin / "kotlin-stdlib.jar", original_java))), "-jvm-target", "1.8", "-Xjvm-default=all-compatibility"]
        originals.mkdir()
        verify.run([*compiler, "-d", originals, sources / "ScalarApi.kt"], "kotlinc-original")
        for source in original_java.rglob("*.class"):
            dest = originals / source.relative_to(original_java)
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.open("xb").write(source.read_bytes())
        verify.compile_local(sorted(observers.glob("*.java")), harness, release=8,
            classpath=os.pathsep.join(map(str, (originals, *dependencies))))
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes,
            "harness": harness_hashes, "authored_sources": authored_hashes}, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        methods = set(methods)
        if len(classes) != 4 or len(methods) != 8 or records[OWNER]["methods"][accessor]["access"] != 0x1009:
            raise RuntimeError("Original scalar default inventory or compatibility flags changed")

        def execute(runtime, original, label):
            outputs = []
            for route in range(4):
                for variant in range(2):
                    for mode in range(14):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                            "ScalarDefaultHarness", str(route), str(variant), str(mode), str(original).lower()], f"{label}-{route}-{variant}-{mode}")
                        pattern = f"route={route},variant={variant},mode={mode},attempt=[01],trace=[0-9]+,bits=[0-9a-f]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                            raise RuntimeError("Original scalar default JVM matrix changed")
                        outputs.append(output)
            return outputs

        baseline = execute((originals, harness), True, "baseline")
        classpath = ["--classpath", args.android_jar.resolve()]
        for dep in dependencies:
            classpath += ["--classpath", dep]
        dex = work / "dex"; dex.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *sorted(originals.rglob("*.class"))], "d8-original")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original scalar default DEX count changed")
        dex_hash = hashlib.sha256(source_dex.read_bytes()).hexdigest()
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original scalar default entry")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-disassembly")
        witnesses = 0
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            if block[1] != OWNER or not re.search(r"(?m)^\s+name\s+: 'access\$getValue\$jd'", block[2]):
                continue
            wide = args.scalar in ("J", "D")
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            if (ops != ["invoke-super", "move-result-object", "invoke-virtual", "move-result-wide" if wide else "move-result", "return-wide" if wide else "return"] or
                OWNER + ".getValue:()" + wrapper not in block[2] or wrapper + "." + unbox + ":()" + args.scalar not in block[2]):
                raise RuntimeError("Original scalar compatibility default/unbox Code changed")
            witnesses += 1
        if witnesses != 1:
            raise RuntimeError("Missing original scalar default/unbox witness")
        annotations = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        visible = Counter(re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations))
        if "VISIBILITY_BUILD" in annotations or visible != Counter({"Lkotlin/Metadata;": 3, "Ljava/lang/Deprecated;": 1}):
            raise RuntimeError("D8 changed original scalar default annotation retention")
        expected = deepcopy(records); dropped = {}
        for owner in classes:
            for identity, row in [(owner, expected[owner]), *expected[owner]["fields"].items(), *expected[owner]["methods"].items()]:
                for key in ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations"):
                    if row.get(key):
                        dropped.setdefault(identity, {})[key] = row[key]
                    row[key] = []
        (work / "d8-annotation-loss.json").open("x").write(json.dumps(dropped, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
            coverage["unrecovered_method_count"] or coverage["status"] != "partial" or
            rows[target].get("projection_kind") != "interface-default-body" or
            rows[accessor].get("projection_kind") != "static-interface-default-accessor" or
            coverage.get("class_source_bindings") or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release", 8) < 9):
            raise RuntimeError("Scalar default recovery changed original contracts or omitted the dispatch receipt")
        helper = rows[target]["default_body_helper"]
        if rows[accessor].get("default_body_helper") != helper or not rows[accessor].get("metadata_differences"):
            raise RuntimeError("Scalar default/accessor duplicated its original body or omitted metadata disclosure")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=coverage["minimum_java_release"],
            classpath=os.pathsep.join(map(str, dependencies)))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes - {"Lfixture/ValueView;"}}
        expected_helpers |= {helper, "Lfixture/ScalarApi$DefaultImpls;-><init>()V"}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Scalar default recovery invented or omitted an entry/helper")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method", "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if after[owner].get(key) != expected[owner].get(key):
                    raise RuntimeError("Undisclosed scalar default class metadata changed: " + owner + ":" + key)
            for identity, metadata in expected[owner]["methods"].items():
                wanted = deepcopy(metadata)
                if identity == accessor:
                    wanted["access"] &= ~0x1000
                if after[owner]["methods"].get(identity) != wanted or after_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Undisclosed scalar default method metadata or Exceptions changed: " + identity)
        if execute((rebuilt, harness), False, "rebuilt") != baseline:
            raise RuntimeError("Scalar default changed dispatch, bits, snapshots, partial fields or exact failures")
        original_source = (sources / "ScalarApi.kt").read_text()
        mutations = (("late-result", "return result", "return Trace.input"),
            ("changed-receiver", "Trace.receiver = this", "Trace.receiver = null"),
            ("lost-effect", "Trace.hit(3)", "Trace.hit(8)"))
        for label, before, changed in mutations:
            if original_source.count(before) != 1:
                raise RuntimeError("Owned scalar default counterexample anchor changed")
            wrong = work / label / "ScalarApi.kt"; wrong.parent.mkdir()
            wrong.open("x").write(original_source.replace(before, changed, 1))
            compiled = work / (label + "-classes"); compiled.mkdir()
            verify.run([*compiler, "-d", compiled, wrong], "kotlinc-" + label)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness, *dependencies))),
                "ScalarDefaultHarness", "1", "1", "0", "true"], text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned scalar default access" not in result.stderr:
                raise RuntimeError("Frozen scalar default observer missed " + label)
        wrong = work / "virtual-dispatch/ScalarDefaultCaller.java"; wrong.parent.mkdir()
        text = (observers / "ScalarDefaultCaller.java").read_text()
        before = "ScalarApi.DefaultImpls.getValue(receiver)"
        if text.count(before) != 1:
            raise RuntimeError("Owned scalar default caller anchor changed")
        wrong.open("x").write(text.replace(before, "receiver.getValue()", 1))
        compiled = work / "virtual-dispatch-classes"
        verify.compile_local([wrong], compiled, release=8, classpath=os.pathsep.join(map(str, (originals, *dependencies))))
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness, *dependencies))),
            "ScalarDefaultHarness", "1", "1", "0", "true"], text=True, capture_output=True, timeout=600, env=verify.environment)
        (work / "virtual-dispatch-counterexample.log").open("x").write(result.stdout + result.stderr)
        if result.returncode == 0 or "owned scalar default access" not in result.stderr:
            raise RuntimeError("Frozen scalar default observer missed virtual dispatch")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or
            hashes(sources) != authored_hashes["originals"] or hashes(observers) != authored_hashes["observers"] or
            hashlib.sha256(source_dex.read_bytes()).hexdigest() != dex_hash or
            any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != value for p, value in {**input_hashes, **dependency_hashes}.items())):
            raise RuntimeError("Original scalar default input, SDK, compiler or frozen observers changed")
        evidence = {"status": "success", "scalar": args.scalar, "class_count": len(classes), "method_count": len(methods),
            "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "independent_wrong_programs": 4,
            "original_default_dispatch_and_unchanged_unboxing": "matched", "primitive_bits_partial_fields_snapshots_and_exact_failures": "matched",
            "original_definitions_metadata_and_exceptions": "unchanged except reported accessor synthetic flag and helpers",
            "original_inputs_and_frozen_observers": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
