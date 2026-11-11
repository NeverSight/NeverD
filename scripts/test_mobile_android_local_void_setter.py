#!/usr/bin/env python3
"""Verify distinct local generic void setter and reference updater JVM slots."""
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
    from scripts.test_mobile_android_final_getter_return_slots import rename_owned_entry
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_final_getter_return_slots import rename_owned_entry


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-local-void-setter-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "local_void_setter"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=8)
        reference = work / "unencoded-reference"
        shutil.copytree(originals, reference)
        for owner in ("Entry", "Client"):
            receipt = rename_owned_entry(originals / ("fixture/" + owner + ".class"), b"assign", b"setValue")
            (work / (owner.lower() + "-original-name-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
        verify.compile_local(sorted((fixture / "harness").glob("*.java")), harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk_hash = sha(args.android_jar)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != 14:
            raise RuntimeError("Owned local setter constructor inventory changed")
        providers = {"Lfixture/Trace;"}
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = sha(dependency)
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label, void_name):
            result = []
            for route in range(4):
                for operation, modes in ((0, range(21)), (1, (0, 16, 17, 19))):
                    for mode in modes:
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "LocalSetterHarness", void_name, str(route), str(operation), str(mode)],
                            f"{label}-{route}-{operation}-{mode}")
                        pattern = f"route={route},operation={operation},mode={mode},attempt=[01],events=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                            raise RuntimeError("Frozen local setter observer changed")
                        result.append(output)
            return result

        baseline = execute((originals, harness), "baseline", "setValue")
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned local setter constructor DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original local setter constructor definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-call-disassembly")
        if ("Lfixture/Entry;.setValue:(Ljava/lang/Object;)V" not in disassembly
                or "Lfixture/Entry;.setValue:(Ljava/lang/Object;)Ljava/lang/Object;" not in disassembly):
            raise RuntimeError("D8 lost the exact original void/reference entry calls")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 1 or coverage["status"] != "partial"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Local setter recovery changed original definitions or source contracts")
        void = "Lfixture/Entry;->setValue(Ljava/lang/Object;)V"
        reference_slot = "Lfixture/Entry;->setValue(Ljava/lang/Object;)Ljava/lang/Object;"
        rows = {row["identity"]: row for row in coverage["methods"]}
        named = [row for row in rows.values() if row.get("source_name")]
        if (len(named) != 1 or named[0]["identity"] != void
                or named[0].get("projection_kind") != "incompatible-local-return-slot"
                or named[0].get("source_name_declaration") != void
                or named[0].get("conflicting_local_contracts") != [reference_slot]
                or named[0].get("generic_signature") != "(TV;)V"
                or not named[0].get("metadata_differences")):
            raise RuntimeError("Local void source name lost original scoped slot or ABI disclosure")
        alias = named[0]
        source_methods = {row.get("source_identity", row["identity"]) for row in rows.values()}
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != source_methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Local setter recovery invented or omitted a runtime entry/helper")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, actual = records[owner], after[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != actual.get(key):
                    raise RuntimeError("Original local setter class metadata changed: " + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                target = alias["source_identity"] if method == void else method
                if method == void:
                    wanted["name"] = alias["source_name"]
                if actual["methods"].get(target) != wanted or after_exceptions.get(target) != exceptions[method]:
                    raise RuntimeError("Original local setter metadata changed beyond the disclosed void name: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt", alias["source_name"]) != baseline:
            raise RuntimeError("Local setter recovery changed casts, overloads, references or failure state")
        setter = b"(TV;)V"
        updater = b"(TV;)TV;"
        owner = b"<K:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;Ljava/util/Map$Entry<TK;TV;>;"
        mutations = (
            ("setter-slot", "Entry", setter, b"(TK;)V"),
            ("updater-parameter", "Entry", updater, b"(TK;)TV;"),
            ("updater-result", "Entry", updater, b"(TV;)TK;"),
            ("field-slot", "Entry", b"TV;", b"TK;"),
            ("unknown-setter", "Entry", setter, b"(TX;)V"),
            ("method-formal-setter", "Entry", setter, b"<X:Ljava/lang/Object;>(TV;)V"),
            ("wrong-interface-scope", "Entry", owner, owner.replace(b"<TK;TV;>", b"<TV;TK;>")),
        )
        for label, owner, original, changed in mutations:
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / ("fixture/" + owner + ".class"), original, changed)
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            negative_classpath = list(classpath)
            if args.dependency_provider:
                negative_jar = work / (label + "-provider.jar")
                with zipfile.ZipFile(negative_jar, "x", compression=zipfile.ZIP_DEFLATED) as jar:
                    for provider in sorted(providers):
                        name = provider[1:-1] + ".class"
                        jar.writestr(name, (negative / name).read_bytes())
                negative_classpath[-1] = negative_jar
            negative_dex = work / (label + "-dex")
            negative_dex.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", *negative_classpath, "--output", negative_dex,
                        *[negative / path.relative_to(originals) for path in paths]], label + "-d8")
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex / "classes.dex"),
                                    *map(str, negative_classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if (result.returncode != 1 or rejected.exists() or not any(
                    token in result.stderr.lower() for token in ("generic", "super invocation", "source", "signature", "interface", "overloaded only by return type"))):
                raise RuntimeError("Local setter constructor accepted an unproved signature: " + label)
        body = (fixture / "java/fixture/Entry.java").read_text()
        wrong_programs = (
            ("new-value-result", "return previous;", "return input;", 0),
            ("duplicate-getter", "V previous = getValue();", "V previous = getValue(); getValue();", 0),
            ("lost-write", "        assign(input);\n        Trace.hit(3);", "        Trace.hit(3);", 0),
            ("late-write", "        assign(input);\n        Trace.hit(3);", "        Trace.hit(3);\n        assign(input);", 6),
            ("lost-callback", "Trace.callback(this, getKey(), input);", "", 0),
            ("old-callback-input", "Trace.callback(this, getKey(), input);", "Trace.callback(this, getKey(), previous);", 0),
            ("reloaded-result", "return previous;", "return value;", 1),
            ("lost-restore-value", "public void restore(V input) { assign(input); }", "public void restore(V input) { assign(null); }", 1),
            ("lost-setter-value", "public void assign(V input) { this.value = input; }", "public void assign(V input) { this.value = value; }", 0),
        )
        for label, before, changed, mode in wrong_programs:
            if body.count(before) != 1 or before == changed:
                raise RuntimeError("Owned local setter counterexample anchor changed: " + label)
            wrong = work / label / "fixture/Entry.java"
            wrong.parent.mkdir(parents=True)
            wrong.open("x").write(body.replace(before, changed))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=reference, release=8)
            receipt = rename_owned_entry(compiled / "fixture/Entry.class", b"assign", b"setValue")
            (work / (label + "-name-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                    "LocalSetterHarness", "setValue", "0", "0", str(mode)], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned local void setter changed" not in result.stderr:
                raise RuntimeError("Frozen local setter observer missed " + label)
        if ({"originals": hashes(originals), "harness": hashes(harness)} != frozen
                or sha(args.android_jar) != sdk_hash or sha(dependency) != dependency_hash):
            raise RuntimeError("Original local setter inputs, frozen observers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "dependency_provider": args.dependency_provider, "original_metadata_and_exceptions": "unchanged",
                    "local_scoped_void_writer_source_name": "disclosed",
                    "original_void_runtime_name_binary_abi_and_name_reflection": "source name differs as reported",
                    "old_value_field_commits_callback_snapshots_and_exact_failures": "matched",
                    "same_code_signature_mutations": len(mutations), "independent_wrong_programs": len(wrong_programs),
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
