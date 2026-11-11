#!/usr/bin/env python3
"""Verify original arrays join the complete mapper and invariant result binding."""
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-array-mapped-carrier-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "array_mapped_carrier"
        originals, harness = work / "originals", work / "harness"
        name = "ArrayMappedCarrierHarness"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        sdk_hash, cli_hash = sha(sdk), sha(args.neverd)
        compiler.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=8)
        compiler.compile_local([FIXTURES / "harness" / (name + ".java")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (5, 13):
            raise RuntimeError("Owned array mapped carrier inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        signature = "<T:Ljava/lang/Object;R:Ljava/lang/Object;>([TT;Ljava/util/function/Function<-TT;+TR;>;)Ljava/util/Set<TR;>;"
        identity = "Lfixture/Provider;->read([Ljava/lang/Object;Ljava/util/function/Function;)Ljava/util/Set;"
        if facts["Lfixture/Provider;"]["methods"][identity]["signature"] != signature:
            raise RuntimeError("Original paired array Signature changed")
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
            for route in range(6):
                for mode in range(22):
                    text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), str(mode)],
                                      f"{method}-{route}-{mode}")
                    if text != "attempts=2,array-mapped-carrier=matched\n":
                        raise RuntimeError("Frozen array mapped carrier observer changed")
                    outputs.append(text)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        texts = {p.name: p.read_text() for p in (fixture / "java").rglob("*.java")}
        wrong = [('lost-call', 'Core.java', 'return Provider.read(source,mapper);', 'return null;', 0, 0), ('repeat-call', 'Core.java', 'return Provider.read(source,mapper);', 'Provider.read(source,mapper); return Provider.read(source,mapper);', 0, 0), ('copied-array', 'Core.java', 'return Provider.read(source,mapper);', 'return Provider.read(source.clone(),mapper);', 0, 0), ('packed-array', 'Core.java', 'public static Set<Object> raw(Object[] source, Function<Object,Object> mapper) { return Provider.read(source,mapper); }', 'public static Set<Object> raw(Object[] source, Function<Object,Object> mapper) { return Provider.read(new Object[]{source},mapper); }', 0, 0), ('lost-ignored-call', 'Core.java', '{ Provider.read(source,mapper); return Trace.marker; }', '{ return Trace.marker; }', 2, 0), ('lost-read', 'Provider.java', 'T item = source[Trace.index];', 'T item = null;', 0, 0), ('lost-item-snapshot', 'Provider.java', 'Trace.item = item;', 'Trace.item = null;', 0, 0), ('wrong-source-reference', 'Provider.java', 'Trace.source = source;', 'Trace.source = null;', 0, 0), ('wrong-mapper-reference', 'Provider.java', 'Trace.mapper = mapper;', 'Trace.mapper = null;', 0, 0), ('lost-mapper', 'Provider.java', 'R mapped = mapper.apply(item);', 'R mapped = null;', 0, 0), ('repeated-mapper', 'Provider.java', 'R mapped = mapper.apply(item);', 'mapper.apply(item); R mapped = mapper.apply(item);', 0, 0), ('wrong-mapper-argument', 'Provider.java', 'mapper.apply(item)', 'mapper.apply(null)', 0, 0), ('lost-action', 'Provider.java', 'Trace.action.run();', ';', 0, 0), ('repeated-action', 'Provider.java', 'Trace.action.run();', 'Trace.action.run(); Trace.action.run();', 0, 0), ('early-result', 'Provider.java', 'Trace.action.run();', 'Object early = Trace.value; Trace.action.run(); Trace.value = early;', 0, 12), ('reread-store', 'Provider.java', 'source[Trace.index] = (T)saved;', 'source[Trace.index] = (T)Trace.store;', 0, 19), ('reread-array', 'Provider.java', 'source[Trace.index] = (T)saved;', 'Trace.source[Trace.index] = saved;', 0, 17), ('early-index', 'Provider.java', 'Object saved = Trace.store;\n        Trace.action.run();\n        source[Trace.index] = (T)saved;', 'Object saved = Trace.store; int savedIndex = Trace.index;\n        Trace.action.run();\n        source[savedIndex] = (T)saved;', 0, 17), ('lost-store', 'Provider.java', 'source[Trace.index] = (T)saved;', ';', 0, 0), ('wrapped-runtime', 'Provider.java', 'R mapped = mapper.apply(item);', 'R mapped; try { mapped = mapper.apply(item); } catch (RuntimeException caught) { throw new IllegalStateException(caught); }', 0, 9), ('replaced-error', 'Provider.java', 'R mapped = mapper.apply(item);', 'R mapped; try { mapped = mapper.apply(item); } catch (Error caught) { throw new AssertionError(caught); }', 0, 10), ('lost-result-cast', 'Provider.java', 'return (Set<R>)Trace.value;', 'return Trace.value instanceof Set ? (Set<R>)Trace.value : null;', 2, 1), ('lost-relay', 'Provider.java', 'Trace.relays++;', ';', 3, 0), ('wrong-ignored-result', 'Core.java', 'return Trace.marker;', 'return null;', 2, 0)]
        for method, filename, anchor, replacement, route, mode in wrong:
            text = texts[filename]
            if anchor not in text:
                raise RuntimeError("Array mapped carrier counterexample anchor changed: " + method)
            source = work / method / filename
            source.parent.mkdir()
            source.open("x").write(text.replace(anchor, replacement))
            compiled = work / (method + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), name, str(route), str(mode)],
                                    text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (method + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned array mapped carrier mismatch" not in result.stderr:
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
            raise RuntimeError("D8 changed original array mapped carrier definitions")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Array mapped carrier recovery changed definitions or generic metadata")
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        allowed = {o + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt = work / "rebuilt"
        dependencies = [dependency] if excluded else []
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=8,
                             classpath=os.pathsep.join(map(str, dependencies)) if dependencies else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if (actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed
                or any(actual_methods[m] != all_methods[m] for m in methods)):
            raise RuntimeError("Array mapped carrier recovery lost or invented runtime definitions")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(facts[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if actual != wanted or any(after_exceptions[m] != exceptions[m] for m in facts[owner]["methods"]):
                raise RuntimeError("Array mapped carrier metadata/roles/Exceptions changed: " + owner)
        if execute((rebuilt, harness, *dependencies), "rebuilt") != baseline:
            raise RuntimeError("Array mapped carrier identity, original casts, arrays or effects changed")
        before = signature.encode()
        controls = [
            ("extra-formal", "Provider", before, before.replace(b"<T:", b"<Unused:Ljava/lang/Object;T:")),
            ("changed-method-bound", "Provider", before, before.replace(b"T:Ljava/lang/Object;", b"T:Ljava/lang/Number;")),
            ("unknown-array-formal", "Provider", before, before.replace(b"[TT;", b"[TMissing;")),
            ("changed-array-rank", "Provider", before, before.replace(b"[TT;", b"[[TT;")),
            ("crossed-mapper-directions", "Provider", before, before.replace(b"Function<-TT;+TR;>", b"Function<+TT;-TR;>")),
            ("repeated-mapper-formal", "Provider", before, before.replace(b"Function<-TT;+TR;>", b"Function<-TT;+TT;>")),
            ("crossed-result-formal", "Provider", before, before.replace(b"Set<TR;>", b"Set<TT;>")),
            ("unknown-result-formal", "Provider", before, before.replace(b"Set<TR;>", b"Set<TMissing;>")),
            ("incomplete-mapper", "Provider", before, before.replace(b"Function<-TT;+TR;>", b"Function<-TT;>")),
            ("generic-throws", "Provider", before, before + b"^TT;"),
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
                raise RuntimeError("Array mapped carrier proof admitted unsupported metadata: " + method)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(sdk) != sdk_hash
                or sha(dependency) != dependency_hash or sha(dex) != dex_hash or sha(args.neverd) != cli_hash):
            raise RuntimeError("Frozen original program, observer, SDK or CLI changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": 132, "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "same_code_counterexamples": [r[0] for r in controls], "dependency_provider": args.dependency_provider,
                   "metadata_roles_exceptions_and_runtime_definitions": "unchanged", "minimum_java_release": 8}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
