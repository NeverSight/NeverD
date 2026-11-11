#!/usr/bin/env python3
"""Verify generic interface finals retain closed source types and initialization effects."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from scripts.test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "java-home", "compiler-java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-generic-interface-final-field-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "generic_interface_final_field"
        originals, harness = work / "originals", work / "harness"
        name = "GenericInterfaceFinalFieldHarness"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        sdk_hash, cli_hash = sha(sdk), sha(args.neverd)
        compiler.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=8)
        compiler.compile_local([FIXTURES / "harness" / (name + ".java")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (4, 7):
            raise RuntimeError("Owned generic interface final field inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        signature = "Lfixture/Box<Ljava/lang/String;>;"
        identity = "Lfixture/Api;->Z_FIRST:Lfixture/Box;"
        if facts["Lfixture/Api;"]["fields"][identity]["signature"] != signature:
            raise RuntimeError("Original generic interface field Signature changed")
        providers = all_classes - {"Lfixture/Api;"}
        excluded = providers if args.dependency_provider else set()
        classes = all_classes - excluded
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        dependency = work / "owned-provider.jar"

        def write_provider(destination, source):
            with zipfile.ZipFile(destination, "x", compression=zipfile.ZIP_DEFLATED) as jar:
                for owner in sorted(providers):
                    entry = owner[1:-1] + ".class"
                    jar.writestr(entry, (source / entry).read_bytes())

        write_provider(dependency, originals)
        dependency_hash = sha(dependency)

        def execute(runtime, method):
            outputs = []
            for route in range(4):
                for mode in range(16):
                    text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), str(mode)],
                                      f"{method}-{route}-{mode}")
                    if text != "attempts=2,generic-interface-final-field=matched\n":
                        raise RuntimeError("Frozen generic interface final field observer changed")
                    outputs.append(text)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        texts = {p.name: p.read_text() for p in (fixture / "java").rglob("*.java")}
        wrong = [('lost-first', 'Api.java', 'Box<String> Z_FIRST = Provider.first();', 'Box<String> Z_FIRST = null;', 0, 0), ('lost-next', 'Api.java', 'A_SECOND = Provider.next(Trace.first);', 'A_SECOND = null;', 0, 0), ('lost-self', 'Api.java', 'SELF = Provider.self(Trace.second);', 'SELF = null;', 0, 0), ('wrong-first-input', 'Api.java', 'Provider.next(Trace.first)', 'Provider.next(null)', 0, 0), ('wrong-second-input', 'Api.java', 'Provider.self(Trace.second)', 'Provider.self(null)', 0, 0), ('changed-first-result', 'Provider.java', 'return Trace.first;', 'return Trace.second;', 0, 0), ('changed-second-result', 'Provider.java', 'return Trace.second;', 'return Trace.first;', 0, 0), ('changed-third-result', 'Provider.java', 'return Trace.third;', 'return Trace.second;', 0, 0), ('lost-callback', 'Provider.java', 'Trace.action.run();', ';', 0, 0), ('repeat-callback', 'Provider.java', 'Trace.action.run();', 'Trace.action.run(); Trace.action.run();', 0, 0), ('lost-runtime-failure', 'Provider.java', 'Trace.action.run();', 'try { Trace.action.run(); } catch (RuntimeException ignored) {}', 0, 1), ('replaced-error', 'Provider.java', 'Trace.action.run();', 'try { Trace.action.run(); } catch (Error caught) { throw new AssertionError(caught); }', 0, 4), ('wrong-first-observation', 'Provider.java', 'Trace.reFirst = Api.Z_FIRST;', 'Trace.reFirst = Trace.first;', 0, 0), ('wrong-next-observation', 'Provider.java', 'Trace.nextFirst = Api.Z_FIRST;', 'Trace.nextFirst = null;', 0, 0), ('wrong-previous-snapshot', 'Provider.java', 'Trace.inputFirst = previous;', 'Trace.inputFirst = Trace.second;', 0, 0), ('wrong-event-order', 'Provider.java', 'Trace.log * 10 + 2', 'Trace.log * 10 + 3', 0, 0), ('early-stage', 'Provider.java', 'Trace.stage = 1;', 'Trace.stage = 2;', 0, 0), ('changed-box-contents', 'Box.java', 'this.value = value;', 'this.value = null;', 0, 0), ('assumed-element-cast', 'Provider.java', 'return Trace.first;', 'return new Box<String>((String)Trace.first.value);', 0, 0), ('swallowed-null-action', 'Provider.java', 'Trace.action.run();', 'if (Trace.action != null) Trace.action.run();', 0, 7)]
        for method, filename, anchor, replacement, route, mode in wrong:
            text = texts[filename]
            if anchor not in text:
                raise RuntimeError("Generic interface final field counterexample anchor changed: " + method)
            source = work / method / filename
            source.parent.mkdir()
            source.open("x").write(text.replace(anchor, replacement))
            compiled = work / (method + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), name, str(route), str(mode)],
                                    text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (method + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned generic interface final field mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program: " + method)
        classpath = ["--classpath", sdk]
        if excluded:
            classpath += ["--classpath", dependency]
        dexdir = work / "dex"
        dexdir.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--release", "--min-api", "24", *classpath, "--output", dexdir, *paths], "d8-original")
        dex = dexdir / "classes.dex"
        dex_hash = sha(dex)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original generic interface final field definitions")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Generic interface final field recovery changed definitions or generic metadata")
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        allowed = {o + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt = work / "rebuilt"
        dependencies = [dependency] if excluded else []
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=8,
                             classpath=os.pathsep.join(map(str, dependencies)) if dependencies else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if (actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed
                or any(actual_methods[m] != all_methods[m] for m in methods)):
            raise RuntimeError("Generic interface final field recovery lost or invented runtime definitions")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(facts[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if actual != wanted or any(after_exceptions[m] != exceptions[m] for m in facts[owner]["methods"]):
                raise RuntimeError("Generic interface final field metadata/roles/Exceptions changed: " + owner)
        if execute((rebuilt, harness, *dependencies), "rebuilt") != baseline:
            raise RuntimeError("Generic interface final field identity, original casts, arrays or effects changed")
        before = signature.encode()
        controls = [
            ("changed-field-erasure", "Api", before, b"Ljava/util/List<Ljava/lang/String;>;"),
            ("missing-field-element", "Api", before, before.replace(b"Ljava/lang/String;", b"Lfixture/Missing;")),
            ("unknown-field-formal", "Api", before, before.replace(b"Ljava/lang/String;", b"TMissing;")),
            ("extra-carrier-slot", "Api", before, before.replace(b"Ljava/lang/String;", b"Ljava/lang/String;Ljava/lang/Object;")),
            ("array-field-erasure", "Api", before, b"[" + before),
            ("raw-signature-attribute", "Api", before, b"Lfixture/Box;"),
            ("changed-carrier-bound", "Box", b"<R:Ljava/lang/Object;>Ljava/lang/Object;", b"<R:Ljava/lang/Number;>Ljava/lang/Object;"),
            ("missing-nested-field-element", "Api", before, b"Lfixture/Box<Lfixture/Box<Lfixture/Missing;>;>;"),
        ]
        for method, owner, before, changed in controls:
            negative = work / (method + "-classes")
            shutil.copytree(originals, negative)
            target = negative / ("fixture/" + owner + ".class")
            original_code = method_code(target.read_bytes())
            receipt = change_signature(target, before, changed)
            if method_code(target.read_bytes()) != original_code:
                raise RuntimeError("Signature-only control changed Code or runtime descriptors")
            (work / (method + "-metadata.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            declared = classpath
            if excluded:
                changed_dependency = work / (method + "-provider.jar")
                write_provider(changed_dependency, negative)
                declared = ["--classpath", sdk, "--classpath", changed_dependency]
            encoded = work / (method + "-dex")
            encoded.mkdir()
            verify.run([*verify.d8, "--release", "--min-api", "24", *declared, "--output", encoded,
                        *[negative / p.relative_to(originals) for p in paths]], "d8-" + method)
            rejected = work / (method + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(encoded / "classes.dex"),
                                     *map(str, declared), "-o", str(rejected)], text=True, capture_output=True,
                                    timeout=1200, env=verify.environment)
            (work / (method + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or not any(marker in result.stderr for marker in ("generic", "interface initializer requires one write")):
                raise RuntimeError("Generic interface final field proof admitted unsupported metadata: " + method)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(sdk) != sdk_hash
                or sha(dependency) != dependency_hash or sha(dex) != dex_hash or sha(args.neverd) != cli_hash):
            raise RuntimeError("Frozen original program, observer, SDK or CLI changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": 64, "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "same_code_counterexamples": [r[0] for r in controls], "dependency_provider": args.dependency_provider,
                   "metadata_roles_exceptions_and_runtime_definitions": "unchanged", "minimum_java_release": 8}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
