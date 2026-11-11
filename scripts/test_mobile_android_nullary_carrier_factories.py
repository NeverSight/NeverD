#!/usr/bin/env python3
"""Verify nullary generic carrier factories without changing their calls or returned objects."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
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
    parser.add_argument("--paired-result", action="store_true",
                        help="Retain both independent formals of an invariant Map result")
    parser.add_argument("--closed-object-tail", action="store_true",
                        help="Retain a sole method formal beside an invariant Object result slot")
    parser.add_argument("--scalar-argument", action="store_true")
    parser.add_argument("--reifiable-input", action="store_true")
    parser.add_argument("--nested-result", action="store_true")
    parser.add_argument("--instance-nested-result", action="store_true")
    parser.add_argument("--enclosing-nested-result", action="store_true")
    parser.add_argument("--independent-member", action="store_true")
    parser.add_argument("--partial-final-constructors", action="store_true")
    args = parser.parse_args()
    if args.partial_final_constructors:
        args.independent_member = True
    if args.independent_member:
        args.enclosing_nested_result = True
    if args.enclosing_nested_result:
        args.instance_nested_result = True
    if args.instance_nested_result:
        args.nested_result = True
    if args.nested_result and (args.reifiable_input or args.scalar_argument or args.paired_result or args.closed_object_tail):
        parser.error("--nested-result requires a unary invariant input and result")
    if args.reifiable_input and (args.scalar_argument or args.paired_result or args.closed_object_tail):
        parser.error("--reifiable-input requires a unary invariant result")
    if args.scalar_argument:
        if args.closed_object_tail:
            parser.error("--scalar-argument requires both independent result formals")
        args.paired_result = True
    if args.closed_object_tail:
        args.paired_result = True
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nullary-carrier-factories-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / ("partial_final_constructors/java" if args.partial_final_constructors else "independent_synthetic_member/java" if args.independent_member else "enclosing_nested_factory/java" if args.enclosing_nested_result else "instance_nested_factory/java" if args.instance_nested_result else "nested_invariant_factory/java" if args.nested_result else "reifiable_carrier_factories/java" if args.reifiable_input else "scalar_pair_factories/java" if args.scalar_argument else "nullary_closed_tail_factories/java" if args.closed_object_tail
                              else "nullary_pair_factories/java" if args.paired_result
                              else "nullary_carrier_factories/java")
        harness_prefix = ("PartialFinal" if args.partial_final_constructors else "IndependentMember" if args.independent_member else "EnclosingNested" if args.enclosing_nested_result else "InstanceNested" if args.instance_nested_result else "NestedInvariant" if args.nested_result else "ReifiableCarrier" if args.reifiable_input else "ScalarPair" if args.scalar_argument else "NullaryClosedTail" if args.closed_object_tail
                          else "NullaryPair" if args.paired_result else "NullaryCarrier")
        operations, modes = (3, 16) if args.paired_result else (2, 16 if args.nested_result else 12)
        originals, harness = work / "originals", work / "harness"
        sources = sorted(fixture.rglob("*.java"))
        if args.scalar_argument:
            sources += [FIXTURES / ("nullary_pair_factories/java/fixture/api/" + name + ".java") for name in ("Facade", "Middle")]
        if args.reifiable_input or (args.nested_result and not args.enclosing_nested_result):
            sources += [FIXTURES / ("nullary_carrier_factories/java/fixture/api/" + name + ".java") for name in ("Facade", "Middle")]
        verify.compile_local(sources, originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              (harness_prefix + "Caller", harness_prefix + "Harness")], harness, classpath=originals, release=8)
        compiler_originals = None
        if args.independent_member:
            try:
                from scripts.test_mobile_android_independent_member import encode_member
            except ModuleNotFoundError as error:
                if error.name != "scripts": raise
                from test_mobile_android_independent_member import encode_member
            compiler_originals = work / "compiler-originals"
            shutil.copytree(originals, compiler_originals)
            witness = encode_member(originals, partial_finals=args.partial_final_constructors)
            (work / "encoding-witness.json").open("x").write(json.dumps(witness, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(
            {"originals": original_hashes, "harness": harness_hashes, **({"compiler-originals": hashes(compiler_originals)} if compiler_originals else {})}, indent=2) + "\n")
        (work / "baseline-cli-sha256.txt").open("x").write(hashlib.sha256(args.neverd.read_bytes()).hexdigest() + "\n")
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != (6 if args.nested_result else 5) or len(all_methods) != (20 if args.partial_final_constructors else 17 if args.independent_member else 16 if args.enclosing_nested_result else 13 if args.paired_result or args.nested_result else 12):
            raise RuntimeError("Owned nullary-carrier-factories inventory changed")
        providers = ({"Lfixture/Carrier;"} if args.enclosing_nested_result else {"Lfixture/api/Provider;", "Lfixture/api/Middle;", "Lfixture/api/Facade;"})
        provider_path = "fixture/Scope$Cell$Provider.class" if args.enclosing_nested_result else "fixture/api/Provider.class"
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
                        output = verify.run([verify.java, *(["-Dfixture.independent.encoded=" + str(label == "baseline").lower()] if args.independent_member else []), *(["-Dfixture.partialfinal.original=" + str(label == "compiler-baseline").lower()] if args.partial_final_constructors else []), "-cp", os.pathsep.join(map(str, runtime)),
                            harness_prefix + "Harness", str(route), str(operation), str(mode)], f"{label}-{route}-{operation}-{mode}")
                        expected = f"route={route},operation={operation},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Original nullary factory behavior matrix changed: " + output)
                        result.append(output)
            return result

        compiler_baseline = execute((compiler_originals, harness), "compiler-baseline") if compiler_originals else None
        baseline = execute((originals, harness), "baseline")
        if compiler_baseline is not None and compiler_baseline != baseline:
            raise RuntimeError("Member flags changed original bytecode behavior")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        source_dex_hash = hashlib.sha256(source_dex.read_bytes()).hexdigest()
        (work / "source-dex-sha256.txt").open("x").write(source_dex_hash + "\n")
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned nullary-factory fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original nullary-carrier-factories runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-argument-disassembly")
        if ("Lfixture/Scope$Cell$Provider;.make:" if args.enclosing_nested_result else "Lfixture/api/Facade;.make:") not in disassembly:
            raise RuntimeError("D8 lost the inherited nullary factory call witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != (2 if args.independent_member else 0) or coverage["status"] != ("partial" if args.independent_member else "recovered")
                or (not args.independent_member and coverage.get("class_source_bindings")) or coverage.get("generic_signature_projections")):
            raise RuntimeError("Nullary factory invocation recovery changed original inventory or contracts")
        if coverage.get("minimum_java_release", 8) != 8:
            raise RuntimeError("Nullary factory invocation unexpectedly raised its source release")
        if args.independent_member:
            bindings = coverage.get("class_source_bindings", [])
            target = "Lfixture/Scope$Cell;"
            projected = {row["identity"] for row in coverage["methods"] if row.get("status") == "source-projected"}
            wanted = {identity for identity in methods if identity.startswith(target + "->")}
            if (len(bindings) != 1 or bindings[0].get("class") != target
                    or bindings[0].get("binding_kind") != "independent-synthetic-member"
                    or bindings[0].get("binary_name_status") != "preserved"
                    or not bindings[0].get("metadata_differences") or projected != wanted):
                raise RuntimeError("Independent member projection omitted original identities or differences")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        if args.partial_final_constructors:
            import copy
            try:
                from scripts.test_mobile_android_static_defaults import method_code
            except ModuleNotFoundError as error:
                if error.name != "scripts": raise
                from test_mobile_android_static_defaults import method_code
            recipe = coverage.get("java_rebuild", {})
            bindings = coverage.get("field_source_bindings", [])
            if (not coverage.get("requires_java_rebuild_postprocessing")
                    or recipe.get("application_code_execution") is not False
                    or recipe.get("utility_source") != "rebuild/RestoreFieldFlags.java"
                    or recipe.get("field_manifest") != "rebuild/runtime-fields.tsv"
                    or len(bindings) != 2
                    or any(row.get("binding_kind") != "constructor-final-field" or row.get("class") != "Lfixture/Scope$Cell;" for row in bindings)
                    or {row.get("source_name"): row.get("required_postcompile_flags") for row in bindings} != {"value": "final", "token": "final,synthetic"}):
                raise RuntimeError("Missing precise required runtime field restoration recipe")
            top = json.loads((output / "report.json").read_text())
            if (set(top.get("java_rebuild_artifacts", [])) != {recipe["utility_source"], recipe["field_manifest"]}
                    or not top.get("requires_java_rebuild_postprocessing")
                    or top.get("java_source_count") != len(list((output / "sources").rglob("*.java")))):
                raise RuntimeError("Build utility changed original application source accounting")
            before_restore = work / "before-restore"
            shutil.copytree(rebuilt, before_restore)
            before_bytes = {p.relative_to(rebuilt).as_posix(): p.read_bytes() for p in rebuilt.rglob("*.class")}
            missing = subprocess.run([verify.java, "-Dfixture.independent.encoded=false",
                "-Dfixture.partialfinal.original=false", "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                harness_prefix + "Harness", "0", "0", "0"], text=True, capture_output=True,
                timeout=300, env=verify.environment)
            (work / "missing-restoration-counterexample.log").open("x").write(missing.stdout + missing.stderr)
            if missing.returncode == 0 or "nullary carrier invocation changed" not in missing.stderr:
                raise RuntimeError("Frozen oracle did not require final field restoration")
            tool = work / "metadata-build-utility"
            verify.compile_local([output / recipe["utility_source"]], tool, release=8)
            command = [verify.java, "-cp", tool, "RestoreFieldFlags", rebuilt, output / recipe["field_manifest"]]
            verify.run(command, "restore-field-flags")
            after_bytes = {p.relative_to(rebuilt).as_posix(): p.read_bytes() for p in rebuilt.rglob("*.class")}
            if (set(before_bytes) != set(after_bytes)
                    or any(method_code(before_bytes[k]) != method_code(after_bytes[k]) for k in before_bytes)
                    or {k for k in before_bytes if before_bytes[k] != after_bytes[k]} != {"fixture/Scope$Cell.class"}):
                raise RuntimeError("Field restoration changed Code or another runtime definition")
            verify.run(command, "restore-field-flags-idempotent")
            if any((rebuilt / k).read_bytes() != v for k, v in after_bytes.items()):
                raise RuntimeError("Field restoration is not idempotent")
            for label, text in (("wrong-type", "fixture/Scope$Cell\tvalue\tLjava/lang/String;\tfinal\n"),
                                ("duplicate", (output / recipe["field_manifest"]).read_text() * 2),
                                ("outside", "../outside\tvalue\tLjava/lang/Object;\tfinal\n"),
                                ("static", "fixture/Trace\tevents\tI\tfinal\n")):
                manifest = work / (label + "-field-manifest.tsv"); manifest.open("x").write(text)
                bad = subprocess.run([str(verify.java), "-cp", str(tool), "RestoreFieldFlags", str(rebuilt), str(manifest)],
                    text=True, capture_output=True, timeout=300, env=verify.environment)
                (work / (label + "-field-restoration.log")).open("x").write(bad.stdout + bad.stderr)
                if bad.returncode == 0 or any((rebuilt / k).read_bytes() != v for k, v in after_bytes.items()):
                    raise RuntimeError("Invalid field recipe mutated compiled classes")
            synthetic = work / "synthetic-restoration-control"; shutil.copytree(before_restore, synthetic)
            manifest = work / "synthetic-field-manifest.tsv"
            manifest.open("x").write("fixture/Scope$Cell\tvalue\tLjava/lang/Object;\tfinal,synthetic\n")
            verify.run([verify.java, "-cp", tool, "RestoreFieldFlags", synthetic, manifest], "restore-synthetic-field")
            text = verify.run([Path(verify.java).with_name("javap"), "-v", "-p", "-classpath", synthetic, "fixture.Scope$Cell"], "synthetic-field-metadata")
            if not re.search(r"public final E value;.*?flags: \(0x1011\).*?ACC_FINAL.*?ACC_SYNTHETIC", text, re.S):
                raise RuntimeError("Synthetic final field metadata was not restored")
            if any(method_code((synthetic / k).read_bytes()) != method_code(v) for k, v in before_bytes.items()):
                raise RuntimeError("Synthetic field restoration changed Code")
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if not args.nested_result or owner != "Lfixture/Carrier;"}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Nullary factory invocation recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            if args.independent_member and owner == "Lfixture/Scope$Cell;":
                import copy
                before = copy.deepcopy(before)
                before["access"] &= ~0x1000
                before["inner_class"]["access"] = (before["inner_class"]["access"] | 0x0008) & ~0x1000
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
        original_signature = (b"<K:Ljava/lang/Object;>()Ljava/util/Map<TK;Ljava/lang/Object;>;"
                              if args.closed_object_tail else
                              b"<K:Ljava/lang/Object;V:Ljava/lang/Object;>()Ljava/util/Map<TK;TV;>;"
                              if args.paired_result else b"<T:Ljava/lang/Object;>()Ljava/util/List<TT;>;")
        if args.scalar_argument:
            original_signature = original_signature.replace(b">()", b">(I)")
        if args.reifiable_input:
            original_signature = original_signature.replace(b">()", b">(Ljava/util/Collection<*>;)")
        negatives = (
                ("nonobject-bound", original_signature.replace(b"K:Ljava/lang/Object;", b"K:Ljava/lang/Number;")),
                ("variant-formal", original_signature.replace(b"<TK;", b"<+TK;")),
                ("variant-tail", original_signature.replace(b"TK;Ljava/lang/Object;", b"TK;+Ljava/lang/Object;")),
                ("reversed-slots", original_signature.replace(b"<TK;Ljava/lang/Object;>", b"<Ljava/lang/Object;TK;>")),
                ("changed-tail", original_signature.replace(b"TK;Ljava/lang/Object;", b"TK;Ljava/lang/String;")),
                ("extra-formal", original_signature.replace(b";>()", b";U:Ljava/lang/Object;>()")),
        ) if args.closed_object_tail else (
                ("nonobject-bound", original_signature.replace(b"K:Ljava/lang/Object;", b"K:Ljava/lang/Number;")),
                ("variant-result", original_signature.replace(b"<TK;TV;>", b"<+TK;TV;>")),
                ("closed-result", original_signature.replace(b"<TK;TV;>", b"<Ljava/lang/Object;TV;>")),
                ("repeated-formal", original_signature.replace(b"<TK;TV;>", b"<TK;TK;>")),
                ("extra-formal", original_signature.replace(b"V:Ljava/lang/Object;>", b"V:Ljava/lang/Object;W:Ljava/lang/Object;>"))
        ) if args.paired_result else (
                ("nonobject-bound", original_signature.replace(b"T:Ljava/lang/Object;>", b"T:Ljava/lang/Number;>")),
                ("variant-result", original_signature.replace(b"<TT;>", b"<+TT;>")),
                ("closed-result", original_signature.replace(b"<TT;>", b"<Ljava/lang/Object;>")),
                ("extra-formal", original_signature.replace(b"T:Ljava/lang/Object;>", b"T:Ljava/lang/Object;U:Ljava/lang/Object;>")))
        if args.reifiable_input:
            negatives += tuple((label, original_signature.replace(b"Collection<*>;", changed)) for label, changed in (
                ("typed-input", b"Collection<Ljava/lang/String;>;"),
                ("variant-input", b"Collection<+Ljava/lang/String;>;"),
                ("nested-input", b"Collection<Ljava/util/List<*>;>;")))
        if args.nested_result:
            original_signature = b"<T:Ljava/lang/Object;>(Lfixture/Carrier<TT;>;)Lfixture/Carrier<Ljava/util/List<TT;>;>;"
            negatives = (
                ("nonobject-bound", original_signature.replace(b"T:Ljava/lang/Object;>", b"T:Ljava/lang/Number;>")),
                ("variant-input", original_signature.replace(b"(Lfixture/Carrier<TT;>", b"(Lfixture/Carrier<+TT;>")),
                ("variant-outer", original_signature.replace(b";)Lfixture/Carrier<", b";)Lfixture/Carrier<+")),
                ("variant-inner", original_signature.replace(b"List<TT;>", b"List<+TT;>")),
                ("raw-inner", original_signature.replace(b"List<TT;>", b"List")),
                ("unknown-inner", original_signature.replace(b"java/util/List<TT;>", b"fixture/Missing<TT;>")),
                ("unbound-result", original_signature.replace(b"List<TT;>", b"List<TMissing;>")),
                ("extra-formal", original_signature.replace(b"T:Ljava/lang/Object;>", b"T:Ljava/lang/Object;U:Ljava/lang/Object;>")),
            )
        if args.enclosing_nested_result:
            original_signature = original_signature.replace(b"java/util/List", b"fixture/Scope$Cell")
            negatives = tuple((label, changed.replace(b"java/util/List", b"fixture/Scope$Cell")) for label, changed in negatives)
        for label, changed in negatives:
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / provider_path, original_signature, changed)
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
            if (result.returncode != 1 or rejected.exists()
                    or (not args.nested_result and "generic invocation requires" not in result.stderr)
                    or (args.nested_result and not result.stderr.startswith("error: "))):
                raise RuntimeError("Factory contract did not fail clearly without publication: " + label)
        # Independent implementations that bypass or repeat the original
        # invocation must fail the unchanged side-effect and identity oracle.
        body = (fixture / "fixture/Client.java").read_text()
        result_type = "Carrier<?>" if args.nested_result else "Map<?, ?>" if args.paired_result else "List<?>"
        invocation = "Scope.make(Trace.receiver, Trace.box)" if args.enclosing_nested_result else "Trace.receiver.make(Trace.box)" if args.instance_nested_result else "Facade.make(Trace.box)" if args.nested_result else "Facade.make(Trace.collection)" if args.reifiable_input else "Facade.make(Trace.capacity)" if args.scalar_argument else "Facade.make()"
        marker = result_type + " result = " + invocation + ";"
        if body.count(marker) != 1:
            raise RuntimeError("Owned carrier call witness changed")
        for label, replacement in (
                ("bypass", result_type + " result = (" + result_type + ") Trace.result;"),
                ("duplicate", invocation + "; " + marker)):
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
        if args.scalar_argument:
            extra_wrongs = [
                ("zero-capacity", "Client", "Facade.make(Trace.capacity)", "Facade.make(0)"),
                ("truncated-capacity", "Client", "Facade.make(Trace.capacity)", "Facade.make((short)Trace.capacity)"),
                ("late-capacity", "api/Provider", "Trace.capacitySeen = capacity;", "Trace.capacitySeen = Trace.capacity;"),
            ]
            for label, owner, before, changed in extra_wrongs:
                text = (fixture / ("fixture/" + owner + ".java")).read_text()
                if not text.count(before): raise RuntimeError("Scalar factory counterexample anchor changed")
                source = work / label / ("fixture/" + owner + ".java")
                source.parent.mkdir(parents=True)
                source.open("x").write(text.replace(before, changed))
                compiled = work / (label + "-classes")
                verify.compile_local([source], compiled, classpath=originals, release=8)
                result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                    harness_prefix + "Harness", "0", "0", "0"], text=True, capture_output=True, timeout=300, env=verify.environment)
                (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
                if result.returncode == 0 or "nullary carrier invocation changed" not in result.stderr:
                    raise RuntimeError("Frozen scalar factory observer missed " + label)
        if args.reifiable_input:
            extra_wrongs = [
                ("null-input", "Client", "Facade.make(Trace.collection)", "Facade.make(null)"),
                ("copied-input", "Client", "Facade.make(Trace.collection)", "Facade.make(new java.util.ArrayList<Object>(Trace.collection))"),
                ("inspected-input", "api/Provider", "Trace.seen = collection;", "collection.size(); Trace.seen = collection;"),
                ("late-input", "api/Provider", "Trace.seen = collection;", "Trace.seen = Trace.collection;"),
            ]
            for label, owner, before, changed in extra_wrongs:
                text = (fixture / ("fixture/" + owner + ".java")).read_text()
                if not text.count(before): raise RuntimeError("Reifiable factory counterexample anchor changed")
                source = work / label / ("fixture/" + owner + ".java")
                source.parent.mkdir(parents=True)
                source.open("x").write(text.replace(before, changed))
                compiled = work / (label + "-classes")
                verify.compile_local([source], compiled, classpath=originals, release=8)
                result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                    harness_prefix + "Harness", "0", "0", "0"], text=True, capture_output=True, timeout=300, env=verify.environment)
                (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
                if result.returncode == 0 or "nullary carrier invocation changed" not in result.stderr:
                    raise RuntimeError("Frozen reifiable factory observer missed " + label)
        if args.nested_result:
            extra_wrongs = [
                ("lost-input-call", "api/Provider", "input.touch();", ";"),
                ("repeated-input-call", "api/Provider", "input.touch();", "input.touch(); input.touch();"),
                ("invented-input-cast", "api/Provider", "input.touch();", "String unused = (String)input.touch();"),
                ("late-input", "api/Provider", "Trace.seen = input;", "Trace.seen = Trace.box;"),
                ("substituted-input", "Client", invocation, invocation.replace("Trace.box", "Trace.alternative")),
            ]
            if args.instance_nested_result:
                extra_wrongs += [
                    ("late-receiver", "api/Provider", "Trace.seenReceiver = this;", "Trace.seenReceiver = Trace.receiver;"),
                    ("substituted-receiver", "Client", "Scope.make(Trace.receiver," if args.enclosing_nested_result else "Trace.receiver.make(", "Scope.make(Trace.alternativeReceiver," if args.enclosing_nested_result else "Trace.alternativeReceiver.make("),
                ]
            if args.partial_final_constructors:
                extra_wrongs += [
                    ("lost-owned-token-store", "Scope", "this.token = Trace.box;", ";"),
                    ("early-owned-token-store", "Scope", "Trace.beforeCell(this);", "this.token = Trace.box; Trace.beforeCell(this);"),
                ]
            if args.independent_member:
                extra_wrongs += [
                    ("lost-primary-constructor-effect", "Scope", "Trace.hit(7);" if args.partial_final_constructors else "private Cell() { Trace.hit(7); }", ";" if args.partial_final_constructors else "private Cell() {}"),
                    ("repeated-primary-constructor-effect", "Scope", "Trace.hit(7);" if args.partial_final_constructors else "private Cell() { Trace.hit(7); }", "Trace.hit(7); Trace.hit(7);" if args.partial_final_constructors else "private Cell() { Trace.hit(7); Trace.hit(7); }"),
                    ("lost-constructor-store", "Scope", "this.value = value;", ";"),
                ]
            for label, owner, before, changed in extra_wrongs:
                if args.enclosing_nested_result and owner == "api/Provider": owner = "Scope"
                text = (fixture / ("fixture/" + owner + ".java")).read_text()
                if not text.count(before): raise RuntimeError("Nested factory counterexample anchor changed")
                source = work / label / ("fixture/" + owner + ".java")
                source.parent.mkdir(parents=True)
                source.open("x").write(text.replace(before, changed))
                compiled = work / (label + "-classes")
                verify.compile_local([source], compiled, classpath=originals, release=8)
                result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                    harness_prefix + "Harness", "0", "0", "0"], text=True, capture_output=True, timeout=300, env=verify.environment)
                (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
                if result.returncode == 0 or "nullary carrier invocation changed" not in result.stderr:
                    raise RuntimeError("Frozen nested factory observer missed " + label)
        if (compiler_originals and hashes(compiler_originals) != json.loads((work / "frozen-input-hashes.json").read_text())["compiler-originals"]):
            raise RuntimeError("Compiler-original member witness bytes changed")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(source_dex.read_bytes()).hexdigest() != source_dex_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
                    "paired_result": args.paired_result, "closed_object_tail": args.closed_object_tail,
                    "scalar_argument": args.scalar_argument, "reifiable_input": args.reifiable_input, "nested_result": args.nested_result, "instance_nested_result": args.instance_nested_result, "enclosing_nested_result": args.enclosing_nested_result, "independent_member": args.independent_member, "partial_final_constructors": args.partial_final_constructors,
                    "original_metadata_and_exceptions": "unchanged", "nullary_calls_and_original_mutable_or_immutable_identity": "matched",
                    "original_exception_identity": "matched", "same_code_counterexamples": "rejected",
                    "independent_bypass_and_repeated_call_counterexamples": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
