#!/usr/bin/env python3
"""Verify protected interface-bounded void calls retain context and dispatch."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


SIGNATURE = "<T::Lfixture/Marker;>(TT;Lfixture/Base;)V"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-protected-marker-void-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "protected_marker_void/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        name = "ProtectedMarkerVoidHarness"
        verify.compile_local([FIXTURES / "harness" / (name + ".java")], harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        (work / "baseline-cli-sha256.txt").open("x").write(sha(args.neverd) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (5, 11):
            raise RuntimeError("Authored protected marker void inventory changed")
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        base = records["Lfixture/Base;"]
        protected_slot = "->consume(Lfixture/Marker;Lfixture/Base;)V"
        if (base["signature"] is not None or base["access"] != 0x421
                or base["methods"]["Lfixture/Base;" + protected_slot]["signature"] != SIGNATURE
                or base["methods"]["Lfixture/Base;" + protected_slot]["access"] != 4
                or records["Lfixture/Subject;"]["methods"]["Lfixture/Subject;" + protected_slot]["signature"] != SIGNATURE
                or records["Lfixture/Subject;"]["methods"]["Lfixture/Subject;" + protected_slot]["access"] != 4
                or base["methods"]["Lfixture/Base;->write(Lfixture/Marker;Lfixture/Base;I)V"]["signature"] is not None
                or records["Lfixture/Passive;"]["methods"]["Lfixture/Passive;->forward(Lfixture/Marker;Lfixture/Base;I)V"]["signature"] != "<U::Lfixture/Marker;>(TU;Lfixture/Base;I)V"
                or any(exceptions.values())):
            raise RuntimeError("Authored protected marker void owner, scoped inputs or exception metadata changed")
        excluded = {"Lfixture/Base;"} if args.dependency_provider else set()
        classes = all_classes - excluded
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        if (len(classes), len(methods)) != ((4, 7) if excluded else (5, 11)):
            raise RuntimeError("Authored protected marker void provider inventory changed")
        provider = work / "owned-provider.jar"
        with zipfile.ZipFile(provider, "x") as archive:
            archive.writestr("fixture/Base.class", (originals / "fixture/Base.class").read_bytes())
        inputs = {str(path.resolve()): sha(path) for path in (args.neverd, args.android_jar, provider)}
        classpath = ["--classpath", args.android_jar.resolve()]
        if excluded:
            classpath += ["--classpath", provider]

        def execute(runtime, label):
            results = []
            for route in range(2):
                for shape in range(6):
                    for mode in range(6):
                        for receiver in range(2):
                            text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), name,
                                               str(route), str(shape), str(mode), str(receiver)],
                                              f"{label}-{route}-{shape}-{mode}-{receiver}")
                            if text != "attempts=2,protected-marker-void=matched\n":
                                raise RuntimeError("Authored protected marker void observer changed")
                            results.append(text)
            return results

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        texts = {path.name: path.read_text() for path in sorted(fixture.rglob("*.java"))}
        override = """        Trace.hit(5, this, input, context);
        saved = input;
        this.context = context;
        Trace.hit(6, this, input, context);
        context.seen = input;
        writes++;
        Trace.hit(7, this, input, context);"""
        wrong = [
            ("missing-call", "Base.java", "consume(input, context);", ";", 0, 3, 0, 0),
            ("repeated-call", "Base.java", "consume(input, context);", "consume(input, context); consume(input, context);", 0, 3, 0, 0),
            ("wrong-receiver", "Base.java", "consume(input, context);", "new Passive().consume(input, context);", 0, 3, 0, 0),
            ("wrong-input", "Base.java", "consume(input, context);", "consume(null, context);", 0, 3, 0, 0),
            ("wrong-context", "Base.java", "consume(input, context);", "consume(input, this);", 0, 3, 0, 0),
            ("lost-prefix", "Base.java", "mark(tag);", ";", 0, 3, 0, 0),
            ("late-prefix", "Base.java", "mark(tag);\n        consume(input, context);", "consume(input, context);\n        mark(tag);", 0, 3, 0, 0),
            ("wrong-marker", "Base.java", "marker = value;", "marker = 0;", 0, 3, 0, 0),
            ("early-null-check", "Base.java", "mark(tag);", "if (context == null) throw new NullPointerException(); mark(tag);", 0, 1, 1, 0),
            ("lost-saved-store", "Base.java", "saved = input;", ";", 0, 3, 0, 0),
            ("lost-context-store", "Base.java", "this.context = context;", ";", 0, 3, 0, 0),
            ("lost-context-write", "Base.java", "context.seen = input;", ";", 0, 3, 0, 0),
            ("lost-override", "Subject.java", override, "        super.consume(input, context);", 0, 3, 0, 1),
            ("forced-super", "Passive.java", "consume(input, context);", "super.consume(input, context);", 1, 3, 0, 1),
            ("wrapped-failure", "Trace.java", "hook.observe(phase, owner, input, context);",
             "try { hook.observe(phase, owner, input, context); } catch (Throwable failure) { throw new IllegalStateException(failure); }", 0, 3, 3, 0),
        ]
        for label, filename, anchor, replacement, route, shape, mode, receiver in wrong:
            text = texts[filename]
            if text.count(anchor) != 1:
                raise RuntimeError("Authored protected marker void mutation anchor changed: " + label)
            changed = work / label / filename
            changed.parent.mkdir()
            changed.open("x").write(text.replace(anchor, replacement))
            compiled = work / (label + "-classes")
            verify.compile_local([changed], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     name, str(route), str(shape), str(mode), str(receiver)], text=True,
                                    capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned protected marker void mismatch" not in result.stderr:
                raise RuntimeError("Observer accepted an incorrect protected marker void program: " + label)

        paths = [path.relative_to(originals) for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]

        def dex(source, label, declarations):
            dexdir = work / label
            dexdir.mkdir()
            text = verify.run([*verify.d8, "--debug", "--min-api", "24", *declarations,
                               "--output", dexdir, *[source / path for path in paths]], label)
            if "Signature is ignored" in text:
                raise RuntimeError("D8 ignored an authored protected marker void Signature")
            return dexdir / "classes.dex"

        source_dex = dex(originals, "dex", classpath)
        dex_sha = sha(source_dex)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an authored protected marker void definition")
        (work / "inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        (work / "source-dex-sha256.txt").open("x").write(dex_sha + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        report = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in report["methods"]}
        if (report["status"] != "recovered" or set(rows) != methods
                or report["class_count"] != len(classes) or report["method_count"] != len(methods)
                or report["unrecovered_method_count"] or report.get("projected_method_count", 0)
                or report.get("declaration_only_method_count") != 1
                or report["recovered_method_count"] != len(methods) - 1
                or report.get("class_source_bindings") or report.get("generic_signature_projections")
                or report.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Protected marker void recovery changed definition roles or source contracts")
        if any(row["status"] != ("declaration-only" if all_methods[identity] == "declaration" else "recovered")
               for identity, row in rows.items()):
            raise RuntimeError("Protected marker void original method roles changed")

        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=8,
                             classpath=provider if excluded else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helper_rows = report.get("generated_source_helpers", [])
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in helper_rows}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes if owner != "Lfixture/Marker;"}
        if (actual_classes != classes or helpers != expected_helpers or len(helper_rows) != len(expected_helpers)
                or set(actual_methods) != methods | helpers
                or any(row.get("kind") != "throw-helper" or row.get("static") is not True for row in helper_rows)):
            raise RuntimeError("Compiled protected marker void definitions or helper roles changed")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != after[owner].get(key):
                    raise RuntimeError("Protected marker void class metadata changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                if after[owner]["methods"].get(identity) != metadata or after_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Protected marker void method role, Signature or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, provider) if excluded else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Protected marker void projection changed context identity, stores, dispatch or failures")

        controls = [
            ("unknown-input-formal", b"<T::Lfixture/Marker;>(TMissing;Lfixture/Base;)V"),
            ("bound-erasure", b"<T:Ljava/lang/Object;>(TT;Lfixture/Base;)V"),
            ("context-erasure", b"<T::Lfixture/Marker;>(TT;Ljava/lang/Object;)V"),
            ("result-erasure", b"<T::Lfixture/Marker;>(TT;Lfixture/Base;)TT;"),
        ]
        for label, changed in controls:
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            path = negative / "fixture/Base.class"
            before_code = method_code(path.read_bytes())
            receipt = change_signature(path, SIGNATURE.encode(), changed)
            if method_code(path.read_bytes()) != before_code:
                raise RuntimeError("Protected marker void Signature control changed Code")
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            negative_classpath = list(classpath)
            if excluded:
                negative_jar = work / (label + "-provider.jar")
                with zipfile.ZipFile(negative_jar, "x") as archive:
                    archive.writestr("fixture/Base.class", path.read_bytes())
                negative_classpath[-1] = negative_jar
            negative_dex = dex(negative, label + "-dex", negative_classpath)
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, negative_classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode != 1 or rejected.exists() or not result.stderr.startswith("error: "):
                raise RuntimeError("Unproved protected marker void metadata was accepted: " + label)
        if (hashes(originals) != frozen["originals"] or hashes(harness) != frozen["harness"] or sha(source_dex) != dex_sha
                or any(sha(Path(path)) != digest for path, digest in inputs.items())):
            raise RuntimeError("Frozen protected marker void input changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "all_java_units": len(list((output / "sources").rglob("*.java"))),
                   "minimum_java_release": 8, "declaration_only_method_count": 1,
                   "fresh_jvm_cases": 144, "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "wrong_program_labels": [row[0] for row in wrong],
                   "same_code_controls": [row[0] for row in controls],
                   "all_original_metadata_exceptions_definitions_and_helpers": "verified",
                   "frozen_inputs": "unchanged", "dependency_provider": args.dependency_provider}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
