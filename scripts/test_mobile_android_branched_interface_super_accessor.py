#!/usr/bin/env python3
"""Verify inherited Kotlin static accessors over multiple direct interfaces."""
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
    from scripts.test_mobile_android_interface_default_access import DefaultFixtureClassFile
    from scripts.test_mobile_android_visibility_generic_bridge import ParameterClassFile
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_interface_default_access import DefaultFixtureClassFile
    from test_mobile_android_visibility_generic_bridge import ParameterClassFile
    from test_mobile_android_static_defaults import method_code

PARENT = "Lfixture/Parent;"
CHILD = "Lfixture/Child;"
ACCESS = CHILD + "->access$measure$jd(Lfixture/Child;J)I"


class FixtureClassFile(ParameterClassFile, DefaultFixtureClassFile):
    SCHEMAS = {**ParameterClassFile.SCHEMAS, **DefaultFixtureClassFile.SCHEMAS}


def compiler_facts(root):
    result = {}
    for path in sorted(root.rglob("*.class")):
        row = FixtureClassFile(path.read_bytes()).facts()
        if row["name"] in result or path.relative_to(root).as_posix() != row["name"][1:-1] + ".class":
            raise RuntimeError("Duplicate or misplaced original compiler class")
        result[row["name"]] = row
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "kotlin-lib", "kotlin-java-home", "compiler-java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-trace", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-branched-interface-super-accessor-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        original_compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "branched_interface_super_accessor"
        kotlin = args.kotlin_lib.resolve()
        inputs = [args.android_jar.resolve(), *sorted(kotlin.glob("*.jar"))]
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        input_hashes = {str(p): sha(p) for p in inputs}
        dependencies = []
        cli_hash = sha(args.neverd)
        for source in (kotlin / "kotlin-stdlib.jar", kotlin / "annotations-13.0.jar"):
            target = work / source.name
            with zipfile.ZipFile(source) as before, zipfile.ZipFile(target, "x") as after:
                for entry in before.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):
                            raise RuntimeError("Unsafe compiler dependency")
                        after.writestr(entry.filename, before.read(entry))
            dependencies.append(target)
        originals, harness = work / "originals", work / "harness"
        trace, parent, child = work / "trace", work / "parent", work / "child"
        original_compiler.compile_local(sorted((fixture / "java").rglob("*.java")), trace, release=8)
        compiler = [choose_jdk(args.kotlin_java_home) / "bin/java", "-cp", kotlin / "*",
                    "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler", "-no-stdlib", "-no-reflect", "-jvm-target", "1.8"]
        verify.run([*compiler, "-classpath", os.pathsep.join(map(str, (kotlin / "kotlin-stdlib.jar", trace))),
                    "-Xjvm-default=all", "-d", parent, *sorted((fixture / "parent").rglob("*.kt"))], "kotlinc-parent")
        verify.run([*compiler, "-classpath", os.pathsep.join(map(str, (kotlin / "kotlin-stdlib.jar", trace, parent))),
                    "-Xjvm-default=all-compatibility", "-d", child, *sorted((fixture / "child").rglob("*.kt"))], "kotlinc-child")
        originals.mkdir()
        for root in (trace, parent, child):
            for source in root.rglob("*.class"):
                target = originals / source.relative_to(root)
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.exists():
                    raise RuntimeError("Duplicate original compiler definition")
                shutil.copy2(source, target)
                if source.read_bytes() != target.read_bytes() or method_code(source.read_bytes()) != method_code(target.read_bytes()):
                    raise RuntimeError("Original compiler Code changed")
        original_compiler.compile_local(sorted((fixture / "receivers").rglob("*.java")), work / "receivers",
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        for source in (work / "receivers").rglob("*.class"):
            target = originals / source.relative_to(work / "receivers")
            shutil.copy2(source, target)
        names = ("BranchedInterfaceSuperAccessorCaller", "BranchedInterfaceSuperAccessorHarness")
        original_compiler.compile_local([FIXTURES / "harness" / (n + ".java") for n in names], harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 8 or len(all_methods) != 15:
            raise RuntimeError("Original branched interface super accessor inventory changed")
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        if (set(records[PARENT]["methods"]) != {PARENT + "->measure(J)I"}
                or set(records[CHILD]["methods"]) != {ACCESS}
                or records[PARENT]["signature"] is not None or records[CHILD]["signature"] is not None):
            raise RuntimeError("Original inherited declaration or absent ancestor accessor changed")
        declarations = {"Lfixture/Trace;"} if args.dependency_trace else set()
        classes = all_classes - declarations
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        if declarations:
            target = work / "owned-trace.jar"
            with zipfile.ZipFile(target, "x") as jar:
                jar.writestr("fixture/Trace.class", (originals / "fixture/Trace.class").read_bytes())
            dependencies.append(target)
        dependency_hashes = {str(p): sha(p) for p in dependencies}

        def execute(runtime, label, rebuilt=False):
            outputs = []
            for route in range(7):
                for mode in range(12):
                    for future in (False, True):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                                             "probe." + names[1], str(route), str(mode), str(future).lower(), str(rebuilt).lower()],
                                            f"{label}-{route}-{mode}-{future}")
                        if output != "attempts=2,branched-interface-super-accessor=matched\n":
                            raise RuntimeError("Frozen branched interface super accessor observer changed")
                        outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline) + "\n")
        mutations = [
            ("wrong-receiver", "java", "Trace.java", "parent = owner;", "parent = null;", 0),
            ("narrowed-wide-input", "java", "Trace.java", "bits = value;", "bits = (int) value;", 0),
            ("lost-entry", "java", "Trace.java", "hit(1);", ";", 0),
            ("lost-action", "java", "Trace.java", "action.run();", ";", 0),
            ("duplicate-action", "java", "Trace.java", "action.run();", "action.run(); action.run();", 0),
            ("wrapped-checked", "java", "Trace.java", "action.run();", "try { action.run(); } catch (Throwable failure) { throw new IllegalStateException(failure); }", 2),
            ("wrong-after", "java", "Trace.java", "last = value;", "last = 0;", 0),
            ("lost-after-callback", "java", "Trace.java", "afterAction.run();", ";", 8),
            ("lost-read-callback", "receivers", "Receiver.java", "Trace.readAction.run();", ";", 5),
            ("wrong-read-result", "receivers", "Receiver.java", "return Trace.answer;", "return Trace.answer + 1;", 0),
            ("virtual-redispatch", "receivers", "Core.java", "Child.DefaultImpls.measure(receiver, value)", "receiver.measure(value)", 0),
            ("wrong-forwarded-wide-word", "receivers", "Core.java", "Child.DefaultImpls.measure(receiver, value)", "Child.DefaultImpls.measure(receiver, (int) value)", 0),
            ("duplicate-dispatch", "receivers", "Core.java", "int result = Child.DefaultImpls.measure(receiver, value);", "int result = Child.DefaultImpls.measure(receiver, value); Child.DefaultImpls.measure(receiver, value);", 0),
            ("lost-caller-tail", "receivers", "Core.java", "Trace.hit(0);", ";", 0),
        ]
        for label, folder, filename, anchor, replacement, mode in mutations:
            text = (fixture / folder / "fixture" / filename).read_text()
            if text.count(anchor) != 1:
                raise RuntimeError("Wrong-program anchor changed: " + label)
            source = work / label / filename
            source.parent.mkdir()
            source.open("x").write(text.replace(anchor, replacement))
            variant = work / (label + "-classes")
            original_compiler.compile_local([source], variant, classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (variant, originals, harness, *dependencies))),
                                     "probe." + names[1], "0", str(mode), "true", "false"],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned branched interface super accessor mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program: " + label)

        classpath = ["--classpath", args.android_jar.resolve()]
        for target in dependencies:
            classpath += ["--classpath", target]
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        dex = work / "dex"
        dex.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original branched interface super accessor DEX count changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original branched interface super accessor definitions")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-inherited-default-disassembly")
        if "invoke-super" not in disassembly or "Lfixture/Child;.measure:(J)I" not in disassembly:
            raise RuntimeError("D8 lost the original inherited self-owned invoke-super witness")
        expected = {c: deepcopy(records[c]) for c in classes}
        dropped = {}
        for c in classes:
            for identity, row in [(c, expected[c]), *expected[c]["methods"].items()]:
                for key in ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations"):
                    if row.get(key):
                        dropped.setdefault(identity, {})[key] = row[key]
                    row[key] = []
        (work / "d8-class-annotation-loss.json").open("x").write(json.dumps(dropped, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial"
                or coverage.get("projected_method_count") != 1 or coverage.get("minimum_java_release") != 9
                or rows[ACCESS].get("projection_kind") != "static-super-accessor"
                or not rows[ACCESS].get("metadata_differences") or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Branched interface super accessor recovery changed original entries or contracts")
        for identity, row in rows.items():
            expected_status = "source-projected" if identity == ACCESS else "recovered" if records[row["class"]]["methods"][identity]["code"] else "declaration-only"
            if row["status"] != expected_status:
                raise RuntimeError("Original default accessor Code/declaration role changed")
        helper = rows[ACCESS]["super_dispatch_helper"]
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=os.pathsep.join(map(str, dependencies)), release=9)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        body_owners = {c for c in classes if any(row["code"] for row in records[c]["methods"].values())}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in body_owners}
        expected_helpers |= {helper, "Lfixture/Child$DefaultImpls;-><init>()V"}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Branched interface super accessor recovery invented or omitted an original entry/helper")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for identity in expected_helpers:
            metadata = after[identity.split("->", 1)[0]]["methods"][identity]
            throw_helper = "->__neverdThrow(" in identity
            signature = "<E:Ljava/lang/Throwable;>(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;^TE;" if throw_helper else None
            if (metadata["access"] != (0x000A if throw_helper else 0x0002)
                    or metadata["signature"] != signature or not metadata["code"]):
                raise RuntimeError("Generated helper changed its exact private role or contract")
        for c in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if after[c].get(key) != expected[c].get(key):
                    raise RuntimeError("Undisclosed branched interface super accessor class metadata changed: " + c + ":" + key)
            for identity, metadata in expected[c]["methods"].items():
                wanted = deepcopy(metadata)
                if identity == ACCESS:
                    wanted["access"] &= ~0x1000
                if after[c]["methods"].get(identity) != wanted or after_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Undisclosed branched interface super accessor method metadata or Exceptions changed: " + identity)
        if execute((rebuilt, harness), "rebuilt", True) != baseline:
            raise RuntimeError("Branched interface super accessor recovery changed default dispatch, effects, failure or identities")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or sha(args.neverd) != cli_hash or {str(p): sha(p) for p in inputs} != input_hashes or {str(p): sha(p) for p in dependencies} != dependency_hashes):
            raise RuntimeError("Original compiler inputs or frozen observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "stages": 2, "independent_wrong_programs": len(mutations),
                    "original_Code": "compiler output unchanged", "all_generated_Java": "compiled",
                    "original_metadata_and_Exceptions": "matched except exact disclosed ordinary accessor synthetic bit and D8 CLASS annotation loss",
                    "default_dispatch_and_receiver_input_result_identity": "matched", "dependency_trace": args.dependency_trace,
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
