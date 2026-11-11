#!/usr/bin/env python3
"""Verify recursive carrier inputs preserve Comparable erasure and opaque bounded results."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-comparable-carrier-result-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "comparable_carrier_result"
        originals, harness = work / "originals", work / "harness"
        name = "fixture.ComparableCarrierResultHarness"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        sdk_hash, cli_hash = sha(sdk), sha(args.neverd)
        compiler.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=8)
        compiler.compile_local([FIXTURES / "harness" / (n + ".java") for n in ("ComparableCarrierResultHarness", "ComparableCarrierResultCaller")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (3, 8):
            raise RuntimeError("Owned comparable carrier result inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        providers = {"Lfixture/Provider;"}
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
                for branch in range(3):
                    for mode in range(16):
                        text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), str(branch), str(mode)],
                                          f"{method}-{route}-{branch}-{mode}")
                        if text != "attempts=2,comparable-carrier-result=matched\n":
                            raise RuntimeError("Frozen comparable carrier result observer changed")
                        outputs.append(text)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        text = (fixture / "java/fixture/Core.java").read_text()
        wrong = [('lost-call', 'Core.java', 'return Provider.select(values);', 'return null;', 0, 0, 0), ('repeated-call', 'Core.java', 'return Provider.select(values);', 'Provider.select(values); return Provider.select(values);', 1, 0, 0), ('changed-input', 'Core.java', 'Provider.select(values)', 'Provider.select((Collection<Value>)null)', 0, 0, 0), ('copied-collection', 'Core.java', 'Provider.select(values)', 'Provider.select(new java.util.ArrayList<Value>(values))', 0, 0, 0), ('eager-probe', 'Core.java', 'return Provider.select(values);', 'values.size(); return Provider.select(values);', 0, 0, 0), ('substituted-result', 'Core.java', 'return Provider.select(values);', 'Provider.select(values); return null;', 0, 0, 0), ('lost-callback', 'Provider.java', 'Trace.action.run();', ';', 0, 0, 0), ('wrapped-failure', 'Provider.java', 'Trace.action.run();', 'try { Trace.action.run(); } catch(Throwable failure) { throw new IllegalArgumentException(failure); }', 0, 0, 8), ('early-result-read', 'Provider.java', 'Trace.action.run();', 'Object saved=Trace.result; Trace.action.run(); Trace.result=saved;', 0, 0, 0), ('swallowed-bounded-cast', 'Provider.java', 'return (Entry)Trace.result;', 'try { return (Entry)Trace.result; } catch(ClassCastException failure) { return null; }', 0, 0, 10), ('lost-final-stage', 'Provider.java', 'Trace.hit(2);', ';', 0, 0, 0), ('eager-comparison', 'Provider.java', 'return (Entry)Trace.result;', '((Comparable)Trace.result).compareTo(Trace.result); return (Entry)Trace.result;', 0, 0, 0), ('lost-input-cast', 'Core.java', 'return call((Collection)values);', 'return null;', 0, 1, 4), ('lost-narrow-cast', 'Core.java', 'return call(values);', 'call(values); return null;', 0, 2, 11), ('new-null-validation', 'Provider.java', 'Trace.input=values;', 'if(values==null) throw new NullPointerException(); Trace.input=values;', 0, 0, 1)]
        for label, filename, anchor, replacement, route, branch, mode in wrong:
            text = (fixture / "java/fixture" / filename).read_text()
            if text.count(anchor) != 1:
                raise RuntimeError("Comparable carrier result counterexample anchor changed: " + label)
            source = work / label / filename
            source.parent.mkdir()
            source.open("x").write(text.replace(anchor, replacement))
            compiled = work / (label + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), name, str(route), str(branch), str(mode)],
                                    text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned comparable carrier result mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program: " + label)
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
            raise RuntimeError("D8 changed original comparable carrier result definitions")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        minimum_java_release = coverage.get("minimum_java_release", 8)
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("field_source_bindings") or coverage.get("requires_java_rebuild_postprocessing") or minimum_java_release != 8):
            raise RuntimeError("Comparable carrier result recovery changed definitions or generic metadata")
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        allowed = {o + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt = work / "rebuilt"
        dependencies = [dependency] if excluded else []
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=minimum_java_release,
                             classpath=os.pathsep.join(map(str, dependencies)) if dependencies else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if (actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed
                or any(actual_methods[m] != all_methods[m] for m in methods)):
            raise RuntimeError("Comparable carrier result recovery lost or invented runtime definitions")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(facts[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if wanted.pop("major") != 52 or actual.pop("major") != 44 + minimum_java_release:
                raise RuntimeError("Comparable carrier result compiler release changed")
            if actual != wanted or any(after_exceptions[m] != exceptions[m] for m in facts[owner]["methods"]):
                raise RuntimeError("Comparable carrier result metadata/roles/Exceptions changed: " + owner)
        if execute((rebuilt, harness, *dependencies), "rebuilt") != baseline:
            raise RuntimeError("Comparable carrier result input and result array identities, mapper scope, original effects or exceptions changed")
        short_signature = "<Entry::Ljava/lang/Comparable<-TEntry;>;>(Ljava/util/Collection<TEntry;>;)TEntry;"
        controls = [
            ("invariant-recursion", short_signature.replace("<-TEntry;>", "<TEntry;>")),
            ("covariant-recursion", short_signature.replace("<-TEntry;>", "<+TEntry;>")),
            ("foreign-recursive-formal", short_signature.replace("<-TEntry;>", "<-TMissing;>")),
            ("raw-carrier", short_signature.replace("Collection<TEntry;>", "Collection")),
            ("contravariant-carrier", short_signature.replace("Collection<TEntry;>", "Collection<-TEntry;>")),
            ("foreign-input-formal", short_signature.replace("Collection<TEntry;>", "Collection<TMissing;>")),
            ("foreign-result-formal", short_signature.replace(")TEntry;", ")TMissing;")),
            ("extra-formal", short_signature.replace("<Entry:", "<Unused:Ljava/lang/Object;Entry:")),
        ]
        for label, changed_signature in controls:
            changed_originals = work / (label + "-originals")
            shutil.copytree(originals, changed_originals)
            target = changed_originals / "fixture/Provider.class"
            receipt = change_signature(target, short_signature.encode(), changed_signature.encode())
            receipt["target_identity"] = "Lfixture/Provider;->select(Ljava/util/Collection;)Ljava/lang/Comparable;"
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            counter_classpath = ["--classpath", sdk]
            if excluded:
                counter_provider = work / (label + "-provider.jar")
                write_provider(counter_provider, changed_originals)
                counter_classpath += ["--classpath", counter_provider]
            counter_dex = work / (label + "-dex")
            counter_dex.mkdir()
            counter_paths = [p for p in sorted(changed_originals.rglob("*.class")) if "L" + p.relative_to(changed_originals).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--release", "--min-api", "24", *counter_classpath, "--output", counter_dex, *counter_paths], label + "-d8")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(counter_dex / "classes.dex"), *map(str, counter_classpath), "--timeout", "600", "-o", str(work / (label + "-rejected"))], text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (label + "-rejection.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or (work / (label + "-rejected")).exists():
                raise RuntimeError("Unproved comparable carrier result Signature was accepted: " + label)

        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(sdk) != sdk_hash
                or sha(dependency) != dependency_hash or sha(dex) != dex_hash or sha(args.neverd) != cli_hash):
            raise RuntimeError("Frozen original program, observer, SDK or CLI changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "same_code_counterexamples": [r[0] for r in controls], "dependency_provider": args.dependency_provider,
                   "metadata_roles_exceptions_and_runtime_definitions": "unchanged", "minimum_java_release": minimum_java_release}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
