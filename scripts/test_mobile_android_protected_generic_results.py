#!/usr/bin/env python3
"""Verify closed generic protected results with an original public erased bridge."""
from __future__ import annotations

import argparse
import copy
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
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory

OWNER = "Lfixture/Host$1;"
TARGET_SIGNATURE = b"(Ljava/lang/Class<*>;)Lfixture/Fn<Ljava/lang/Throwable;Ljava/lang/Throwable;>;"


def encode_anonymous(root: Path) -> list:
    receipts = []
    for name in ("Host", "Host$1"):
        path = root / ("fixture/" + name + ".class")
        before = path.read_bytes()
        shape = class_records(before)
        data = bytearray(before)
        if name == "Host$1":
            at = shape["pool_end"]
            if int.from_bytes(before[at:at + 2], "big") != 0x20:
                raise RuntimeError("Owned anonymous class flags changed")
            data[at:at + 2] = (0x31).to_bytes(2, "big")
        found = 0
        for key, start, end in shape["rows"]:
            if key != b"InnerClasses":
                continue
            count = int.from_bytes(before[start + 6:start + 8], "big")
            if end != start + 8 + count * 8:
                raise RuntimeError("Malformed owned inner class table")
            for i in range(count):
                at = start + 8 + i * 8
                inner = int.from_bytes(before[at:at + 2], "big")
                if shape["strings"][shape["classes"][inner]] != b"fixture/Host$1":
                    continue
                if int.from_bytes(before[at + 6:at + 8], "big") != 0:
                    raise RuntimeError("Owned anonymous inner flags changed")
                data[at + 6:at + 8] = (0x19).to_bytes(2, "big")
                found += 1
        if found != 1 or method_code(before) != method_code(bytes(data)):
            raise RuntimeError("Anonymous flags changed original Code or descriptors")
        path.write_bytes(data)
        receipts.append({"class": name, "public_static_final_anonymous_flags": "encoded", "Code_and_descriptors": "unchanged"})
    receipts.append(change_method_flags(root / "fixture/Host$1.class",
                    b"(Ljava/lang/Class;)Ljava/lang/Object;", 0x1044, 0x1041, name=b"compute"))
    return receipts


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-protected-generic-results-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "protected_generic_results/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        encoding = encode_anonymous(originals)
        (work / "owned-input-encoding.json").write_text(json.dumps(encoding, indent=2) + "\n")
        verify.compile_local([FIXTURES / "harness/ProtectedGenericResultsHarness.java",
                              FIXTURES / "harness/ProtectedGenericCaller.java"], harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Base;"} if args.dependency_parent else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, original_exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-parent.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("fixture/Base.class", (originals / "fixture/Base.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_parent:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                "ProtectedGenericResultsHarness", "original"], "baseline")
        if baseline.splitlines() != ["values=36,identities=540,states=366,failures=132",
                "closed-generic-signatures=retained,target-access-and-anonymous-source-binding=reported"]:
            raise RuntimeError("Owned generic result matrix changed: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned generic result fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original generic result runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        target = OWNER + "->compute(Ljava/lang/Class;)Lfixture/Fn;"
        bridge = OWNER + "->compute(Ljava/lang/Class;)Ljava/lang/Object;"
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        expected_projected = {target, bridge, OWNER + "-><init>()V", "Lfixture/Host;-><clinit>()V"}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count") != 4 or projected != expected_projected
                or coverage["status"] != "partial" or coverage.get("generic_signature_projections")):
            raise RuntimeError("Generic result access projection changed inventory or signatures")
        if (rows[target].get("projection_kind") != "public-erased-protected-generic-result-target"
                or rows[target].get("original_bridge") != bridge or rows[bridge].get("bridge_target") != target
                or rows[target].get("generic_signature") != TARGET_SIGNATURE.decode()
                or not rows[target].get("metadata_differences") or not rows[bridge].get("metadata_differences")):
            raise RuntimeError("Original bridge or generic target access change was not reported")
        bindings = coverage.get("class_source_bindings", [])
        if (len(bindings) != 1 or bindings[0].get("class") != OWNER
                or bindings[0].get("binding_kind") != "anonymous-to-named-member"
                or not bindings[0].get("metadata_differences")):
            raise RuntimeError("Anonymous generic result lacks its original source binding")
        binding = bindings[0]
        named = binding["source_enclosing_class"][:-1] + "$" + binding["source_name"] + ";"
        def rename(text: str) -> str:
            return text.replace(OWNER, named)
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_parent else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {rename(entry["class"] + "->" + entry["name"] + entry["prototype"])
                   for entry in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != {rename(c) for c in classes} or set(rebuilt_methods) != {rename(m) for m in methods} | helpers:
            raise RuntimeError("Rebuilt generic result changed entries outside reported source mapping")
        actual_records, rebuilt_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for identity in methods:
            if rebuilt_exceptions.get(rename(identity)) != original_exceptions[identity]:
                raise RuntimeError("Original generic result Exceptions metadata changed")
        for identity in classes:
            before, after = copy.deepcopy(records[identity]), actual_records[rename(identity)]
            if identity == OWNER:
                before["enclosing_method"] = None
                before["inner_class"] = {"inner": named, "outer": binding["source_enclosing_class"],
                                         "name": binding["source_name"], "access": 0x19}
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Unreported generic result class/field metadata change: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if method == target:
                    metadata["access"] = 1
                if after["methods"][rename(method)] != metadata:
                    raise RuntimeError("Original method metadata changed outside target access: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_parent else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                "ProtectedGenericResultsHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original generic result state, aliases or callback effects changed")
        for label in ("missing-synthetic", "parameter-binding", "result-binding"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "missing-synthetic":
                receipt = change_method_flags(negative / "fixture/Host$1.class",
                        b"(Ljava/lang/Class;)Ljava/lang/Object;", 0x1041, 0x41, name=b"compute")
            else:
                changed = (b"(Ljava/lang/Class<Ljava/lang/String;>;)Lfixture/Fn<Ljava/lang/Throwable;Ljava/lang/Throwable;>;"
                           if label == "parameter-binding" else
                           b"(Ljava/lang/Class<*>;)Lfixture/Fn<Ljava/lang/String;Ljava/lang/Throwable;>;")
                receipt = change_signature(negative / "fixture/Host$1.class", TARGET_SIGNATURE, changed)
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Incomplete generic result proof admitted: " + label)
        invalid = work / "wrong-result-java/fixture/Wrong.java"
        invalid.parent.mkdir(parents=True)
        invalid.write_text("package fixture; public final class Wrong extends Base<Fn<? super Throwable, ? extends Throwable>> {\n"
                           "  @Override protected Fn<String, Throwable> compute(Class<?> type) { return null; }\n}\n")
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-cp", str(originals), "-d",
                                   str(work / "wrong-result-compiler"), str(invalid)], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-result-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Independent javac accepted incompatible closed result arguments")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": coverage["projected_method_count"],
                    "value_cases": 36, "identity_cases": 540, "state_cases": 366, "failure_cases": 132,
                    "profile": "dependency-parent" if args.dependency_parent else "source-parent",
                    "original_public_erased_entry_and_closed_generic_signatures": "retained",
                    "target_access_and_anonymous_source_binding": "reported",
                    "nullable_functions_mutable_aliases_and_ordered_failures": "matched",
                    "all_other_original_metadata": "unchanged",
                    "same_Code_bridge_flags_parameter_and_result_counterexamples": "rejected",
                    "independent_javac_closed_result_conflict": "rejected",
                    "input_harness_and_sdk_bytes": "unchanged", "coverage": coverage["status"]}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
