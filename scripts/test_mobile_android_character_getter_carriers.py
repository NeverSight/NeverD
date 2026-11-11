#!/usr/bin/env python3
"""Verify scoped char forwarding with complete sibling getter contracts."""
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
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_shared_long_bridges import shared_metadata, owner_metadata
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_shared_long_bridges import shared_metadata, owner_metadata
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory

OWNER = "Lfixture/Owner$Worker;"
BRIDGE = OWNER + "->key(Ljava/lang/Object;)Ljava/lang/Object;"


def main():
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-character-getter-carriers-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = FIXTURES / "character_getter_carriers/java"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        harness_name = "CharacterGetterCarrierHarness"
        verify.compile_local([FIXTURES / "harness" / (harness_name + ".java")], harness, classpath=originals)
        worker = originals / "fixture/Owner$Worker.class"
        edits = [shared_metadata(worker, boxed=b"Ljava/lang/Character;"),
                 owner_metadata(originals / "fixture/Owner.class"),
                 bridge_flag(worker, "key", "(Ljava/lang/Object;)Ljava/lang/Object;",
                             original_flags=0x1, input_flags=0x1041)]
        (work / "original-encoding.json").write_text(json.dumps(edits, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        records = class_identity.compiler_classes(originals)
        exceptions = exception_inventory(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        declarations = {"Lfixture/Function;", "Lfixture/View;"} if args.dependency_root else set()
        classes = all_classes - declarations
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        dependency = work / "api.jar"
        with zipfile.ZipFile(dependency, "x") as archive:
            for name in ("Function", "View"):
                archive.writestr("fixture/" + name + ".class", (originals / "fixture" / (name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()] + (["--classpath", dependency] if args.dependency_root else [])
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))), harness_name, "original"], "baseline")
        if baseline.splitlines() != ["values=336,identities=384,states=776,failures=48",
                "original-char-bits-captured-results-and-sibling-getter=retained,source-scope-differences=reported"]:
            raise RuntimeError("Independent char/getter oracle missed authored outcomes: " + baseline)

        def dex(root, destination):
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned char/getter fixture produced unexpected DEX files")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed the original char/getter inventory")
        (work / "inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"):
            raise RuntimeError("Char/getter recovery changed the input inventory")
        projections = coverage.get("generic_signature_projections", [])
        changes = [change for row in projections for change in row["changes"]]
        scoped = [change for change in changes if change["kind"] == "shared-method-type-variable"]
        if (len(scoped) != 4 or any(change["original_type"] !=
                "Lfixture/Owner;->make(Ljava/lang/Object;)Ljava/lang/Object;::S" for change in scoped)
                or sum(change["kind"] == "checked-value-carrier-interface" for change in changes) != 1):
            raise RuntimeError("Char/getter source projection lost the original scope")
        row = rows[BRIDGE]
        if (row.get("projection_kind") != "value-carrier-interface-bridge"
                or row.get("bridge_target") != OWNER + "->key(C)Ljava/lang/Object;"
                or row.get("unbox_methods") != ["Ljava/lang/Character;->charValue()C"]
                or not row.get("metadata_differences")):
            raise RuntimeError("Original char conversion lost its body proof")
        bindings = coverage.get("class_source_bindings", [])
        if (len(bindings) != 1 or bindings[0].get("class") != OWNER
                or bindings[0].get("binding_kind") != "anonymous-to-shared-member"
                or not bindings[0].get("metadata_differences")):
            raise RuntimeError("Shared anonymous scope lost its source binding")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_root else None,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        renamed = "Lfixture/Owner$" + bindings[0]["source_name"] + ";"
        def rename(identity):
            return identity.replace(OWNER, renamed)
        helpers = {rename(entry["class"] + "->" + entry["name"] + entry["prototype"])
                   for entry in coverage.get("generated_source_helpers", [])}
        if actual_classes != {rename(c) for c in classes} or set(actual_methods) != {rename(m) for m in methods} | helpers:
            raise RuntimeError("Recovered char/getter output changed an original runtime slot")
        actual_records = class_identity.compiler_classes(rebuilt)
        for name in classes:
            expected = copy.deepcopy(records[name])
            if name == OWNER:
                expected["signature"] = None
                expected["enclosing_method"] = None
                expected["inner_class"] = {"inner": renamed, "outer": "Lfixture/Owner;", "name": bindings[0]["source_name"], "access": 0x19}
                expected["fields"][OWNER + "->result:Ljava/lang/Object;"]["signature"] = None
                for identity in (OWNER + "-><init>(Ljava/lang/Object;)V", OWNER + "->key(C)Ljava/lang/Object;"):
                    expected["methods"][identity]["signature"] = None
                expected["methods"][BRIDGE]["access"] &= ~0x1040
            expected["fields"] = {rename(k): v for k, v in expected["fields"].items()}
            expected["methods"] = {rename(k): v for k, v in expected["methods"].items()}
            actual = actual_records[rename(name)]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if actual.get(key) != expected.get(key):
                    raise RuntimeError("Unreported char/getter metadata changed: " + name + ":" + key)
            for identity, method in expected["methods"].items():
                if actual["methods"][identity] != method:
                    raise RuntimeError("Unreported original method metadata changed: " + identity)
        actual_exceptions = exception_inventory(rebuilt)
        if any(actual_exceptions[rename(identity)] != exceptions[identity] for identity in methods):
            raise RuntimeError("Original exception metadata changed")
        runtime = (rebuilt, harness, dependency) if args.dependency_root else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), harness_name, "recovered"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original char conversion, capture, state or getter behavior changed")
        for label in ("result-scope", "getter-scope", "missing-synthetic"):
            negative = work / label
            shutil.copytree(originals, negative)
            path = negative / "fixture/Owner$Worker.class"
            if label == "result-scope":
                receipt = change_signature(path, b"(C)TS;", b"(C)TV;")
            elif label == "getter-scope":
                receipt = change_signature(path, b"()Lfixture/View<Ljava/lang/Character;>;", b"()Lfixture/View<Ljava/lang/Integer;>;")
            else:
                receipt = change_method_flags(path, b"(Ljava/lang/Object;)Ljava/lang/Object;", 0x1041, 0x41, name=b"key")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "-o", str(rejected)],
                                    capture_output=True, text=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Unproved char/getter metadata was accepted: " + label)
        invalid = work / "invalid/Invalid.java"
        invalid.parent.mkdir()
        invalid.write_text("import fixture.*; public final class Invalid implements Function<Character,Object> { "
                           "public Object key(Character value) { return value; } "
                           "public View<Integer> source() { return null; } }\n")
        compiler = subprocess.run([str(verify.javac), "-cp", str(originals), "-d", str(work / "invalid-classes"), str(invalid)],
                                  capture_output=True, text=True, timeout=300, env=verify.environment)
        (work / "independent-javac-rejection.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or "source" not in compiler.stderr:
            raise RuntimeError("Independent javac did not reject the conflicting sibling getter")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original input, harness, dependency or SDK changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_profile": args.dependency_root, "jvm_output": baseline.splitlines(),
                    "counterexamples": ["same-Code result scope", "same-Code getter scope", "missing synthetic", "independent javac sibling conflict"],
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
