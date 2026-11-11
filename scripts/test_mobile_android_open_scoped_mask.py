#!/usr/bin/env python3
"""Verify public open generic owners retain their original raw mask constructors."""
from __future__ import annotations
import argparse
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
    from scripts.test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


OWNER = "Lfixture/Span;"
ENTRY = OWNER + "-><init>(FFLjava/lang/Object;ILkotlin/jvm/internal/DefaultConstructorMarker;)V"


def facts(root):
    result = {}
    for path in sorted(root.rglob("*.class")):
        row = KotlinFixtureClassFile(path.read_bytes()).facts()
        if row["name"] in result or path.relative_to(root).as_posix() != row["name"][1:-1] + ".class":
            raise RuntimeError("Duplicate or misplaced owned mask definition")
        result[row["name"]] = row
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "kotlin-lib", "kotlin-java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-trace", action="store_true")
    args = parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-open-scoped-mask-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "open_scoped_mask"
        kotlin = args.kotlin_lib.resolve()
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        inputs = [args.android_jar.resolve(), *sorted(kotlin.glob("*.jar"))]
        if not (kotlin / "kotlin-compiler.jar").is_file():
            raise RuntimeError("Original Kotlin compiler distribution required")
        input_hashes = {str(p): sha(p) for p in inputs}
        dependencies = []
        for name in ("kotlin-stdlib.jar", "annotations-13.0.jar"):
            path = work / name
            with zipfile.ZipFile(kotlin / name) as before, zipfile.ZipFile(path, "x") as after:
                for entry in before.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):
                            raise RuntimeError("Unsafe compiler dependency entry")
                        after.writestr(entry.filename, before.read(entry))
            dependencies.append(path)
        original_java, originals, harness = work / "original-java", work / "originals", work / "harness"
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), original_java, release=8)
        originals.mkdir()
        compiler = [choose_jdk(args.kotlin_java_home) / "bin/java", "-cp", kotlin / "*",
                    "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler", "-no-stdlib", "-no-reflect",
                    "-classpath", os.pathsep.join(map(str, (kotlin / "kotlin-stdlib.jar", original_java))),
                    "-jvm-target", "1.8"]
        verify.run([*compiler, "-d", originals, fixture / "kotlin/fixture/Span.kt"], "kotlinc-original")
        for path in original_java.rglob("*.class"):
            target = originals / path.relative_to(original_java)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
        names = ("OpenScopedMaskCaller", "OpenScopedMaskHarness")
        verify.compile_local([FIXTURES / "harness" / (n + ".java") for n in names], harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (2, 4):
            raise RuntimeError("Original open mask definition inventory changed")
        classes = all_classes - ({"Lfixture/Trace;"} if args.dependency_trace else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        records, exceptions = facts(originals), exception_inventory(originals)
        synthetic = {m for r in records.values() for m, v in r["methods"].items() if v["access"] & 0x1000}
        if (records[OWNER]["access"] & 0x10 or synthetic != {ENTRY}
                or records[OWNER]["methods"][ENTRY]["signature"] is not None
                or records[OWNER]["signature"] != "<T:Ljava/lang/Object;>Ljava/lang/Object;"):
            raise RuntimeError("Original open owner or raw mask ABI changed")
        if args.dependency_trace:
            path = work / "owned-trace.jar"
            with zipfile.ZipFile(path, "x") as archive:
                archive.writestr("fixture/Trace.class", (originals / "fixture/Trace.class").read_bytes())
            dependencies.append(path)
        dependency_hashes = {str(p): sha(p) for p in dependencies}
        classpath = ["--classpath", args.android_jar.resolve()]
        for path in dependencies: classpath += ["--classpath", path]

        def execute(runtime, synthetic_flag, label):
            outputs = []
            for route in range(4):
                for mode in range(24):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))), names[1],
                                         str(route), str(mode), str(synthetic_flag).lower()], f"{label}-{route}-{mode}")
                    pattern = f"route={route},mode={mode},attempt=[01],events=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                        raise RuntimeError("Frozen open mask observer output changed: " + output)
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), True, "baseline")
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]

        def dex(root, destination, label):
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", destination,
                        *[root / p.relative_to(originals) for p in paths]], label)
            source = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [source]:
                raise RuntimeError("Original open mask DEX inventory changed")
            return source

        source_dex = dex(originals, work / "dex", "d8")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original open mask definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-mask-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            if block[1] != OWNER or "(FFLjava/lang/Object;ILkotlin/jvm/internal/DefaultConstructorMarker;)V" not in block[2]: continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
            if (len(ops) != 5 or [op for op, _ in ops] != ["and-int/lit8", "if-eqz", "const/4", "invoke-direct", "return-void"]
                    or "#int 4" not in ops[0][1] or "#int 0" not in ops[2][1]
                    or "Lfixture/Span;.<init>:(FFLjava/lang/Object;)V" not in ops[3][1]):
                raise RuntimeError("Original open mask instruction witness changed")
            witnessed = True
        if not witnessed: raise RuntimeError("Missing original open mask constructor witness")
        annotations = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        if "VISIBILITY_BUILD" in annotations or re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations) != ["Lkotlin/Metadata;"]:
            raise RuntimeError("D8 annotation retention witness changed")
        wanted = {c: deepcopy(records[c]) for c in classes}
        dropped = {}
        for owner, record in wanted.items():
            for identity, row in [(owner, record), *record["fields"].items(), *record["methods"].items()]:
                for key in ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations"):
                    if row.get(key): dropped.setdefault(identity, {})[key] = row[key]
                    row[key] = []
        jvm_field = {"type": "Lkotlin/jvm/JvmField;", "elements": {}}
        nullable = {"type": "Lorg/jetbrains/annotations/Nullable;", "elements": {}}
        expected_dropped = {
            OWNER + "->start:F": {"runtime_invisible_annotations": [jvm_field]},
            OWNER + "->end:F": {"runtime_invisible_annotations": [jvm_field]},
            OWNER + "->data:Ljava/lang/Object;": {"runtime_invisible_annotations": [jvm_field, nullable]},
            OWNER + "-><init>(FFLjava/lang/Object;)V": {"runtime_invisible_parameter_annotations": [[], [], [nullable]]},
        }
        if dropped != expected_dropped: raise RuntimeError("Exact original CLASS annotation inventory changed")
        (work / "d8-annotation-differences.json").open("x").write(json.dumps(dropped, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count", 0) != 1
                or coverage["status"] != "partial" or coverage.get("generic_signature_projections")
                or coverage.get("class_source_bindings") or coverage.get("minimum_java_release") != 25
                or {m for m, r in rows.items() if r["status"] == "source-projected"} != {ENTRY}
                or rows[ENTRY].get("projection_kind") != "synthetic-constructor"
                or rows[ENTRY].get("source_identity") != ENTRY or len(rows[ENTRY].get("metadata_differences", [])) != 1):
            raise RuntimeError("Open mask recovery changed an original definition or contract")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=os.pathsep.join(map(str, dependencies)), release=25)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes != classes or helpers != expected_helpers or set(actual_methods) != methods | helpers:
            raise RuntimeError("Open mask source invented or omitted a runtime definition/helper")
        after, after_exceptions = facts(rebuilt), exception_inventory(rebuilt)
        for owner, record in wanted.items():
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if record.get(key) != after[owner].get(key):
                    raise RuntimeError("Original open mask class metadata changed: " + owner + ":" + key)
            for identity, metadata in record["methods"].items():
                expected = deepcopy(metadata)
                if identity == ENTRY: expected["access"] &= ~0x1000
                if after[owner]["methods"].get(identity) != expected or after_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Original open mask method metadata/Exceptions changed: " + identity)
        if execute((rebuilt, harness), False, "rebuilt") != baseline:
            raise RuntimeError("Recovered open mask changed input bits, payload identity or partial failures")
        controls = (
            ("unknown-target-formal", "(FFTT;)V", "(FFTU;)V"),
            ("wrong-target-scalar", "(FFTT;)V", "(FITT;)V"),
            ("target-method-formal", "(FFTT;)V", "<T:Ljava/lang/Object;>(FFTT;)V"),
            ("incompatible-owner-bound", "<T:Ljava/lang/Object;>Ljava/lang/Object;", "<T:Ljava/lang/CharSequence;>Ljava/lang/Object;"),
        )
        for label, before, changed in controls:
            negative = work / (label + "-originals"); shutil.copytree(originals, negative)
            path = negative / "fixture/Span.class"; code = method_code(path.read_bytes())
            receipt = change_signature(path, before.encode(), changed.encode())
            if method_code(path.read_bytes()) != code: raise RuntimeError("Signature control changed original Code")
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            bad_dex = dex(negative, work / (label + "-dex"), label + "-d8")
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(bad_dex), *map(str, classpath), "--timeout", "600", "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or not any(t in result.stderr.lower() for t in ("generic", "signature", "constructor", "erasure")):
                raise RuntimeError("Open mask accepted an unproved Signature: " + label)
        source = (fixture / "kotlin/fixture/Span.kt").read_text()
        explicit = source.replace("@JvmField val start: Float, @JvmField val end: Float,", "start: Float, end: Float,")
        explicit = explicit.replace("@JvmField val data: T? = null) {", "data: T? = null) {\n    @JvmField val start: Float = start\n    @JvmField val end: Float = end\n    @JvmField val data: T? = data")
        fields = "    @JvmField val start: Float = start\n    @JvmField val end: Float = end\n    @JvmField val data: T? = data\n"
        late = explicit.replace(fields, "").replace("\n}", "\n" + fields + "}")
        wrong_programs = (
            ("wrong-first", explicit.replace("val start: Float = start", "val start: Float = end"), 0),
            ("wrong-second", explicit.replace("val end: Float = end", "val end: Float = start"), 0),
            ("narrowed-first", explicit.replace("val start: Float = start", "val start: Float = start.toInt().toFloat()"), 0),
            ("narrowed-second", explicit.replace("val end: Float = end", "val end: Float = end.toInt().toFloat()"), 0),
            ("lost-payload", explicit.replace("val data: T? = data", "val data: T? = null"), 0),
            ("wrong-default", explicit.replace("data: T? = null)", "data: T? = Any() as T?)"), 2),
            ("late-fields", late, 8),
            ("missing-publication", explicit.replace("Trace.partial = this", ""), 0),
            ("late-publication", explicit.replace("Trace.partial = this\n        Trace.step(1)", "Trace.step(1)\n        Trace.partial = this"), 8),
            ("wrong-payload-snapshot", explicit.replace("Trace.seen = data", "Trace.seen = this"), 0),
            ("duplicated-step", explicit.replace("Trace.step(1)", "Trace.step(1); Trace.step(1)"), 0),
            ("wrapped-failure", explicit.replace("Trace.step(1)", "try { Trace.step(1) } catch (failure: RuntimeException) { throw IllegalArgumentException() }"), 8),
        )
        for label, text, mode in wrong_programs:
            if text == explicit: raise RuntimeError("Original open mask counterexample anchor changed: " + label)
            path = work / label / "Span.kt"; path.parent.mkdir(); path.open("x").write(text)
            wrong = work / (label + "-classes"); wrong.mkdir()
            verify.run([*compiler, "-d", wrong, path], "kotlinc-" + label)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness, *dependencies))), names[1], "0", str(mode), "true"],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned open scoped mask" not in result.stderr:
                raise RuntimeError("Frozen open mask observer missed " + label)
        if ({"originals": hashes(originals), "harness": hashes(harness)} != frozen
                or any(sha(Path(p)) != h for p, h in {**input_hashes, **dependency_hashes}.items())):
            raise RuntimeError("Frozen original compiler, inputs, observer or SDK changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "wrong_programs": len(wrong_programs),
                   "same_Code_Signature_controls": len(controls), "dependency_trace": args.dependency_trace}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
