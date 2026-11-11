#!/usr/bin/env python3
"""Verify exact nominal constructor inputs against one declared generic bound."""
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "java-home", "compiler-java-home"):
        parser.add_argument("--" + name, required=True, type=Path)
    for name in ("d8", "work-dir", "baseline-neverd", "fixture-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-bound-nominal-super-")
    with context as directory:
        work = Path(directory).resolve()
        v = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = args.fixture_dir or FIXTURES / "bound_nominal_super_results"
        harness_root = fixture / "harness" if args.fixture_dir else FIXTURES / "harness"
        originals, harness = work / "originals", work / "harness"
        name = "BoundNominalSuperHarness"
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        compiler.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=8)
        compiler.compile_local([harness_root / (n + ".java") for n in (name, "BoundNominalSuperCaller")], harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness), "sdk": sha(sdk), "cli": sha(args.neverd)}
        (work / "frozen-input-hashes.json").write_text(json.dumps(frozen, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(compiler, originals)
        if len(all_classes) != 4:
            raise RuntimeError("Owned bound nominal super class inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        providers = {"Lfixture/Parent;", "Lfixture/Provider;", "Lfixture/Trace;"}
        classes = all_classes - providers if args.dependency_provider else all_classes
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        provider = work / "owned-provider.jar"
        with zipfile.ZipFile(provider, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for owner in sorted(providers):
                entry = owner[1:-1] + ".class"
                archive.writestr(entry, (originals / entry).read_bytes())
        provider_hash = sha(provider)

        def execute(runtime, label):
            results = []
            for route in range(4):
                for operation in range(2):
                    for mode in range(14):
                        output = v.run([compiler.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), str(operation), str(mode)], f"{label}-{route}-{operation}-{mode}")
                        if len(output.splitlines()) != 2 or any(not line.startswith(f"route={route},operation={operation},mode={mode},attempt={i},trace=") for i, line in enumerate(output.splitlines())):
                            raise RuntimeError("Frozen bound nominal super observer output changed")
                        results.append(output)
            return results

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").write_text(json.dumps(baseline, indent=2) + "\n")
        wrong = [
            ("skip-factory", "Core.java", "Provider.take(first)", "(Integer)null", 0),
            ("repeat-factory", "Provider.java", "++Trace.calls", "(Trace.calls += 2)", 0),
            ("swapped-results", "Provider.java", "n == 1 ? Trace.first : Trace.second", "n == 1 ? Trace.second : Trace.first", 0),
            ("lost-field", "Parent.java", "this.first = first;", "this.first = null;", 0),
            ("copied-identity", "Parent.java", "this.first = first;", "this.first = (T)new Integer(first.intValue());", 0),
            ("skip-action", "Parent.java", "Trace.action.run();", ";", 0),
            ("wrapped-failure", "Parent.java", "Trace.action.run();", "try { Trace.action.run(); } catch(RuntimeException e) { throw new IllegalArgumentException(e); }", 8),
            ("early-cast", "Parent.java", "Trace.hit(5);", "Integer ignored = (Integer)this.first; Trace.hit(5);", 12),
            ("reread-result", "Parent.java", "Trace.hit(5);", "this.first = (T)Trace.first; Trace.hit(5);", 13),
        ]
        for label, filename, before, after, mode in wrong:
            original_text = (fixture / "java/fixture" / filename).read_text()
            if original_text.count(before) != 1:
                raise RuntimeError("Owned wrong-program anchor changed")
            source = work / label / filename
            source.parent.mkdir()
            source.write_text(original_text.replace(before, after))
            compiled = work / (label + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            p = subprocess.run([compiler.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), name, "1", "0", str(mode)], capture_output=True, text=True, timeout=1200, env=v.environment)
            (work / (label + "-counterexample.log")).write_text(p.stdout + p.stderr)
            if p.returncode == 0 or "owned bound nominal super mismatch" not in p.stderr:
                raise RuntimeError("Frozen observer missed wrong program: " + label)
        cp = ["--classpath", sdk]
        if args.dependency_provider:
            cp += ["--classpath", provider]
        dexdir = work / "dex"
        dexdir.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        v.run([*v.d8, "--release", "--min-api", "24", *cp, "--output", dexdir, *paths], "d8-original")
        dex = dexdir / "classes.dex"
        dex_hash = sha(dex)
        dump = v.run([v.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original bound nominal super definitions")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        if args.baseline_neverd:
            red = subprocess.run([args.baseline_neverd.resolve(), "mobile", dex, *cp, "--timeout", "600", "-o", work / "before"], text=True, capture_output=True, env=v.environment, timeout=1200)
            (work / "before.log").write_text(red.stdout + red.stderr)
            if red.returncode == 0 or "constructor overload requires a proven generic parameter binding" not in red.stdout + red.stderr:
                raise RuntimeError("Expected the prior CLI's bounded constructor rejection")
        output = work / "recovered"
        v.run([args.neverd.resolve(), "mobile", dex, *cp, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        release = coverage.get("minimum_java_release", 8)
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("field_source_bindings")
                or coverage.get("requires_java_rebuild_postprocessing") or release != 25):
            raise RuntimeError("Bound nominal super recovery changed original definitions or generic metadata")
        declarations = {m for owner in classes for m, row in facts[owner]["methods"].items() if row["access"] & 0x500}
        if ({m["identity"] for m in coverage["methods"] if m["status"] == "declaration-only"} != declarations
                or coverage["declaration_only_method_count"] != len(declarations)
                or any(m["status"] not in ("recovered", "declaration-only") for m in coverage["methods"])
                or any(m["instruction_count"] for m in coverage["methods"] if m["status"] == "declaration-only")):
            raise RuntimeError("Bound nominal super Code/declaration roles changed")
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        allowed = {o + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt = work / "rebuilt"
        deps = [provider] if args.dependency_provider else []
        compiler.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=release, classpath=os.pathsep.join(map(str, deps)) if deps else None)
        actual_classes, actual_methods = compiler_inventory(compiler, rebuilt)
        if actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed or any(actual_methods[m] != all_methods[m] for m in methods):
            raise RuntimeError("Bound nominal super recovery invented or omitted definitions/roles")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(facts[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if wanted.pop("major") != 52 or actual.pop("major") != 44 + release:
                raise RuntimeError("Owned compiler release changed")
            if actual != wanted or any(after_exceptions[m] != exceptions[m] for m in facts[owner]["methods"]):
                raise RuntimeError("Bound nominal super original metadata/Exceptions changed: " + owner)
        if execute((rebuilt, harness, *deps), "rebuilt") != baseline:
            raise RuntimeError("Bound nominal super result, effects or exception identity changed")
        parent_signature = "<T:Ljava/lang/Number;>Ljava/lang/Object;"
        constructor_signature = "(TT;TT;J)V"
        child_signature = "Lfixture/Parent<Ljava/lang/Integer;>;"
        controls = [
            ("erased-bound", "Parent", parent_signature, parent_signature.replace("Number", "Object")),
            ("missing-bound", "Parent", parent_signature, parent_signature.replace("Ljava/lang/Number;", "Lfixture/Missing;")),
            ("unbound-formal", "Parent", constructor_signature, "(TMissing;TT;J)V"),
            ("shadowed-formal", "Parent", constructor_signature, "<T:Ljava/lang/Number;>" + constructor_signature),
            ("wrong-array-rank", "Parent", constructor_signature, "([TT;TT;J)V"),
            ("wrong-closed-bound", "Core", child_signature, child_signature.replace("Integer", "String")),
        ]
        for label, owner, signature, changed in controls:
            root = work / (label + "-originals")
            shutil.copytree(originals, root)
            target = root / "fixture" / (owner + ".class")
            code_before = method_code(target.read_bytes())
            receipt = change_signature(target, signature.encode(), changed.encode())
            if method_code(target.read_bytes()) != code_before:
                raise RuntimeError("Signature control changed original Code")
            receipt["target_class"] = "Lfixture/" + owner + ";"
            receipt["Code"] = "unchanged"
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            countercp = ["--classpath", sdk]
            if args.dependency_provider:
                counterprovider = work / (label + "-provider.jar")
                with zipfile.ZipFile(counterprovider, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                    for provider_owner in sorted(providers):
                        entry = provider_owner[1:-1] + ".class"
                        archive.writestr(entry, (root / entry).read_bytes())
                countercp += ["--classpath", counterprovider]
            counterdex = work / (label + "-dex")
            counterdex.mkdir()
            counterpaths = [root / p.relative_to(originals) for p in paths]
            v.run([*v.d8, "--release", "--min-api", "24", *countercp, "--output", counterdex, *counterpaths], label + "-d8")
            rejected = subprocess.run([args.neverd.resolve(), "mobile", counterdex / "classes.dex", *countercp, "--timeout", "600", "-o", work / (label + "-recovered")], text=True, capture_output=True, timeout=1200, env=v.environment)
            (work / (label + "-reject.log")).write_text(rejected.stdout + rejected.stderr)
            if rejected.returncode == 0 or "error" not in rejected.stdout + rejected.stderr:
                raise RuntimeError("Accepted malformed/unproved bound nominal constructor Signature: " + label)
        if frozen != {"originals": hashes(originals), "harness": hashes(harness), "sdk": sha(sdk), "cli": sha(args.neverd)} or sha(dex) != dex_hash or sha(provider) != provider_hash:
            raise RuntimeError("Frozen bound nominal super input changed")
        summary = {"status": "verified", "classes": len(classes), "methods": len(methods),
                   "all_java_units": len(list((output / "sources").rglob("*.java"))),
                   "fresh_jvm_cases": 112, "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "signature_controls": len(controls), "minimum_java_release": release,
                   "dependency_provider": args.dependency_provider}
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
