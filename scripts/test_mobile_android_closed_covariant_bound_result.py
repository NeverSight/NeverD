#!/usr/bin/env python3
"""Verify closed covariant return bounds with nominal inputs and checked exceptions."""
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-closed-covariant-bound-")
    with context as directory:
        work = Path(directory).resolve()
        v = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = args.fixture_dir or FIXTURES / "closed_covariant_bound_result"
        harness_root = fixture / "harness" if args.fixture_dir else FIXTURES / "harness"
        originals, harness = work / "originals", work / "harness"
        name = "ClosedCovariantBoundHarness"
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        compiler.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=8)
        compiler.compile_local([harness_root / (n + ".java") for n in (name, "ClosedCovariantBoundCaller")], harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness), "sdk": sha(sdk), "cli": sha(args.neverd)}
        (work / "frozen-input-hashes.json").write_text(json.dumps(frozen, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(compiler, originals)
        if (len(all_classes), len(all_methods)) != (8, 13):
            raise RuntimeError("Owned closed covariant bound class inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        providers = all_classes - {"Lfixture/Core;"}
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
                for operation in range(3):
                    for mode in range(18):
                        output = v.run([compiler.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), str(operation), str(mode)], f"{label}-{route}-{operation}-{mode}")
                        if len(output.splitlines()) != 2 or any(not line.startswith(f"route={route},operation={operation},mode={mode},attempt={i},trace=") for i, line in enumerate(output.splitlines())):
                            raise RuntimeError("Frozen closed covariant bound observer output changed")
                        results.append(output)
            return results

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").write_text(json.dumps(baseline, indent=2) + "\n")
        wrong = [
            ("skip-factory", "Core.java", "provider.make(label, sink)", "Trace.result", 0, 0),
            ("repeat-factory", "Core.java", "Carrier value = provider.make(label, sink);", "provider.make(label, sink); Carrier value = provider.make(label, sink);", 0, 0),
            ("changed-label", "Core.java", "provider.make(label, sink)", "provider.make(null, sink)", 0, 0),
            ("changed-sink", "Core.java", "provider.make(label, sink)", "provider.make(label, null)", 0, 0),
            ("lost-result", "Factory.java", "return (R) Trace.result;", "return null;", 0, 0),
            ("changed-identity", "Factory.java", "return (R) Trace.result;", "return (R) Trace.alternate;", 0, 0),
            ("skip-callback", "Factory.java", "Trace.action.run(label, sink);", ";", 0, 0),
            ("changed-receiver", "Factory.java", "Trace.receiver = this;", "Trace.receiver = null;", 0, 0),
            ("wrapped-checked", "Factory.java", "Trace.action.run(label, sink);", "try { Trace.action.run(label, sink); } catch (java.io.IOException failure) { throw new RuntimeException(failure); }", 0, 5),
            ("lost-cause", "Wrapped.java", "super(cause);", "super((Throwable)null);", 1, 5),
        ]
        for label, filename, before, after, operation, mode in wrong:
            original_text = (fixture / "java/fixture" / filename).read_text()
            if original_text.count(before) != 1:
                raise RuntimeError("Owned wrong-program anchor changed")
            source = work / label / filename
            source.parent.mkdir()
            source.write_text(original_text.replace(before, after))
            compiled = work / (label + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            p = subprocess.run([compiler.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), name, "1", str(operation), str(mode)], capture_output=True, text=True, timeout=1200, env=v.environment)
            (work / (label + "-counterexample.log")).write_text(p.stdout + p.stderr)
            if p.returncode == 0 or "owned closed covariant bound mismatch" not in p.stderr:
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
            raise RuntimeError("D8 changed original closed covariant bound definitions")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        if args.baseline_neverd:
            red = subprocess.run([args.baseline_neverd.resolve(), "mobile", dex, *cp, "--timeout", "600", "-o", work / "before"], text=True, capture_output=True, env=v.environment, timeout=1200)
            (work / "before.log").write_text(red.stdout + red.stderr)
            if red.returncode == 0 or "generic invocation requires a proven erased member binding" not in red.stdout + red.stderr:
                raise RuntimeError("Expected the prior CLI's closed bound invocation rejection")
        output = work / "recovered"
        v.run([args.neverd.resolve(), "mobile", dex, *cp, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        release = coverage.get("minimum_java_release", 8)
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("field_source_bindings")
                or coverage.get("requires_java_rebuild_postprocessing") or release != 8):
            raise RuntimeError("Closed covariant bound recovery changed original definitions or generic metadata")
        declarations = {m for owner in classes for m, row in facts[owner]["methods"].items() if row["access"] & 0x500}
        if ({m["identity"] for m in coverage["methods"] if m["status"] == "declaration-only"} != declarations
                or coverage["declaration_only_method_count"] != len(declarations)
                or any(m["status"] not in ("recovered", "declaration-only") for m in coverage["methods"])
                or any(m["instruction_count"] for m in coverage["methods"] if m["status"] == "declaration-only")):
            raise RuntimeError("Closed covariant bound Code/declaration roles changed")
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        allowed = {o + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt = work / "rebuilt"
        deps = [provider] if args.dependency_provider else []
        compiler.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=release, classpath=os.pathsep.join(map(str, deps)) if deps else None)
        actual_classes, actual_methods = compiler_inventory(compiler, rebuilt)
        if actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed or any(actual_methods[m] != all_methods[m] for m in methods):
            raise RuntimeError("Closed covariant bound recovery invented or omitted definitions/roles")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(facts[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if wanted.pop("major") != 52 or actual.pop("major") != 44 + release:
                raise RuntimeError("Owned compiler release changed")
            if actual != wanted or any(after_exceptions[m] != exceptions[m] for m in facts[owner]["methods"]):
                raise RuntimeError("Closed covariant bound original metadata/Exceptions changed: " + owner)
        if execute((rebuilt, harness, *deps), "rebuilt") != baseline:
            raise RuntimeError("Closed covariant bound result, effects or exception identity changed")
        factory_signature = "<R:Lfixture/Carrier<+Lfixture/Element;>;>(Ljava/lang/String;Ljava/io/OutputStream;)TR;"
        carrier_signature = "<E::Lfixture/Element;>Ljava/io/OutputStream;"
        controls = [
            ("lower-bound", "Factory", factory_signature, factory_signature.replace("+", "-")),
            ("invariant-bound", "Factory", factory_signature, factory_signature.replace("+", "")),
            ("unbound-result", "Factory", factory_signature, factory_signature.replace("TR;", "TX;")),
            ("extra-formal", "Factory", factory_signature, factory_signature.replace(">(", "X:Ljava/lang/Object;>(")),
            ("missing-element", "Carrier", carrier_signature, carrier_signature.replace("Element", "Missing")),
            ("wrong-carrier-arity", "Carrier", carrier_signature, carrier_signature.replace(">Ljava/io/OutputStream;", "F:Ljava/lang/Object;>Ljava/io/OutputStream;")),
            ("wrong-result-erasure", "Factory", factory_signature, factory_signature.replace("TR;", "Ljava/lang/Object;")),
            ("missing-covariant-element", "Factory", factory_signature, factory_signature.replace("Element", "Missing")),
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
                raise RuntimeError("Accepted malformed/unproved closed covariant bound Signature: " + label)
        if frozen != {"originals": hashes(originals), "harness": hashes(harness), "sdk": sha(sdk), "cli": sha(args.neverd)} or sha(dex) != dex_hash or sha(provider) != provider_hash:
            raise RuntimeError("Frozen closed covariant bound input changed")
        summary = {"status": "verified", "classes": len(classes), "methods": len(methods),
                   "all_java_units": len(list((output / "sources").rglob("*.java"))),
                   "fresh_jvm_cases": 216, "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "signature_controls": len(controls), "minimum_java_release": release,
                   "dependency_provider": args.dependency_provider}
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
