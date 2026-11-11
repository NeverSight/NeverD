#!/usr/bin/env python3
"""Verify independent method scopes on original raw interface members."""
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
    from scripts.test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from test_mobile_android_nested_return import change_signature

PROTO = "([Ljava/lang/Object;Ljava/lang/Object;)[Ljava/lang/Object;"
METHOD = "Lfixture/Core;->array" + PROTO
CONTRACT = "Lfixture/Parent;->array" + PROTO
SIGNATURE = "<U:Ljava/lang/Object;V:Ljava/lang/Object;>([TU;TV;)[TU;"


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
    parser.add_argument("--dependency-root", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-raw-method-formals-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "raw_method_formal_members/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/RawMethodFormalMembersHarness.java"], harness, classpath=originals)
        receipt = add_parameter_signature(originals / "fixture/Core.class", SIGNATURE,
                                           method_name="array", prototype=PROTO)
        (work / "original-method-scope-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Api;", "Lfixture/Parent;"} if args.dependency_root else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        if records["Lfixture/Core;"].get("signature") is not None:
            raise RuntimeError("Owned raw interface owner acquired class metadata")
        dependency = work / "owned-api.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Api", "Parent"):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_root:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "RawMethodFormalMembersHarness", "original"], "baseline")
        lines = baseline.splitlines()
        match = re.fullmatch(r"values=(\d+),identities=(\d+),states=(\d+),failures=(\d+)", lines[0]) if len(lines) == 2 else None
        counts = tuple(map(int, match.groups())) if match else ()
        if counts != (45, 432, 432, 18) or lines[1] != "raw-interface-and-runtime-body=retained,independent-method-formal-source-erasure=reported":
            raise RuntimeError("Owned raw method baseline changed its independent case matrix: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned raw method fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original raw method runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projections = coverage.get("generic_signature_projections", [])
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 1 or coverage["status"] != "partial"
                or len(projections) != 1 or coverage.get("class_source_bindings")
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != {METHOD}):
            raise RuntimeError("Raw method erasure changed inventory or added an unrelated projection")
        row = rows[METHOD]
        changes = projections[0].get("changes", [])
        if (row.get("projection_kind") != "raw-interface-method-formal-erasure"
                or row.get("inherited_class_contract") != CONTRACT or row.get("source_identity") != METHOD
                or row.get("original_method_signature") != SIGNATURE or row.get("source_generic_signature") != PROTO
                or not row.get("metadata_differences") or projections[0].get("declaration") != METHOD
                or projections[0].get("original_signature") != SIGNATURE or len(changes) != 1
                or changes[0].get("kind") != "raw-interface-method-formal-erasure"
                or changes[0].get("source_type") != PROTO or changes[0].get("scope_origin") != CONTRACT):
            raise RuntimeError("Raw method report lost its complete original scope or erased source subsignature")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_root else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {entry["class"] + "->" + entry["name"] + entry["prototype"]
                   for entry in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != methods | helpers:
            raise RuntimeError("Recovered raw method invented or renamed a runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Raw owner or original generic interface metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if method == METHOD:
                    wanted["signature"] = None
                if after["methods"][method] != wanted:
                    raise RuntimeError("Original method metadata changed beyond the reported source Signature: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_root else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             "RawMethodFormalMembersHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Raw method effects, aliases, default dispatch or exception identity changed")
        for label, wrong in (
            ("wrong-result-formal", "<U:Ljava/lang/Object;V:Ljava/lang/Object;>([TU;TV;)[TV;"),
            ("wrong-tag-formal", "<U:Ljava/lang/Object;V:Ljava/lang/Object;>([TU;TU;)[TU;"),
        ):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Core.class", SIGNATURE.encode(), wrong.encode())
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Raw method erasure hid different original method scope: " + label)
        wrong_java = work / "wrong-java/fixture/Wrong.java"
        wrong_java.parent.mkdir(parents=True)
        wrong_java.write_text("package fixture; public abstract class Wrong implements Api<Object> {\n"
                             "  public <U,V> V[] array(U[] input, V tag) { return null; }\n}\n")
        rejected = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-classpath", str(originals),
                                   "-d", str(work / "wrong-compiled"), str(wrong_java)], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-javac.log").write_text(rejected.stdout + rejected.stderr)
        if rejected.returncode == 0:
            raise RuntimeError("Independent javac accepted a different original method result scope")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 1, "value_cases": counts[0], "identity_cases": counts[1],
                    "state_cases": counts[2], "failure_cases": counts[3],
                    "profile": "dependency-root" if args.dependency_root else "source-root",
                    "original_raw_owner_and_runtime_body": "unchanged",
                    "independent_method_formal_source_signature": "erasure reported after complete alpha-equivalent contract proof",
                    "same_code_and_descriptor_counterexamples": "wrong result/tag method scopes rejected",
                    "independent_javac_counterexample": "wrong result method scope rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
