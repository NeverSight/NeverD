#!/usr/bin/env python3
"""Verify original erased bridges beneath typed unchecked Exceptions declarations."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature

BRIDGE = "Lfixture/Codec;->convert(Ljava/lang/Object;)Ljava/lang/Object;"
TARGET = "Lfixture/Codec;->convert(Ljava/lang/CharSequence;)[B"
DISPATCH = "Lfixture/Api;->convert(Ljava/lang/Object;)Ljava/lang/Object;"


def hashes(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-interface", action="store_true")
    parser.add_argument("--default-override", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-unchecked-bridge-target-throws-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "unchecked_bridge_target_throws/java"
        inputs = sorted(fixture.rglob("*.java"))
        if args.default_override:
            authored = work / "default-override-java"
            adjusted = []
            for source in inputs:
                content = source.read_text()
                if source.name == "Api.java":
                    original = "public interface Api<I, O> { O convert(I input); }"
                    if content.count(original) != 1:
                        raise RuntimeError("Missing exact owned default interface overlay")
                    content = content.replace(original,
                        "public interface Api<I, O> { O convert(I input); default void reset() {} }", 1)
                elif source.name == "Codec.java":
                    end = content.rfind("}")
                    content = content[:end] + (
                        "    @Override\n    public void reset() throws LeafFailure, FatalFailure {\n"
                        "        calls++;\n        if ((calls & 1) == 0) throw fatal;\n"
                        "        throw failure;\n    }\n") + content[end:]
                destination = authored / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                adjusted.append(destination)
            fixture, inputs = authored, adjusted
        profile = "default" if args.default_override else "bridge"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        harness_input = FIXTURES / "harness/UncheckedBridgeTargetThrowsHarness.java"
        if args.default_override:
            content = harness_input.read_text()
            begin = content.index("    private static void reset(int path, Codec codec) throws Throwable {")
            end = content.index("    private static void state(Codec codec, int expected) {", begin)
            direct = ("    @SuppressWarnings({\"rawtypes\", \"unchecked\"})\n"
                      "    private static void reset(int path, Codec codec) throws Throwable {\n"
                      "        if (path == 0) { codec.reset(); return; }\n"
                      "        if (path == 1) { ((Api<CharSequence, byte[]>) codec).reset(); return; }\n"
                      "        if (path == 2) { ((Api) codec).reset(); return; }\n"
                      "        Method method = path == 3 ? Codec.class.getDeclaredMethod(\"reset\")\n"
                      "                                     : Api.class.getDeclaredMethod(\"reset\");\n"
                      "        try { method.invoke(codec); }\n"
                      "        catch (InvocationTargetException error) { throw error.getCause(); }\n"
                      "    }\n")
            harness_input = work / "harness-input/UncheckedBridgeTargetThrowsHarness.java"
            harness_input.parent.mkdir()
            harness_input.write_text(content[:begin] + direct + content[end:])
        verify.compile_local([harness_input], harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        all_classes, all_methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        classes = all_classes - ({"Lfixture/Api;"} if args.dependency_interface else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        expected_methods = (7 if args.dependency_interface else 9) if args.default_override else (6 if args.dependency_interface else 7)
        if (len(classes), len(methods)) != ((4 if args.dependency_interface else 5), expected_methods):
            raise RuntimeError("Owned unchecked bridge inventory changed")
        dependency = work / "owned-api.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("fixture/Api.class", (originals / "fixture/Api.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_interface:
            classpath += ["--classpath", dependency]

        def dex(root: Path, output: Path) -> Path:
            paths = [p for p in sorted(root.rglob("*.class"))
                     if not (args.dependency_interface and p.relative_to(root).as_posix() == "fixture/Api.class")]
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", output, *paths], "d8-" + output.name)
            result = output / "classes.dex"
            if not result.is_file() or list(output.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned unchecked bridge fixture did not produce exactly one DEX")
            return result

        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "UncheckedBridgeTargetThrowsHarness", profile], "baseline")
        expected = [("values=10,identities=30,states=42,failures=32" if args.default_override
                     else "values=10,identities=20,states=27,failures=17"),
                    "unchecked-target-exceptions=retained,erased-interface-exceptions=empty,casts-before-effects=retained"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned unchecked bridge baseline missed an exception or dispatch case: " + baseline)
        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed the original unchecked bridge inventory")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage.get("projected_method_count", 0) != 1
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != {BRIDGE}):
            raise RuntimeError("Unchecked exception proof changed original scopes, methods or source bindings")
        if (rows[BRIDGE].get("projection_kind") != "generic-interface-bridge"
                or rows[BRIDGE].get("bridge_target") != TARGET
                or rows[BRIDGE].get("bridge_dispatch") != DISPATCH):
            raise RuntimeError("Original unchecked bridge lost its inherited dispatch or complete target")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_interface else None,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != methods | helpers:
            raise RuntimeError("Unchecked bridge recompile changed original inventory or added an unreported helper")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            wanted, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != wanted.get(key):
                    raise RuntimeError("Original unchecked bridge class metadata changed: " + owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original typed or erased Exceptions metadata changed: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "UncheckedBridgeTargetThrowsHarness", profile], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Unchecked exception identity, bridge cast order or effects changed")
        negative = work / "checked-hierarchy-originals"
        shutil.copytree(originals, negative)
        receipt = change_signature(negative / "fixture/MidFailure.class",
                                   b"java/lang/RuntimeException", b"java/lang/Exception")
        (work / "checked-hierarchy-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = dex(negative, work / "checked-hierarchy-dex")
        rejected = work / "checked-hierarchy-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                 *map(str, classpath), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "checked-hierarchy-recovery.log").write_text(result.stdout + result.stderr)
        if (result.returncode == 0 or rejected.exists()
                or not any(word in result.stderr for word in ("generic", "bridge", "throws"))):
            raise RuntimeError("Unchecked bridge proof accepted a checked exception ancestor")
        invalid = work / "checked-hierarchy-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "MidFailure.java":
                if content.count("extends RuntimeException") != 1:
                    raise RuntimeError("Missing exact owned unchecked ancestor counterexample")
                content = content.replace("extends RuntimeException", "extends Exception", 1)
            destination = invalid / source.relative_to(fixture)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "checked-hierarchy-compiler"), *compiler_inputs], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "checked-hierarchy-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or not any(word in compiler.stderr for word in ("throw", "override")):
            raise RuntimeError("Independent javac accepted the checked exception override")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original classes, immutable harness or declaration bytes changed")
        evidence = {"status": "success", "dependency_interface": args.dependency_interface,
                    "default_override": args.default_override,
                    "class_count": len(classes), "method_count": len(methods), "bridge_count": 1,
                    "value_cases": 10, "identity_cases": 30 if args.default_override else 20,
                    "state_cases": 42 if args.default_override else 27, "failure_cases": 32 if args.default_override else 17,
                    "complete_original_metadata_and_inventory": "unchanged except reported helpers",
                    "typed_unchecked_and_empty_erased_exceptions": "unchanged",
                    "bridge_casts_exception_identity_and_effect_order": "matched",
                    "same_Code_checked_hierarchy_counterexample": "rejected",
                    "independent_javac_checked_override_counterexample": "rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged", "coverage": "partial"}
        (work / "verified.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
