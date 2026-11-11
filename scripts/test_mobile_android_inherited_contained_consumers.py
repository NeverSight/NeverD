#!/usr/bin/env python3
"""Verify contained consumer projections across complete generic inheritance paths."""
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
    from scripts.test_mobile_android_empty_original_bridge_throws import exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_empty_original_bridge_throws import exception_inventory

PROTO = "(Lfixture/Consumer;Lfixture/Consumer;)Ljava/lang/Object;"
OWNERS = {"Core": "C", "Middle": "M"}


def signature(formal, unbounded):
    return "(Lfixture/Consumer<-T" + formal + ";>;Lfixture/Consumer<" + ("*" if unbounded else "-Lfixture/Value;") + ">;)Ljava/lang/Object;"


def member(owner):
    return "Lfixture/" + owner + ";->request" + PROTO


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-inherited-contained-consumers-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "inherited_contained_consumers/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/InheritedContainedConsumersHarness.java"], harness, classpath=originals)
        edits = []
        for owner, formal in OWNERS.items():
            edits.append(change_signature(originals / ("fixture/" + owner + ".class"),
                         signature(formal, False).encode(), signature(formal, True).encode()))
        (work / "original-wildcard-encoding.json").write_text(json.dumps(edits, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        declarations = {"Lfixture/" + name + ";" for name in ("Root", "Consumer", "Value", "Base")}
        classes = all_classes - (declarations if args.dependency_root else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        original_exceptions = exception_inventory(originals)
        dependency = work / "owned-api.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Root", "Consumer", "Value", "Base"):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_root:
            classpath += ["--classpath", dependency]
        counts = (144, 264, 416, 80)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "InheritedContainedConsumersHarness", "original"], "baseline")
        if baseline.splitlines() != ["values=144,identities=264,states=416,failures=80",
                "complete-scoped-ancestry-and-original-dispatch=retained,consumer-projections=reported"]:
            raise RuntimeError("Owned inherited consumer case matrix changed: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned inherited consumer fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original inherited consumer runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projections = coverage.get("generic_signature_projections", [])
        targets = {member(owner) for owner in OWNERS}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 0 or coverage["status"] != "partial"
                or len(projections) != 2 or coverage.get("class_source_bindings")
                or {p.get("declaration") for p in projections} != targets):
            raise RuntimeError("Inherited consumer projection changed inventory or unrelated declarations")
        for projection in projections:
            owner = projection["declaration"].split(";", 1)[0].split("/")[-1]
            changes = projection.get("changes", [])
            witness = member("Middle" if owner == "Core" else "Root")
            if (projection.get("original_signature") != signature(OWNERS[owner], True)
                    or not projection.get("metadata_differences") or len(changes) != 1
                    or changes[0].get("kind") != "inherited-contained-consumer-parameter"
                    or changes[0].get("source_type") != signature(OWNERS[owner], False)
                    or changes[0].get("scope_origin") != witness):
                raise RuntimeError("Inherited consumer report lost an original contract or source projection")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_root else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {entry["class"] + "->" + entry["name"] + entry["prototype"]
                   for entry in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != methods | helpers:
            raise RuntimeError("Recovered inherited consumer invented or renamed a runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        rebuilt_exceptions = exception_inventory(rebuilt)
        for method in methods:
            if rebuilt_exceptions.get(method) != original_exceptions[method]:
                raise RuntimeError("Original constant member Exceptions metadata changed")
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original scoped class or field metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if method in targets:
                    owner = identity.split("/")[-1][:-1]
                    wanted["signature"] = signature(OWNERS[owner], False)
                if after["methods"][method] != wanted:
                    raise RuntimeError("Original method metadata changed beyond the reported source Signature: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_root else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             "InheritedContainedConsumersHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original scoped dispatch, mutable aliases or failure ordering changed")
        for label, owner, wrong in (
            ("partial-input", "Core", signature("C", True).replace("<-TC;>", "<TC;>")),
            ("upper-output", "Middle", signature("M", False).replace("<-Lfixture/Value;>", "<+Lfixture/Value;>")),
            ("method-formal", "Core", "<R:Ljava/lang/Object;>" + signature("C", True)),
        ):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / ("fixture/" + owner + ".class"), signature(OWNERS[owner], True).encode(), wrong.encode())
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Inherited consumer projection admitted an incompatible original contract: " + label)
        wrong_java = work / "wrong-java/fixture/Wrong.java"
        wrong_java.parent.mkdir(parents=True)
        wrong_java.write_text("package fixture; public interface Wrong<T> extends Root<T> {\n"
                              "  Object request(Consumer<? super T> input, Consumer<?> output);\n}\n")
        rejected = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-classpath", str(originals),
                                   "-d", str(work / "wrong-compiled"), str(wrong_java)], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-javac.log").write_text(rejected.stdout + rejected.stderr)
        if rejected.returncode == 0:
            raise RuntimeError("Independent javac accepted the conflicting consumer override")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 0, "generic_signature_projections": 2, "value_cases": counts[0], "identity_cases": counts[1],
                    "state_cases": counts[2], "failure_cases": counts[3],
                    "profile": "dependency-root" if args.dependency_root else "source-root",
                    "complete_scoped_ancestry": "original formals, superclass, interface paths and runtime inventory retained",
                    "consumer_parameter_source_signatures": "only the two reported wildcard projections",
                    "mutable_aliases_and_ordered_failures": "matched across direct, inherited, subclass, reflection and MethodHandle routes",
                    "same_code_and_descriptor_counterexamples": "partial input, upper output and method formal rejected",
                    "independent_javac_counterexample": "conflicting wildcard override rejected",
                    "complete_metadata_and_exceptions": "unchanged apart from the two reported method Signatures",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged", "coverage": coverage["status"]}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
