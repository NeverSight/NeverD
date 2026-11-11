#!/usr/bin/env python3
"""Verify ordered method inputs and a nested completion retain the original call."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature

OWNER = "Lfixture/api/Base;"
TARGET = OWNER + "->invoke(Lfixture/Fn;Ljava/lang/Object;Ljava/lang/Object;Lfixture/Completion;)Ljava/lang/Object;"
SIGNATURE = "<A:Ljava/lang/Object;B:Ljava/lang/Object;R:Ljava/lang/Object;>(Lfixture/Fn<-TA;-TB;-Lfixture/Completion<-TR;>;+Ljava/lang/Object;>;TA;TB;Lfixture/Completion<-TR;>;)Ljava/lang/Object;"
PROVIDERS = {"Lfixture/" + name + ";" for name in ("Marker", "Fn", "Completion", "Trace", "api/Base", "api/Middle", "api/Api")}
compiler_facts = class_identity.compiler_classes
MAIN = "probe.PairedSeedCompletionHarness"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-paired-seed-completion-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "paired_seed_completion/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / ("PairedSeedCompletion" + name + ".java") for name in ("Caller", "Harness")],
                             harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        sdk_hash, cli_hash = sha(args.android_jar), sha(args.neverd)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 9 or len(all_methods) != 14:
            raise RuntimeError("Owned paired-seed compiler inventory changed")
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        if records[OWNER]["methods"][TARGET]["signature"] != SIGNATURE:
            raise RuntimeError("Original ordered input/completion Signature changed")
        classes = all_classes - (PROVIDERS if args.dependency_provider else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}

        def provider_archive(source, path):
            with zipfile.ZipFile(path, "x") as jar:
                for owner in sorted(PROVIDERS):
                    relative = owner[1:-1] + ".class"
                    jar.writestr(relative, (source / relative).read_bytes())
            return sha(path)

        dependency = work / "owned-provider.jar"
        dependency_hash = provider_archive(originals, dependency)

        def execute(runtime, label):
            values = []
            for route in range(5):
                for mode in range(16):
                    value = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), MAIN, str(route), str(mode)],
                                       f"{label}-{route}-{mode}")
                    if value != "attempts=2,paired-seed-completion=matched\n":
                        raise RuntimeError("Frozen paired-seed observer changed")
                    values.append(value)
            return values

        baseline = execute((originals, harness), "originals")
        (work / "baseline.json").open("x").write(json.dumps(baseline) + "\n")
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        mutations = [
            ("fixture/api/Base.java", "callback.apply(left, right, completion)", "callback.apply(right, left, completion)", 0),
            ("fixture/api/Base.java", "callback.apply(left, right, completion)", "callback.apply(null, right, completion)", 0),
            ("fixture/api/Base.java", "callback.apply(left, right, completion)", "callback.apply(left, null, completion)", 0),
            ("fixture/api/Base.java", "callback.apply(left, right, completion)", "callback.apply(left, right, null)", 0),
            ("fixture/api/Base.java", "Trace.enter(left, right, completion);", ";", 0),
            ("fixture/api/Base.java", "Trace.enter(left, right, completion);", "Trace.tail(left); Trace.enter(left, right, completion);", 0),
            ("fixture/api/Base.java", "Object result = callback.apply(left, right, completion);", "Object result = null;", 0),
            ("fixture/api/Base.java", "return result;", "return (Token) result;", 0),
            ("fixture/api/Base.java", "return result;", "return null;", 0),
            ("fixture/api/Base.java", "Trace.tail(result);", ";", 0),
            ("fixture/api/Base.java", "Trace.tail(result);", "callback.apply(left, right, completion); Trace.tail(result);", 0),
            ("fixture/Trace.java", "beforeAction.run();", ";", 8),
            ("fixture/Trace.java", "beforeAction.run();", "try { beforeAction.run(); } catch (Throwable failure) { throw new IllegalStateException(failure); }", 8),
            ("fixture/Trace.java", "afterAction.run();", ";", 13),
            ("fixture/Trace.java", "afterAction.run();", "try { afterAction.run(); } catch (Throwable failure) { throw new IllegalStateException(failure); }", 13),
            ("fixture/api/Base.java", "Trace.tail(result);", "completion.accept(result); Trace.tail(result);", 0),
            ("fixture/api/Base.java", "return result;", "return left;", 0),
            ("fixture/Trace.java", "Trace.second = right;", "Trace.second = left;", 0),
        ]
        for index, (name, anchor, replacement, mode) in enumerate(mutations):
            source = (fixture / name).read_text()
            if source.count(anchor) != 1:
                raise RuntimeError("Independent paired-seed counterexample anchor changed")
            changed = work / f"wrong-{index}-source" / Path(name).name
            changed.parent.mkdir()
            changed.open("x").write(source.replace(anchor, replacement))
            wrong = work / f"wrong-{index}-classes"
            # Erased parameter swaps are legal bytecode but javac rejects them
            # against distinct method formals; a raw callback preserves that
            # independent wrong program without altering the tested provider.
            if index == 0:
                text = changed.read_text().replace("callback.apply(right, left, completion)", "((Fn) callback).apply(right, left, completion)")
                changed.write_text(text)
            verify.compile_local([changed], wrong, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness))), MAIN, "0", str(mode)],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / f"wrong-{index}-counterexample.log").open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned paired seed completion mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program " + str(index))

        controls = {}
        callback_signature = records["Lfixture/Fn;"]["signature"]
        changes = [
            ("collapsed-input", "fixture/api/Base.class", SIGNATURE, SIGNATURE.replace(";TA;TB;", ";TA;TA;")),
            ("changed-completion", "fixture/api/Base.class", SIGNATURE, SIGNATURE.replace("-Lfixture/Completion<-TR;>", "-Lfixture/Completion<-TA;>")),
            ("narrow-callback-bound", "fixture/Fn.class", callback_signature, callback_signature.replace("A:Ljava/lang/Object;", "A:Ljava/lang/Number;")),
        ]
        for label, relative, before, changed in changes:
            if before == changed:
                raise RuntimeError("Same-Code generic counterexample did not change its constraint")
            control = work / (label + "-originals")
            shutil.copytree(originals, control)
            controls[label] = change_signature(control / relative, before.encode(), changed.encode())
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (control, harness))), MAIN, "0", "0"],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-metadata-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned paired seed completion mismatch" not in result.stderr:
                raise RuntimeError("Frozen reflection observer missed unchanged-Code generic drift")
        (work / "signature-controls.json").open("x").write(json.dumps(controls, indent=2) + "\n")
        classpath = ["--classpath", args.android_jar.resolve()] + (["--classpath", dependency] if args.dependency_provider else [])

        def make_dex(source, path, cp, label):
            path.mkdir()
            inputs = [p for p in sorted(source.rglob("*.class")) if "L" + p.relative_to(source).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *cp, "--output", path, *inputs], label)
            if list(path.glob("classes*.dex")) != [path / "classes.dex"]:
                raise RuntimeError("Owned paired-seed DEX inventory changed")
            return path / "classes.dex"

        dex = make_dex(originals, work / "dex", classpath, "d8")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if {row["descriptor"] for row in inventory["classes"]} != classes or {row["identity"] for row in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original paired-seed definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", dex], "original-calls")
        if "Lfixture/api/Api;.invoke:(Lfixture/Fn;Ljava/lang/Object;Ljava/lang/Object;Lfixture/Completion;)Ljava/lang/Object;" not in disassembly:
            raise RuntimeError("Original inherited static qualifier changed")
        (work / "input-method-witness.json").open("x").write(json.dumps({"dex_sha256": sha(dex), "inherited_static_qualifier": "Lfixture/api/Api;", "canonical_declaration": TARGET}) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "recovered" or coverage.get("projected_method_count", 0)
                or coverage.get("minimum_java_release", 8) != 8 or coverage.get("class_source_bindings")
                or coverage.get("field_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Paired-seed recovery changed original entries or contracts")
        for identity, row in rows.items():
            expected = "recovered" if records[row["class"]]["methods"][identity]["code"] else "declaration-only"
            if row["status"] != expected:
                raise RuntimeError("Original paired-seed Code/declaration role changed")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_provider else None, release=8)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes
                            if any(row["code"] for row in records[c]["methods"].values())}
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Paired-seed recovery invented or omitted an entry/helper")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for c in classes:
            for key in records[c]:
                if key not in ("sha256", "path", "size", "methods") and after[c].get(key) != records[c][key]:
                    raise RuntimeError("Paired-seed class metadata changed: " + c + ":" + key)
            for identity, metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity) != metadata or after_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Original paired-seed method metadata or Exceptions changed: " + identity)
        for identity in expected_helpers:
            metadata = after[identity.split("->", 1)[0]]["methods"][identity]
            if (metadata["access"] != 0x000A or not metadata["code"]
                    or metadata["signature"] != "<E:Ljava/lang/Throwable;>(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;^TE;"):
                raise RuntimeError("Generated exception helper changed its private contract")
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Paired-seed recovery changed aliases, deferred effects or exact failures")
        for label, _, _, _ in changes:
            control = work / (label + "-originals")
            cp = ["--classpath", args.android_jar.resolve()]
            if args.dependency_provider:
                provider = work / (label + "-provider.jar")
                provider_archive(control, provider)
                cp += ["--classpath", provider]
            changed_dex = make_dex(control, work / (label + "-dex"), cp, label + "-d8")
            result = subprocess.run([args.neverd.resolve(), "mobile", changed_dex, *cp, "-o", work / (label + "-recovered")],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-binding-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "generic" not in result.stderr.lower():
                raise RuntimeError("Erased binding accepted unchanged-Code generic drift: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(dex) != json.loads((work / "input-method-witness.json").read_text())["dex_sha256"]
                or sha(args.android_jar) != sdk_hash or sha(args.neverd) != cli_hash or sha(dependency) != dependency_hash):
            raise RuntimeError("Original compiler inputs or frozen observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "stages": 2,
                    "independent_wrong_programs": len(mutations), "unchanged_Code_signature_controls": len(changes),
                    "all_generated_Java": "compiled", "original_metadata_and_Exceptions": "matched",
                    "ordered_inputs_completion_and_opaque_results": "matched", "deferred_reentry_and_exact_failures": "matched",
                    "dependency_provider": args.dependency_provider, "original_inputs_and_observers": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
