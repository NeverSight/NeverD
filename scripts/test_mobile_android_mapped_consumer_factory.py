#!/usr/bin/env python3
"""Verify mapped consumers retain method scope, opaque factories and callback order."""
from __future__ import annotations

import argparse
import copy
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_visibility_generic_bridge import compiler_facts
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_visibility_generic_bridge import compiler_facts
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-mapped-consumer-factory-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "mapped_consumer_factory/java"
        prefix = "MappedConsumerFactory"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (prefix + n + ".java") for n in ("Caller", "Future", "Harness")],
                             harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        inputs = {str(p): sha(p) for p in (args.android_jar.resolve(), args.neverd.resolve())}
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (4, 8):
            raise RuntimeError("Original mapped consumer inventory changed")
        excluded = {"Lfixture/Mapper;", "Lfixture/Rule;"} if args.dependency_provider else set()
        release = 8 if excluded else 9
        classes = all_classes - excluded
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        signature = b"<T:Ljava/lang/Object;U:Ljava/lang/Object;>(Lfixture/Mapper<-TT;+TU;>;Lfixture/Rule<-TU;>;)Lfixture/Rule<TT;>;"
        identity = "Lfixture/Rule;->bind(Lfixture/Mapper;Lfixture/Rule;)Lfixture/Rule;"
        if records["Lfixture/Rule;"]["methods"][identity]["signature"] != signature.decode():
            raise RuntimeError("Original mapped factory method scope changed")
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for owner in sorted(excluded):
                path = owner[1:-1] + ".class"
                jar.writestr(path, (originals / path).read_bytes())
        inputs[str(dependency)] = sha(dependency)
        classpath = ["--classpath", args.android_jar.resolve()]
        if excluded:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            outputs = []
            for route in range(4):
                for platform in ("owned", "jdk"):
                    for mode in range(14):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), prefix + "Harness",
                                             str(route), platform, str(mode)], f"{label}-{route}-{platform}-{mode}")
                        pattern = f"route={route},platform={platform},mode={mode},attempt=[01],events=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                            raise RuntimeError("Original mapped consumer observer changed: " + output)
                        outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        wrongs = (
            ("lost-mapper", "Rule", "Trace.mapper = mapper;", "Trace.mapper = null;", "owned", 0),
            ("lost-consumer", "Rule", "Trace.rule = rule;", "Trace.rule = null;", "owned", 0),
            ("lost-entry", "Rule", "Trace.hit(1);", ";", "owned", 0),
            ("lost-completion", "Rule", "Trace.hit(3);", ";", "owned", 0),
            ("duplicate-callback", "Rule", "Trace.action.run();", "Trace.action.run(); Trace.action.run();", "owned", 0),
            ("wrapped-checked", "Rule", "Trace.action.run();", "try { Trace.action.run(); } catch (Throwable failure) { throw new IllegalStateException(failure); }", "owned", 4),
            ("wrong-result", "Rule", "return (Rule<T>) Trace.result;", "return (Rule<T>) rule;", "owned", 0),
            ("lost-input-mapper", "Core", "return Rule.bind(mapper, rule);", "return Rule.bind(null, rule);", "owned", 0),
            ("lost-input-consumer", "Core", "return Rule.bind(mapper, rule);", "return Rule.bind(mapper, null);", "owned", 0),
            ("repeated-factory", "Core", "return Rule.bind(mapper, rule);", "Rule.bind(mapper, rule); return Rule.bind(mapper, rule);", "owned", 0),
            ("early-jdk-mapper", "Core", "return Comparator.comparing(mapper, comparator);", 'mapper.apply("owned early"); return Comparator.comparing(mapper, comparator);', "jdk", 0),
            ("reversed-jdk-comparison", "Core", "return Comparator.comparing(mapper, comparator);", "return Comparator.comparing(mapper, comparator).reversed();", "jdk", 0),
        )
        for label, owner, anchor, replacement, platform, mode in wrongs:
            original = (fixture / "fixture" / (owner + ".java")).read_text()
            if original.count(anchor) != 1:
                raise RuntimeError("Original mapped factory counterexample anchor changed: " + label)
            changed = work / label / (owner + ".java")
            changed.parent.mkdir()
            changed.open("x").write(original.replace(anchor, replacement))
            compiled = work / (label + "-classes")
            verify.compile_local([changed], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     prefix + "Harness", "1", platform, str(mode)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned mapped consumer factory invariant" not in result.stderr:
                raise RuntimeError("Frozen observer missed incorrect mapped factory: " + label)

        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        inputs[str(source_dex)] = sha(source_dex)
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original mapped factory DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original mapped factory definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        report = json.loads((output / "report.json").read_text())["android_method_recovery"]
        expected_status = {m: ("recovered" if records[m.split("->", 1)[0]]["methods"][m]["code"] else "declaration-only") for m in methods}
        if ({r["identity"] for r in report["methods"]} != methods or report["class_count"] != len(classes)
                or report["method_count"] != len(methods) or report["unrecovered_method_count"]
                or report["recovered_method_count"] != sum(s == "recovered" for s in expected_status.values())
                or any(r["status"] != expected_status[r["identity"]] for r in report["methods"])
                or report.get("declaration_only_method_count", 0) != sum(s == "declaration-only" for s in expected_status.values()) or report.get("projected_method_count", 0)
                or report["status"] != "recovered" or report.get("minimum_java_release", 8) != release
                or report.get("class_source_bindings") or report.get("generic_signature_projections")
                or report.get("field_source_bindings") or report.get("default_instance_final_initializers")):
            raise RuntimeError("Mapped factories changed original definitions or source contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if excluded else None, release=release)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in report.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if any(m["code"] for m in records[c]["methods"].values())}
        if actual_classes != classes or set(actual_methods) != methods | helpers or helpers != expected_helpers:
            raise RuntimeError("Mapped factory source invented or omitted a runtime definition")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(records[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if wanted.pop("major") != 52 or actual.pop("major") != 44 + release:
                raise RuntimeError("Mapped factory compiler release changed")
            if actual != wanted or any(after_exceptions[m] != exceptions[m] for m in records[owner]["methods"]):
                raise RuntimeError("Original mapped factory Code, access, fields, Signature or Exceptions changed: " + owner)
        runtime = (rebuilt, harness, dependency) if excluded else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Mapped factories changed input snapshots, callback order, result identity or exceptions")
        controls = (
            ("missing-input-formal", signature, signature.replace(b"TT;", b"TMissing;")),
            ("changed-result-formal", signature, signature.replace(b")Lfixture/Rule<TT;>;", b")Lfixture/Rule<TU;>;")),
            ("wrong-mapper-input-variance", signature, signature.replace(b"Mapper<-TT;", b"Mapper<+TT;")),
            ("wrong-mapper-output-variance", signature, signature.replace(b"+TU;", b"-TU;")),
            ("wrong-consumer-variance", signature, signature.replace(b"Rule<-TU;", b"Rule<+TU;")),
            ("duplicate-method-formal", signature, signature.replace(b"U:Ljava/lang/Object;>", b"T:Ljava/lang/Object;>")),
            ("wrong-result-erasure", signature, signature.replace(b")Lfixture/Rule<TT;>;", b")Ljava/lang/Object;")),
            ("changed-carrier-bound", b"<Owner:Ljava/lang/Object;>Ljava/lang/Object;", b"<Owner:Ljava/lang/Number;>Ljava/lang/Object;"),
        )
        for label, before_signature, changed_signature in controls:
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            target = negative / "fixture/Rule.class"
            original_code = method_code(target.read_bytes())
            receipt = change_signature(target, before_signature, changed_signature)
            if method_code(target.read_bytes()) != original_code:
                raise RuntimeError("Mapped factory Signature control changed Code")
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            negative_classpath = classpath
            if excluded:
                changed_dependency = work / (label + "-provider.jar")
                with zipfile.ZipFile(changed_dependency, "x") as jar:
                    for owner in sorted(excluded):
                        entry = owner[1:-1] + ".class"
                        jar.writestr(entry, (negative / entry).read_bytes())
                negative_classpath = ["--classpath", args.android_jar.resolve(), "--classpath", changed_dependency]
            negative_dex = work / (label + "-dex")
            negative_dex.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", *negative_classpath, "--output", negative_dex,
                        *[negative / path.relative_to(originals) for path in paths]], label + "-d8")
            rejected = work / (label + "-rejected")
            result = subprocess.run([args.neverd.resolve(), "mobile", negative_dex / "classes.dex", *negative_classpath,
                                     "-o", rejected], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Unproved mapped factory Signature accepted: " + label)
        if hashes(originals) != frozen["originals"] or hashes(harness) != frozen["harness"] or any(sha(Path(p)) != h for p, h in inputs.items()):
            raise RuntimeError("Original mapped factory inputs or frozen observers changed")
        summary = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                   "all_java_units": len(list((output / "sources").rglob("*.java"))), "fresh_jvm_cases": 112,
                   "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                   "independent_wrong_programs": len(wrongs), "same_code_signature_controls": len(controls), "all_original_definitions_and_compiler_metadata": "unchanged",
                   "opaque_results_callback_order_delayed_casts_and_exact_failures": "matched",
                   "original_inputs_and_immutable_callers": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
