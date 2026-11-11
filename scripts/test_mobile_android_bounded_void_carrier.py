#!/usr/bin/env python3
"""Verify nominally bounded void carriers preserve factory and callback effects."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-inputs", action="store_true")
    parser.add_argument("--baseline-only", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-bounded-void-carrier-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "bounded_void_carrier/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (n + ".java") for n in
                              ("BoundedVoidCarrierCaller", "BoundedVoidCarrierHarness")],
                             harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 8 or len(all_methods) != 11:
            raise RuntimeError("Original bounded void carrier inventory changed")
        declarations = {"Lapi/" + n + ";" for n in ("Function", "Bound", "Context", "Unit", "Marker")}
        classes = all_classes - (declarations if args.dependency_inputs else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        target = "Lfixture/Provider;->work(Lapi/Function;Lapi/Marker;Lapi/Function;Lapi/Function;Lapi/Function;Lapi/Marker;II)V"
        signature = ("<T:Lapi/Bound;>(Lapi/Function<-Lapi/Context;+TT;>;Lapi/Marker;"
                     "Lapi/Function<-TT;Lapi/Unit;>;Lapi/Function<-TT;Lapi/Unit;>;"
                     "Lapi/Function<-TT;Lapi/Unit;>;Lapi/Marker;II)V")
        if records["Lfixture/Provider;"]["methods"][target]["signature"] != signature:
            raise RuntimeError("Original nominal bound and callback slot correlations changed")
        dependency = work / "owned-contracts.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for name in sorted(declarations):
                path = name[1:-1] + ".class"
                jar.writestr(path, (originals / path).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_inputs:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            outputs = []
            for route in range(4):
                for mode in range(24):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                         "BoundedVoidCarrierHarness", str(route), str(mode)], f"{label}-{route}-{mode}")
                    pattern = f"route={route},mode={mode},attempt=[01],trace=-?[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                        raise RuntimeError("Original bounded void carrier observer changed")
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "original")
        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original bounded void carrier changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original bounded void carrier entry")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        mutations = (("wrong-factory-result", "T value=factory.invoke(Trace.context);", "T value=(T)(Object)Trace.context;", 0),
                     ("skipped-callback", "Trace.releaseResult=release.invoke(value);", "Trace.releaseResult=Trace.resetResult;", 0),
                     ("swapped-masks", "Trace.flags=flags;Trace.mask=mask;", "Trace.flags=mask;Trace.mask=flags;", 0),
                     ("delayed-publication", "Trace.done=true;Trace.hit(6);", "Trace.hit(6);Trace.done=true;", 19))
        text = (fixture / "fixture/Provider.java").read_text()
        for label, old, new, mode in mutations:
            if text.count(old) != 1:
                raise RuntimeError("Original bounded void carrier counterexample anchor changed")
            changed = work / label / "fixture/Provider.java"
            changed.parent.mkdir(parents=True)
            changed.open("x").write(text.replace(old, new))
            wrong = work / (label + "-classes")
            verify.compile_local([changed], wrong, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness))),
                                     "BoundedVoidCarrierHarness", "1", str(mode)], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned bounded void carrier" not in result.stderr:
                raise RuntimeError("Frozen observer missed incorrect bounded void carrier: " + label)
        if not args.baseline_only:
            output = work / "recovered"
            verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
            coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
            if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                    or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                    or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                    or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")):
                raise RuntimeError("Bounded void carrier recovery changed original entries or scopes")
            rebuilt = work / "rebuilt"
            verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                                 classpath=dependency if args.dependency_inputs else None,
                                 release=coverage.get("minimum_java_release", 8))
            actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
            helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
            expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes - {"Lapi/Function;"}}
            if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
                raise RuntimeError("Bounded void carrier invented or omitted a runtime entry")
            after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
            for owner in classes:
                for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                            "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                    if records[owner].get(key) != after[owner].get(key):
                        raise RuntimeError("Original bounded void carrier class metadata changed: " + owner + ":" + key)
                for identity, metadata in records[owner]["methods"].items():
                    if after[owner]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                        raise RuntimeError("Original bounded void carrier method metadata or Exceptions changed: " + identity)
            runtime = (rebuilt, harness, dependency) if args.dependency_inputs else (rebuilt, harness)
            if execute(runtime, "rebuilt") != baseline:
                raise RuntimeError("Bounded void carrier changed factories, original values, callbacks, casts, order or failures")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original programs or immutable observers changed")
        evidence = {"status": "baseline-only" if args.baseline_only else "success", "class_count": len(classes),
                    "method_count": len(methods), "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "dependency_inputs": args.dependency_inputs, "independent_wrong_programs": len(mutations),
                    "all_original_entries_and_metadata": "unchanged", "nominal_bound_full_factory_and_callback_contracts": "matched",
                    "original_callback_identity_values_checked_casts_order_partial_results_and_exceptions": "matched"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
