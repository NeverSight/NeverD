#!/usr/bin/env python3
"""Verify scoped carrier inputs, closed contexts and fixed nominal result slots."""
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nullary-carrier-factories-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "contextual_closed_tail_factories/java"
        harness_prefix = "ContextualClosedTail"
        operations, modes = 3, 18
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              (harness_prefix + "Caller", harness_prefix + "Harness")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 6 or len(all_methods) != 14:
            raise RuntimeError("Owned nullary-carrier-factories inventory changed")
        providers = {"Lfixture/api/Provider;", "Lfixture/api/Middle;", "Lfixture/api/Facade;"}
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            result = []
            for route in range(4):
                for operation in range(operations):
                    for mode in range(modes):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            harness_prefix + "Harness", str(route), str(operation), str(mode)], f"{label}-{route}-{operation}-{mode}")
                        expected = f"route={route},operation={operation},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Original nullary factory behavior matrix changed: " + output)
                        result.append(output)
            return result

        baseline = execute((originals, harness), "baseline")
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned nullary-factory fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original nullary-carrier-factories runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-argument-disassembly")
        if "Lfixture/api/Facade;.make:" not in disassembly:
            raise RuntimeError("D8 lost the inherited nullary factory call witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Nullary factory invocation recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != 8:
            raise RuntimeError("Nullary factory invocation unexpectedly raised its source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Nullary factory invocation recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original nullary-factory class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original nullary-factory method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Nullary factory call changed an original effect or result")
        original_signature = b"<K:Ljava/lang/Object;>(Ljava/util/List<TK;>;Lfixture/Config;)Ljava/util/Map<TK;Ljava/lang/String;>;"
        negatives = (
            ("nonobject-bound",original_signature.replace(b"K:Ljava/lang/Object;",b"K:Ljava/lang/Number;")),
            ("variant-input",original_signature.replace(b"List<TK;>",b"List<+TK;>")),
            ("variant-result",original_signature.replace(b"TK;Ljava/lang/String;",b"TK;+Ljava/lang/String;")),
            ("repeated-formal",original_signature.replace(b"TK;Ljava/lang/String;",b"TK;TK;")),
            ("extra-formal",original_signature.replace(b";>(",b";U:Ljava/lang/Object;>(")),
            ("lost-binding",original_signature.replace(b"List<TK;>",b"List<Ljava/lang/Object;>")),
        )
        for label, changed in negatives:
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/api/Provider.class", original_signature, changed)
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_classpath = list(classpath)
            if args.dependency_provider:
                negative_jar = work / (label + "-provider.jar")
                with zipfile.ZipFile(negative_jar, "x", compression=zipfile.ZIP_DEFLATED) as jar:
                    for owner in sorted(providers):
                        name = owner[1:-1] + ".class"
                        jar.writestr(name, (negative / name).read_bytes())
                negative_classpath[-1] = negative_jar
            negative_dex = work / (label + "-dex")
            negative_dex.mkdir()
            negative_paths = [negative / path.relative_to(originals) for path in paths]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *negative_classpath,
                        "--output", negative_dex, *negative_paths], label + "-d8")
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex / "classes.dex"),
                                     *map(str, negative_classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Nullary factory invocation accepted an unproved method contract: " + label)
        # Independent implementations that bypass or repeat the original
        # invocation must fail the unchanged side-effect and identity oracle.
        body = (fixture / "fixture/Client.java").read_text()
        result_type = "Map<?, ?>"
        call = "Facade.make(Trace.source, Trace.context)"
        marker = result_type + " result = " + call + ";"
        if body.count(marker) != 1:
            raise RuntimeError("Owned carrier call witness changed")
        for label, replacement in (
                ("bypass", result_type + " result = (" + result_type + ") Trace.result;"),
                ("duplicate", call + "; " + marker)):
            wrong = work / label / "fixture/Client.java"
            wrong.parent.mkdir(parents=True)
            wrong.write_text(body.replace(marker, replacement))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     harness_prefix + "Harness", "1", "0", "0"], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "nullary carrier invocation changed" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        provider_body=(fixture/"fixture/api/Provider.java").read_text()
        wrongs=(
            ("copied-context","fixture/Client.java",body,call,"Facade.make(Trace.source, new Config(Trace.context.mark))",0),
            ("copied-producer","fixture/Client.java",body,call,"Facade.make(Trace.source==null?null:new java.util.ArrayList(Trace.source), Trace.context)",0),
            ("lost-producer-snapshot","fixture/api/Provider.java",provider_body,"Trace.seenSource=source;","Trace.seenSource=null;",0),
            ("narrowed-wide-snapshot","fixture/api/Provider.java",provider_body,"Trace.snapshot=context.mark;","Trace.snapshot=(int)context.mark;",0),
            ("copied-result","fixture/api/Provider.java",provider_body,"return (Map<K, String>) Trace.result;","Map<K,String> old=(Map<K,String>)Trace.result;return old==null?null:new java.util.HashMap<K,String>(old);",0),
            ("lost-context-read","fixture/api/Provider.java",provider_body,"Trace.snapshot=context.mark;","",17),
        )
        for label,relative,text,old,new,mode in wrongs:
            if old not in text:raise RuntimeError("Contextual factory wrong-program anchor changed: "+label)
            wrong=work/label/relative;wrong.parent.mkdir(parents=True);wrong.open("x").write(text.replace(old,new))
            compiled=work/(label+"-classes");verify.compile_local([wrong],compiled,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),harness_prefix+"Harness","1","0",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "nullary carrier invocation changed" not in result.stderr:raise RuntimeError("Frozen observer missed "+label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "fixed_nominal_result_tail": True, "same_code_signature_mutations": len(negatives), "independent_wrong_programs": 8,
                    "original_metadata_and_exceptions": "unchanged", "nullary_calls_and_original_mutable_or_immutable_identity": "matched",
                    "original_exception_identity": "matched", "same_code_counterexamples": "rejected",
                    "independent_bypass_and_repeated_call_counterexamples": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
