#!/usr/bin/env python3
"""Verify captured constructor alignment retains original outer-formal identity."""
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
    from scripts.test_mobile_android_open_scoped_mask import facts
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_open_scoped_mask import facts
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


OWNER = "Lfixture/Tree;"
NODE = "Lfixture/Tree$Node;"
CAPTURE = NODE + "->this$0:" + OWNER
CTOR = NODE + "-><init>(Lfixture/Tree;FFLjava/lang/Object;I)V"


def metadata(coverage, records, exceptions, rebuilt):
    bindings = coverage.get("class_source_bindings", [])
    if (len(bindings) != 1 or bindings[0].get("class") != NODE
            or bindings[0].get("binding_kind") != "inner-to-static-member"
            or bindings[0].get("outer_capture_field") != CAPTURE
            or not any("synthetic capture-field" in text for text in bindings[0].get("metadata_differences", []))
            or not any("constructor generic signatures" in text for text in bindings[0].get("metadata_differences", []))):
        raise RuntimeError("Captured superclass ownership/capture disclosure changed")
    declarations = {NODE: ("class", records[NODE]["signature"]),
                    CAPTURE: ("field", records[NODE]["fields"][CAPTURE]["signature"]),
                    CTOR: ("method", records[NODE]["methods"][CTOR]["signature"])}
    projections = {p["declaration"]: p for p in coverage.get("generic_signature_projections", [])}
    if set(projections) != set(declarations):
        raise RuntimeError("Captured superclass projection inventory changed")
    for identity, (kind, signature) in declarations.items():
        row = projections[identity]
        tokens = [(m.start(), OWNER + "::" + m[1]) for m in re.finditer(r"T([TU]);", signature)]
        if (row.get("declaration_kind") != kind or row.get("original_signature") != signature
                or not row.get("metadata_differences") or len(row["changes"]) != len(tokens)
                or sorted((p["offset"], p["original_type"]) for p in row["changes"]) != tokens
                or any(p["kind"] != "static-inner-type-variable" or p["source_type"] != "Ljava/lang/Object;" for p in row["changes"])):
            raise RuntimeError("Captured superclass lost an original outer-formal projection: " + identity)
    rows = {r["identity"]: r for r in coverage["methods"]}
    if (rows[CTOR].get("generic_signature_parameter_indices") != [1, 2, 3, 4]
            or rows[CTOR].get("generic_signature_omitted_parameters") != 1
            or rows[CTOR].get("projection_kind") != "inner-to-static-member"):
        raise RuntimeError("Captured superclass changed its uniquely aligned enclosing input")
    after, after_exceptions = facts(rebuilt), exception_inventory(rebuilt)
    for owner, record in records.items():
        wanted = deepcopy(record)
        if owner == NODE:
            if wanted["fields"][CAPTURE]["access"] != 0x1010:
                raise RuntimeError("Original synthetic enclosing field changed")
            wanted["fields"][CAPTURE]["access"] &= ~0x1000
            wanted["fields"][CAPTURE]["signature"] = "Lfixture/Tree<Ljava/lang/Object;Ljava/lang/Object;>;"
            wanted["inner_class"]["access"] |= 8
            wanted["signature"] = "Lfixture/Base<Ljava/lang/Object;>;"
            # Every explicit source parameter is now a plain descriptor. Javac
            # omits the redundant Signature; its original omitted capture and
            # exact formal projection are separately checked above.
            wanted["methods"][CTOR]["signature"] = None
        for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                    "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
            if after[owner].get(key) != wanted.get(key):
                raise RuntimeError("Undisclosed captured superclass class metadata change: " + owner + ":" + key)
        for identity, method in wanted["methods"].items():
            if after[owner]["methods"].get(identity) != method or after_exceptions.get(identity) != exceptions[identity]:
                raise RuntimeError("Undisclosed captured superclass method metadata/Exceptions change: " + identity)
    return after


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "kotlin-lib", "kotlin-java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-inputs", action="store_true")
    args = parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-captured-owner-super-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "captured_owner_super"
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
                    "-classpath", os.pathsep.join(map(str, (kotlin / "kotlin-stdlib.jar", original_java))), "-jvm-target", "1.8"]
        verify.run([*compiler, "-d", originals, *sorted((fixture / "kotlin").rglob("*.kt"))], "kotlinc-original")
        for path in original_java.rglob("*.class"):
            target = originals / path.relative_to(original_java)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
        names = ("CapturedOwnerSuperCaller", "CapturedOwnerSuperHarness")
        verify.compile_local([FIXTURES / "harness" / (n + ".java") for n in names], harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (5, 6):
            raise RuntimeError("Original captured superclass definition inventory changed")
        providers = {"Lfixture/Base;", "Lfixture/Trace;", "Lfixture/Probe;"}
        classes = all_classes - (providers if args.dependency_inputs else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        records, exceptions = facts(originals), exception_inventory(originals)
        if (records[NODE]["signature"] != "Lfixture/Base<TT;>;"
                or records[OWNER]["signature"] != "<T:Ljava/lang/Object;U:Ljava/lang/Object;>Ljava/lang/Object;"
                or records[NODE]["methods"][CTOR]["signature"] != "(FFTT;I)V"):
            raise RuntimeError("Original distinct outer/parent formals or omitted enclosing Signature changed")
        if args.dependency_inputs:
            path = work / "owned-inputs.jar"
            with zipfile.ZipFile(path, "x") as archive:
                for owner in sorted(providers):
                    name = owner[1:-1] + ".class"
                    archive.writestr(name, (originals / name).read_bytes())
            dependencies.append(path)
        dependency_hashes = {str(p): sha(p) for p in dependencies}
        classpath = ["--classpath", args.android_jar.resolve()]
        for path in dependencies: classpath += ["--classpath", path]

        def execute(runtime, original, label):
            outputs = []
            for route in range(4):
                for mode in range(18):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))), names[1],
                                         str(route), str(mode), str(original).lower()], f"{label}-{route}-{mode}")
                    pattern = f"route={route},mode={mode},attempt=[01],events=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                        raise RuntimeError("Frozen captured superclass observer output changed: " + output)
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
                raise RuntimeError("Original captured superclass DEX inventory changed")
            return source

        source_dex = dex(originals, work / "dex", "d8")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original captured superclass definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-capture-super-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != NODE or not name or name[1] != "<init>": continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
            captures = [i for i, (op, tail) in enumerate(ops) if op == "iput-object" and "Lfixture/Tree$Node;.this$0:Lfixture/Tree;" in tail]
            supers = [i for i, (op, tail) in enumerate(ops) if op.startswith("invoke-direct") and "Lfixture/Base;.<init>:(FFLjava/lang/Object;)V" in tail]
            registers = re.search(r"(?m)^\s+registers\s+:\s+(\d+)", block[2])
            incoming = re.search(r"(?m)^\s+ins\s+:\s+(\d+)", block[2])
            if len(captures) != 1 or len(supers) != 1 or captures[0] >= supers[0] or not registers or not incoming or int(incoming[1]) != 6:
                raise RuntimeError("Original enclosing capture/superclass witness changed")
            self_word = int(registers[1]) - int(incoming[1])
            if ([int(r) for r in re.findall(r"\bv(\d+)\b", ops[captures[0]][1])] != [self_word + 1, self_word]
                    or [int(r) for r in re.findall(r"\bv(\d+)\b", ops[supers[0]][1])] != [self_word, self_word + 2, self_word + 3, self_word + 4]):
                raise RuntimeError("Original captured superclass word frame changed")
            witnessed = True
        if not witnessed: raise RuntimeError("Missing original captured superclass constructor")
        annotations = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        if ("VISIBILITY_BUILD" in annotations
                or re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations) != ["Lkotlin/Metadata;"] * (2 if args.dependency_inputs else 3)):
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
            "Lfixture/Base;->start:F": {"runtime_invisible_annotations": [jvm_field]},
            "Lfixture/Base;->end:F": {"runtime_invisible_annotations": [jvm_field]},
            "Lfixture/Base;->data:Ljava/lang/Object;": {"runtime_invisible_annotations": [jvm_field, nullable]},
            "Lfixture/Base;-><init>(FFLjava/lang/Object;)V": {"runtime_invisible_parameter_annotations": [[], [], [nullable]]},
            OWNER + "->seed:Ljava/lang/Object;": {"runtime_invisible_annotations": [jvm_field, nullable]},
            NODE + "->color:I": {"runtime_invisible_annotations": [jvm_field]},
            NODE + "->min:F": {"runtime_invisible_annotations": [jvm_field]},
            NODE + "->max:F": {"runtime_invisible_annotations": [jvm_field]},
            CTOR: {"runtime_invisible_parameter_annotations": [[], [], [nullable], []]},
        }
        expected_dropped = {k: v for k, v in expected_dropped.items() if k.split("->", 1)[0] in classes}
        if dropped != expected_dropped: raise RuntimeError("Exact original CLASS annotation inventory changed")
        (work / "d8-annotation-differences.json").open("x").write(json.dumps(dropped, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count", 0) != 1
                or coverage["status"] != "partial" or coverage.get("minimum_java_release") != 25
                or {m for m, r in rows.items() if r["status"] == "source-projected"} != {CTOR}):
            raise RuntimeError("Captured superclass recovery changed an original definition or contract")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=os.pathsep.join(map(str, dependencies)), release=25)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c != "Lfixture/Probe;"}
        if actual_classes != classes or helpers != expected_helpers or set(actual_methods) != methods | helpers:
            raise RuntimeError("Captured superclass source invented or omitted a runtime definition/helper")
        after = metadata(coverage, wanted, exceptions, rebuilt)
        (work / "metadata-accounting.json").open("x").write(json.dumps({"original": records, "rebuilt": after}, indent=2) + "\n")
        if execute((rebuilt, harness), False, "rebuilt") != baseline:
            raise RuntimeError("Recovered captured superclass changed capture, bits, payload identity or partial failures")
        controls = (
            ("different-outer-input", "(FFTT;I)V", "(FFTU;I)V"),
            ("different-outer-parent", "Lfixture/Base<TT;>;", "Lfixture/Base<TU;>;"),
            ("method-shadow", "(FFTT;I)V", "<T:Ljava/lang/Object;>(FFTT;I)V"),
            ("wrong-scalar", "(FFTT;I)V", "(FITT;I)V"),
        )
        for label, before, changed in controls:
            negative = work / (label + "-originals"); shutil.copytree(originals, negative)
            path = negative / "fixture/Tree$Node.class"; code = method_code(path.read_bytes())
            receipt = change_signature(path, before.encode(), changed.encode())
            if method_code(path.read_bytes()) != code: raise RuntimeError("Signature control changed original Code")
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            bad_dex = dex(negative, work / (label + "-dex"), label + "-d8")
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(bad_dex), *map(str, classpath), "--timeout", "600", "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or not any(t in result.stderr.lower() for t in ("generic", "signature", "constructor", "erasure")):
                raise RuntimeError("Captured superclass accepted unproved scope/Signature: " + label)
        base = (fixture / "kotlin/fixture/Base.kt").read_text()
        tree = (fixture / "kotlin/fixture/Tree.kt").read_text()
        late_color = tree.replace("@JvmField val color: Int", "color: Int")
        late_color = late_color.replace("\n    }", "\n        @JvmField val color: Int = color\n    }")
        fields = "        @JvmField val min: Float = start\n        @JvmField val max: Float = end\n"
        late_fields = tree.replace(fields, "").replace("\n    }", "\n" + fields + "    }")
        wrong_color = tree.replace("@JvmField val color: Int", "color: Int").replace("        @JvmField val min:", "        @JvmField val color: Int = color + 1\n        @JvmField val min:")
        wrong_programs = (
            ("swapped-floats", "Tree", tree.replace("Base<T>(start,end,data)", "Base<T>(end,start,data)"), 0),
            ("narrowed-first", "Tree", tree.replace("Base<T>(start,end,data)", "Base<T>(start.toInt().toFloat(),end,data)"), 0),
            ("narrowed-second", "Tree", tree.replace("Base<T>(start,end,data)", "Base<T>(start,end.toInt().toFloat(),data)"), 0),
            ("lost-payload", "Tree", tree.replace("Base<T>(start,end,data)", "Base<T>(start,end,null)"), 0),
            ("missing-base-publication", "Base", base.replace("Trace.base = this", ""), 0),
            ("late-base-publication", "Base", base.replace("Trace.base = this\n        Trace.step(1)", "Trace.step(1)\n        Trace.base = this"), 3),
            ("lost-probe", "Base", base.replace("Trace.probe.accept(this)", ""), 0),
            ("duplicated-probe", "Base", base.replace("Trace.probe.accept(this)", "Trace.probe.accept(this); Trace.probe.accept(this)"), 0),
            ("late-color", "Tree", late_color, 9),
            ("late-fields", "Tree", late_fields, 9),
            ("wrong-color", "Tree", wrong_color, 0),
            ("wrapped-failure", "Base", base.replace("Trace.step(1)", "try { Trace.step(1) } catch (failure: RuntimeException) { throw IllegalArgumentException() }"), 3),
        )
        changed_compiler = list(compiler)
        changed_compiler[changed_compiler.index("-classpath") + 1] += os.pathsep + str(originals)
        for label, name, text, mode in wrong_programs:
            if text == (base if name == "Base" else tree):
                raise RuntimeError("Original captured superclass counterexample anchor changed: " + label)
            path = work / label / (name + ".kt"); path.parent.mkdir(); path.open("x").write(text)
            wrong = work / (label + "-classes"); wrong.mkdir()
            verify.run([*changed_compiler, "-d", wrong, path], "kotlinc-" + label)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness, *dependencies))), names[1], "0", str(mode), "true"],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned captured owner super" not in result.stderr:
                raise RuntimeError("Frozen captured superclass observer missed " + label)
        if ({"originals": hashes(originals), "harness": hashes(harness)} != frozen
                or any(sha(Path(p)) != h for p, h in {**input_hashes, **dependency_hashes}.items())):
            raise RuntimeError("Frozen original compiler, inputs, observer or SDK changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "wrong_programs": len(wrong_programs),
                   "same_Code_Signature_controls": len(controls), "dependency_inputs": args.dependency_inputs}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
