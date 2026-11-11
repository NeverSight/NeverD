#!/usr/bin/env python3
"""Verify paired mapper bindings preserve opaque collector results and effects."""
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-paired-mapper-collector-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "paired_mapper_collector"
        originals, harness = work / "originals", work / "harness"
        name = "PairedMapperCollectorHarness"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        sdk_hash, cli_hash = sha(sdk), sha(args.neverd)
        compiler.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=8)
        compiler.compile_local([FIXTURES / "harness" / (name + ".java")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (5, 10):
            raise RuntimeError("Owned paired mapper collector inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        signature = ("<T:Ljava/lang/Object;K:Ljava/lang/Object;V:Ljava/lang/Object;>"
                     "(Ljava/util/function/Function<-TT;+TK;>;Ljava/util/function/Function<-TT;+TV;>;)"
                     "Lfixture/Bucket<TT;*Lfixture/Pair<TK;TV;>;>;")
        identity = "Lfixture/Factory;->fuse(Ljava/util/function/Function;Ljava/util/function/Function;)Lfixture/Bucket;"
        if facts["Lfixture/Factory;"]["methods"][identity]["signature"] != signature:
            raise RuntimeError("Original paired mapper Signature changed")
        providers = all_classes - {"Lfixture/Core;"}
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
                for mode in range(14 if route == 3 else 20):
                    text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), str(mode)],
                                      f"{method}-{route}-{mode}")
                    if text != "attempts=2,paired-mapper-collector=matched\n":
                        raise RuntimeError("Frozen paired mapper collector observer changed")
                    outputs.append(text)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        texts = {p.name: p.read_text() for p in (fixture / "java").rglob("*.java")}
        wrong = [('lost-first', 'Factory.java', 'Trace.left = first.apply((T)value);', 'Trace.left = null;', 0, 0), ('repeat-first', 'Factory.java', 'Trace.left = first.apply((T)value);', 'Trace.left = first.apply((T)value); first.apply((T)value);', 0, 0), ('wrong-first-call', 'Factory.java', 'Trace.left = first.apply((T)value);', 'Trace.left = second.apply((T)value);', 0, 0), ('changed-source', 'Factory.java', 'Object value = Trace.source;', 'Object value = null;', 0, 0), ('reread-source', 'Factory.java', 'second.apply((T)value)', 'second.apply((T)Trace.source)', 0, 17), ('lost-first-result', 'Factory.java', 'Trace.left = first.apply((T)value);', 'first.apply((T)value);', 0, 0), ('lost-action', 'Factory.java', 'Trace.action.run();', ';', 0, 0), ('repeat-action', 'Factory.java', 'Trace.action.run();', 'Trace.action.run(); Trace.action.run();', 0, 0), ('assumed-result', 'Factory.java', 'return (Bucket<T, ?, Pair<K, V>>)Trace.result;', 'return null;', 0, 0), ('early-result', 'Factory.java', 'Trace.action.run();', 'Object saved = Trace.result; Trace.action.run(); Trace.result = saved;', 0, 15), ('lost-target', 'Factory.java', 'Trace.targets++;', ';', 0, 0), ('wrong-mapper-reference', 'Factory.java', 'Trace.first = first;', 'Trace.first = second;', 0, 0), ('lost-runtime-failure', 'Factory.java', 'Trace.left = first.apply((T)value);', 'try { Trace.left = first.apply((T)value); } catch (RuntimeException ignored) {}', 0, 5), ('replaced-error', 'Factory.java', 'Trace.right = second.apply((T)value);', 'try { Trace.right = second.apply((T)value); } catch (Error caught) { throw new AssertionError(caught); }', 0, 8), ('lost-ignored-call', 'Core.java', '        Factory.fuse(first, second);', '        ;', 2, 0), ('lost-checked-value', 'Core.java', 'return (Pair)Factory.<Object, String, Integer>fuse(first, second).value;', 'Factory.<Object, String, Integer>fuse(first, second); return null;', 1, 0), ('lost-return-cast', 'Factory.java', 'return (Bucket<T, ?, Pair<K, V>>)Trace.result;', 'return Trace.result instanceof Bucket ? (Bucket)Trace.result : null;', 2, 13), ('lost-standard-factory', 'Core.java', 'return Collectors.toMap(first, second);', 'return null;', 3, 1), ('swapped-standard-mappers', 'Core.java', 'return Collectors.toMap(first, second);', 'return Collectors.toMap(second, first);', 3, 13), ('eager-standard-callback', 'Core.java', 'return Collectors.toMap(first, second);', 'first.apply(null); return Collectors.toMap(first, second);', 3, 1)]
        for method, filename, anchor, replacement, route, mode in wrong:
            text = texts[filename]
            if anchor not in text:
                raise RuntimeError("Paired mapper collector counterexample anchor changed: " + method)
            source = work / method / filename
            source.parent.mkdir()
            source.open("x").write(text.replace(anchor, replacement))
            compiled = work / (method + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), name, str(route), str(mode)],
                                    text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (method + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned paired mapper collector mismatch" not in result.stderr:
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
            raise RuntimeError("D8 changed original paired mapper collector definitions")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Paired mapper collector recovery changed definitions or generic metadata")
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        allowed = {o + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt = work / "rebuilt"
        dependencies = [dependency] if excluded else []
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=8,
                             classpath=os.pathsep.join(map(str, dependencies)) if dependencies else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if (actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed
                or any(actual_methods[m] != all_methods[m] for m in methods)):
            raise RuntimeError("Paired mapper collector recovery lost or invented runtime definitions")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(facts[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if actual != wanted or any(after_exceptions[m] != exceptions[m] for m in facts[owner]["methods"]):
                raise RuntimeError("Paired mapper collector metadata/roles/Exceptions changed: " + owner)
        if execute((rebuilt, harness, *dependencies), "rebuilt") != baseline:
            raise RuntimeError("Paired mapper collector identity, original casts, arrays or effects changed")
        before = signature.encode()
        controls = [
            ("different-mapper-input", "Factory", before, before.replace(b"Function<-TT;+TV;>", b"Function<-TK;+TV;>")),
            ("swapped-result-pair", "Factory", before, before.replace(b"Pair<TK;TV;>", b"Pair<TV;TK;>")),
            ("upper-mapper-input", "Factory", before, before.replace(b"Function<-TT;+TK;>", b"Function<+TT;+TK;>")),
            ("lower-mapper-output", "Factory", before, before.replace(b"+TV;", b"-TV;")),
            ("closed-accumulator", "Factory", before, before.replace(b"Bucket<TT;*", b"Bucket<TT;Ljava/lang/Object;")),
            ("variant-inner-result", "Factory", before, before.replace(b"Pair<TK;TV;>", b"Pair<+TK;TV;>")),
            ("unknown-output-formal", "Factory", before, before.replace(b"+TK;", b"+TMissing;")),
            ("changed-method-bound", "Factory", before, before.replace(b"V:Ljava/lang/Object;", b"V:Ljava/lang/Number;")),
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
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Paired mapper collector proof admitted unsupported metadata: " + method)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(sdk) != sdk_hash
                or sha(dependency) != dependency_hash or sha(dex) != dex_hash or sha(args.neverd) != cli_hash):
            raise RuntimeError("Frozen original program, observer, SDK or CLI changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": 74, "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "same_code_counterexamples": [r[0] for r in controls], "dependency_provider": args.dependency_provider,
                   "metadata_roles_exceptions_and_runtime_definitions": "unchanged", "minimum_java_release": 8}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
