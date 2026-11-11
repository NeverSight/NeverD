#!/usr/bin/env python3
"""Verify closed parameterized carrier bounds with exact seed and result scopes."""
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-closed-collection-bound-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "closed_collection_bound/java"
        observers = [FIXTURES / "harness" / (name + ".java") for name in ("ClosedCollectionCaller", "ClosedCollectionHarness")]
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local(observers, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(
            {"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != 8:
            raise RuntimeError("Owned instance nullary factory inventory changed")
        providers = {"Lfixture/Provider;"}
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
                for operation in range(2):
                    for mode in range(25):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "ClosedCollectionHarness", str(route), str(operation), str(mode)],
                            f"{label}-{route}-{operation}-{mode}")
                        expected = f"route={route},operation={operation},mode={mode},attempt=[01],trace=[0-9,]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Owned instance factory behavior matrix changed")
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
            raise RuntimeError("Owned instance nullary factory DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original instance factory definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-call-disassembly")
        if "Lfixture/Provider;.collect:" not in disassembly or "invoke-static" not in disassembly:
            raise RuntimeError("D8 lost the exact bounded static collection call witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Instance factory recovery changed original definitions or contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=8,
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Instance factory recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Instance factory class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Instance factory method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Instance factory call changed original effects or failure state")
        original_signature = b"<C::Ljava/util/Collection<-Ljava/lang/Character;>;>(Ljava/lang/CharSequence;TC;)TC;"
        mutations = (
            ("upper-bound-slot", original_signature.replace(b"<-Ljava/lang/Character;>", b"<+Ljava/lang/Character;>")),
            ("unbound-slot", original_signature.replace(b"<-Ljava/lang/Character;>", b"<*>")),
            ("raw-bound", original_signature.replace(b"<-Ljava/lang/Character;>", b"")),
            ("extra-formal", original_signature.replace(b">(", b"D:Ljava/lang/Object;>(")),
            ("unknown-slot", original_signature.replace(b"<-Ljava/lang/Character;>", b"<-TX;>")),
            ("lost-seed", original_signature.replace(b";TC;)", b";Ljava/util/Collection;)")),
            ("lost-result", original_signature[:-3] + b"Ljava/util/Collection;"),
            ("intersection-bound", original_signature.replace(b">(", b":Ljava/io/Serializable;>(")),
        )
        for label, changed in mutations:
            if changed == original_signature:
                raise RuntimeError("Signature counterexample did not change the contract: " + label)
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Provider.class", original_signature, changed)
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
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
            (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr.lower():
                raise RuntimeError("Instance factory invocation accepted an unproved signature: " + label)
        bodies = {name: (fixture / ("fixture/" + name + ".java")).read_text() for name in ("Provider", "Client")}
        returned = "return Trace.mode == 15 || Trace.mode == 16 ? (C)Trace.returned : destination;"
        wrong_programs = (
            ("snapshotted-length", "Provider", "int index = 0;\n        while (index < text.length())", "int length = text.length(); int index = 0;\n        while (index < length)", 2, 0),
            ("reloaded-text", "Provider", "text.charAt(index)", "Trace.text.charAt(index)", 0, 0),
            ("reloaded-destination", "Provider", "destination.add(boxed);", "Trace.destination.add(boxed);", 0, 0),
            ("changed-utf16", "Provider", "Character.valueOf(value)", "Character.valueOf((char)(value + 1))", 17, 0),
            ("false-add-exit", "Provider", "destination.add(boxed);", "if (!destination.add(boxed)) break;", 0, 0),
            ("repeated-add", "Provider", "destination.add(boxed);", "destination.add(boxed); destination.add(boxed);", 0, 0),
            ("dropped-add", "Provider", "destination.add(boxed);", "", 0, 0),
            ("inferred-input-result", "Provider", returned, "return destination;", 15, 0),
            ("early-destination-check", "Provider", "Trace.hit(2);", "Trace.hit(2); destination.getClass();", 7, 0),
            ("early-result-cast", "Client", "Trace.after = result; Trace.hit(8);\n        return (ArrayList)result;", "ArrayList checked = (ArrayList)result; Trace.after = result; Trace.hit(8);\n        return checked;", 15, 1),
        )
        for label, owner, before, after, mode, operation in wrong_programs:
            body = bodies[owner]
            if body.count(before) != 1 or before == after:
                raise RuntimeError("Owned closed bound counterexample anchor changed: " + label)
            wrong = work / label / ("fixture/" + owner + ".java"); wrong.parent.mkdir(parents=True)
            wrong.open("x").write(body.replace(before, after))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     "ClosedCollectionHarness", "0", str(operation), str(mode)], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned closed collection bound changed" not in result.stderr:
                raise RuntimeError("Frozen closed bound observer missed " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable observers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "dependency_provider": args.dependency_provider, "original_metadata_and_exceptions": "unchanged",
                    "closed_unary_bound_exact_seed_and_result_scope": "matched",
                    "utf16_boxing_mutable_text_add_query_timing_and_late_casts": "matched",
                    "same_code_signature_mutations": len(mutations), "independent_wrong_programs": len(wrong_programs),
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
