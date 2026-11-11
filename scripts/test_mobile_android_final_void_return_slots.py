#!/usr/bin/env python3
"""Retain distinct generic void/scalar JVM slots through source and SDK ancestry."""
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
    from scripts.test_mobile_android_final_getter_return_slots import rename_owned_entry
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_final_getter_return_slots import rename_owned_entry
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code

VOID = "Lfixture/Core;->add(Ljava/lang/Object;)V"
SCALAR = "Lfixture/Base;->add(Ljava/lang/Object;)Z"


def hashes(root: Path) -> dict:
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(root.rglob("*.class"))}


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-final-void-slot-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted((FIXTURES / "final_void_return_slots/java").rglob("*.java")), originals)
        for owner in ("Core", "Caller"):
            receipt = rename_owned_entry(originals / ("fixture/" + owner + ".class"), b"append", b"add")
            (work / (owner.lower() + "-original-slot-name-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
        verify.compile_local([FIXTURES / "harness/FinalVoidReturnSlotsHarness.java"], harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        dependency_classes = {"Lfixture/Api;", "Lfixture/Base;", "Lfixture/Middle;"}
        classes = all_classes - (dependency_classes if args.dependency_parent else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        dependency = work / "owned-parents.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for owner in ("Api", "Base", "Middle"):
                archive.writestr("fixture/" + owner + ".class", (originals / ("fixture/" + owner + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_parent:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "FinalVoidReturnSlotsHarness", "add"], "baseline")
        lines = baseline.splitlines()
        match = re.fullmatch(r"values=(\d+),identities=(\d+),states=(\d+),failures=(\d+)", lines[0]) if len(lines) == 2 else None
        counts = tuple(map(int, match.groups())) if match else ()
        if counts != (56, 130, 403, 26) or lines[1] != "distinct-runtime-return-slots-and-generic-provider=retained,void-source-name=reported":
            raise RuntimeError("Owned distinct return-slot case matrix changed: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned distinct return-slot fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original distinct runtime return slot")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        named = [row for row in rows.values() if row.get("source_name")]
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 1 or coverage["status"] != "partial"
                or len(named) != 1 or named[0]["identity"] != VOID
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")):
            raise RuntimeError("Distinct void source name changed unrelated contracts or inventory")
        row = named[0]
        if (row.get("projection_kind") != "incompatible-inherited-return-slot"
                or row.get("generic_signature") != "(TT;)V" or row.get("source_name_declaration") != VOID
                or row.get("conflicting_inherited_contracts") != [SCALAR] or not row.get("metadata_differences")
                or row.get("source_identity") != "Lfixture/Core;->" + row["source_name"] + "(Ljava/lang/Object;)V"):
            raise RuntimeError("Distinct void source name lost its full original method or ancestor contract")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_parent else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        expected_methods = {entry.get("source_identity", entry["identity"]) for entry in rows.values()}
        helpers = {entry["class"] + "->" + entry["name"] + entry["prototype"]
                   for entry in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != expected_methods | helpers:
            raise RuntimeError("Rebuilt distinct void slot changed provider ownership or added a declaration")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Complete class scope or original fields changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                target = method
                if method == VOID:
                    wanted["name"] = row["source_name"]
                    target = row["source_identity"]
                if after["methods"][target] != wanted:
                    raise RuntimeError("Method metadata changed beyond the disclosed void source name: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_parent else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             "FinalVoidReturnSlotsHarness", row["source_name"]], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Distinct void/scalar effects, original calls, aliases or exception order changed")
        for label in ("wrong-ancestor-binding", "missing-final-entry"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            core = negative / "fixture/Core.class"
            if label == "wrong-ancestor-binding":
                receipt = change_signature(core, b"<T:Ljava/lang/Object;>Lfixture/Middle<TT;>;",
                                            b"<T:Ljava/lang/Object;>Lfixture/Middle<Ljava/lang/Object;>;")
            else:
                before = core.read_bytes()
                authored = work / "no-final-java/fixture/Core.java"
                authored.parent.mkdir(parents=True)
                text = (FIXTURES / "final_void_return_slots/java/fixture/Core.java").read_text()
                authored.write_text(text.replace("public final void append", "public void append"))
                temporary = work / "no-final-compiled"
                verify.compile_local([authored], temporary, classpath=originals)
                rename_owned_entry(temporary / "fixture/Core.class", b"append", b"add")
                after = (temporary / "fixture/Core.class").read_bytes()
                original_pool, changed_pool = class_records(before), class_records(after)
                if (method_code(before) != method_code(after)
                        or original_pool["classes"] != changed_pool["classes"]
                        or original_pool["strings"] != changed_pool["strings"]):
                    raise RuntimeError("Owned no-final counterexample changed Code or constant-pool binding")
                core.write_bytes(after)
                receipt = {"original_flag": "public final", "counterexample_flag": "public",
                           "independently_authored": True, "code_descriptors_and_pool_bindings": "unchanged"}
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Void source name hid an incomplete original contract: " + label)
        wrong_java = work / "wrong-java/fixture/Wrong.java"
        wrong_java.parent.mkdir(parents=True)
        wrong_java.write_text("package fixture; public final class Wrong<T> extends Middle<T> {\n"
                             "  public final void add(T item) {}\n}\n")
        rejected = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-classpath", str(originals),
                                   "-d", str(work / "wrong-compiled"), str(wrong_java)], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-javac.log").write_text(rejected.stdout + rejected.stderr)
        if rejected.returncode == 0:
            raise RuntimeError("Independent javac accepted two incompatible original source return slots")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness, ancestors or SDK changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 1, "value_cases": counts[0], "identity_cases": counts[1],
                    "state_cases": counts[2], "failure_cases": counts[3],
                    "profile": "dependency-parent" if args.dependency_parent else "source-parent",
                    "full_generic_parameters_and_original_runtime_bodies": "unchanged",
                    "inherited_scalar_provider": "unchanged ownership and dynamic dispatch",
                    "source_void_name_and_runtime_reflection_difference": "reported partial",
                    "wrong_binding_missing_final_and_javac_return_conflict": "rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
