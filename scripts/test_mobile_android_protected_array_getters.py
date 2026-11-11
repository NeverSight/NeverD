#!/usr/bin/env python3
"""Verify protected primitive array getters and unchanged public erased entry points."""
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
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory

METHODS = {
    "BooleanCore": "Z", "ByteCore": "B", "ShortCore": "S", "CharacterCore": "C",
    "IntegerCore": "I", "LongCore": "J", "FloatCore": "F", "DoubleCore": "D",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parent", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-protected-array-getters-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "protected_array_getters/java/fixture"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/ProtectedArrayGetterHarness.java", FIXTURES / "harness/ProtectedArrayGetterCaller.java"], harness, classpath=originals)
        encoded = [change_method_flags(originals / ("fixture/" + owner + ".class"), b"()Ljava/lang/Object;",
                                      0x1044, 0x1041, name=b"get") for owner in METHODS]
        (work / "original-bridge-flags.json").write_text(json.dumps(encoded, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Base;"} if args.dependency_parent else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        original_exceptions = exception_inventory(originals)
        dependency = work / "owned-parent.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Base",):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_parent:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "ProtectedArrayGetterHarness", "original"], "baseline")
        counts = (9376, 5480, 3456, 2736)
        if baseline.splitlines() != ["values=9376,identities=5480,states=3456,failures=2736",
                "all-eight-primitive-array-results-and-original-aliases=retained,typed-access-changes=reported"]:
            raise RuntimeError("Owned protected array getter dispatch matrix changed: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned array getter bridge fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original array getter bridge runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        targets = {"Lfixture/" + owner + ";->get()[" + element for owner, element in METHODS.items()}
        bridges = {"Lfixture/" + owner + ";->get()Ljava/lang/Object;" for owner in METHODS}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 16 or projected != targets | bridges
                or coverage["status"] != "partial" or coverage.get("generic_signature_projections")
                or coverage.get("class_source_bindings")):
            raise RuntimeError("Array getter access projection changed inventory or unrelated generic signatures")
        for owner, element in METHODS.items():
            target = "Lfixture/" + owner + ";->get()[" + element
            bridge = "Lfixture/" + owner + ";->get()Ljava/lang/Object;"
            if (rows[target].get("projection_kind") != "public-erased-protected-getter-target"
                    or rows[target].get("original_bridge") != bridge
                    or rows[bridge].get("bridge_target") != target or not rows[target].get("metadata_differences")
                    or not rows[bridge].get("metadata_differences")):
                raise RuntimeError("Original public erased entry or typed access projection was not reported: " + owner)
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_parent else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {entry["class"] + "->" + entry["name"] + entry["prototype"]
                   for entry in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != methods | helpers:
            raise RuntimeError("Rebuilt array getter access projection changed an original runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        rebuilt_exceptions = exception_inventory(rebuilt)
        for method in methods:
            if rebuilt_exceptions.get(method) != original_exceptions[method]:
                raise RuntimeError("Original array getter bridge Exceptions metadata changed")
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original array getter class or field metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if method in targets:
                    wanted["access"] = 1
                if after["methods"][method] != wanted:
                    raise RuntimeError("Original method metadata changed outside target visibility: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_parent else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             "ProtectedArrayGetterHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original array getter state, checked cast, access or callback effects changed")
        for label in ("missing-synthetic", "wrong-array-element", "wrong-array-rank"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "missing-synthetic":
                receipt = change_method_flags(negative / "fixture/BooleanCore.class", b"()Ljava/lang/Object;", 0x1041, 0x41, name=b"get")
            elif label == "wrong-array-element":
                receipt = change_signature(negative / "fixture/BooleanCore.class", b"Lfixture/Base<[Z>;", b"Lfixture/Base<[B>;")
            else:
                receipt = change_signature(negative / "fixture/BooleanCore.class", b"Lfixture/Base<[Z>;", b"Lfixture/Base<[[Z>;")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Incomplete original closed array getter access proof admitted: " + label)
        invalid = work / "wrong-parent-java/fixture/BooleanCore.java"
        invalid.parent.mkdir(parents=True)
        content = (fixture / "BooleanCore.java").read_text()
        if content.count("extends Base<boolean[]>") != 1:
            raise RuntimeError("Missing owned closed array getter counterexample")
        invalid.write_text(content.replace("extends Base<boolean[]>", "extends Base<byte[]>", 1))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-cp", str(originals), "-d",
                                   str(work / "wrong-parent-compiler"), str(invalid)], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-parent-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Independent javac accepted an incompatible closed array getter override")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 16, "value_cases": counts[0], "identity_cases": counts[1],
                    "state_cases": counts[2], "failure_cases": counts[3],
                    "profile": "dependency-parent" if args.dependency_parent else "source-parent",
                    "original_public_erased_entries_and_protected_parent_callbacks": "retained",
                    "primitive_array_kinds": len(METHODS),
                    "typed_target_access": "protected to public; reported",
                    "nullable_mutable_inputs_checked_casts_and_ordered_effects": "matched",
                    "original_inventory_and_other_metadata": "unchanged except reported private helpers",
                    "same_Code_flags_array_element_and_rank_counterexamples": "rejected",
                    "independent_javac_incompatible_array_getter_override": "rejected",
                    "input_harness_and_sdk_bytes": "unchanged", "coverage": coverage["status"]}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
