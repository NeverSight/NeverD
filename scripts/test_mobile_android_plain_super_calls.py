#!/usr/bin/env python3
"""Verify closed generic super calls through plain and captured class scopes."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--generic-ancestor", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-plain-super-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        original_fixture = FIXTURES / "plain_super_calls/java"
        fixture = work / "fixture"
        for original in sorted(original_fixture.rglob("*.java")):
            text = original.read_text()
            if args.generic_ancestor:
                replacements = {
                    "Base.java": [("public class Base {", "public class Base<T> {"),
                                  ("apply(Box<Object> box, Object token,", "apply(Box<T> box, T token,")],
                    "Middle.java": [("extends Base {", "extends Base<Object> {")],
                }
                for before, after in replacements.get(original.name, []):
                    if text.count(before) != 1:
                        raise RuntimeError("Original generic ancestor anchor changed")
                    text = text.replace(before, after)
            path = fixture / original.relative_to(original_fixture)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("PlainSuperCaller", "PlainSuperHarness")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 6 or len(all_methods) != 11:
            raise RuntimeError("Owned plain superclass inventory changed")
        providers = {"Lfixture/Base;", "Lfixture/Middle;", "Lfixture/Box;"}
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        signatures = {"Lfixture/Box;": "<T:Ljava/lang/Object;>Ljava/lang/Object;"}
        if args.generic_ancestor:
            signatures.update({"Lfixture/Base;": "<T:Ljava/lang/Object;>Ljava/lang/Object;",
                               "Lfixture/Middle;": "Lfixture/Base<Ljava/lang/Object;>;"})
        for owner in all_classes:
            if records[owner]["signature"] != signatures.get(owner):
                raise RuntimeError("Original class Signature inventory changed")
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
            for branch in range(2):
                for route in range(4):
                    for mode in range(7):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "PlainSuperHarness", str(branch), str(route), str(mode)],
                            f"{label}-{branch}-{route}-{mode}")
                        expected = f"branch={branch},route={route},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Original plain superclass matrix changed: " + output)
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
            raise RuntimeError("Owned plain superclass fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed original plain superclass identities")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if (coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or {row["identity"] for row in coverage["methods"]} != methods
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage.get("minimum_java_release") != 25 or coverage.get("generic_signature_projections")):
            raise RuntimeError("Plain superclass recovery lost entries or invented generic metadata")
        bindings = coverage.get("class_source_bindings", [])
        capture = "Lfixture/Owner$Child;->this$0:Lfixture/Owner;"
        if (len(bindings) != 1 or bindings[0]["binding_kind"] != "inner-to-static-member"
                or bindings[0].get("outer_capture_field") != capture
                or not any("synthetic capture-field" in text for text in bindings[0].get("metadata_differences", []))):
            raise RuntimeError("Missing precise captured ownership disclosure")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=25,
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Plain superclass recovery invented or lost runtime entries")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            expected_fields = json.loads(json.dumps(before["fields"]))
            expected_inner = dict(before["inner_class"]) if before["inner_class"] else None
            if owner == "Lfixture/Owner$Child;":
                if expected_fields[capture]["access"] != 0x1010 or not expected_inner:
                    raise RuntimeError("Original captured field/ownership metadata changed")
                expected_fields[capture]["access"] &= ~0x1000
                expected_inner["access"] |= 8
            if after["fields"] != expected_fields or after["inner_class"] != expected_inner:
                raise RuntimeError("Plain captured scope changed beyond disclosed source metadata differences")
            for key in ("access", "signature", "superclass", "interfaces", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original plain superclass metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original plain superclass method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Superclass dispatch, original captured receiver or ordered effects changed")
        for label, before, after in (
            ("virtual-dispatch", "super.apply(box,", "this.apply(box,"),
            ("wrong-overload", "super.apply(box,", "super.apply((Object) box,"),
        ):
            wrong = work / label / "fixture/Owner.java"
            wrong.parent.mkdir(parents=True)
            text = (fixture / "fixture/Owner.java").read_text()
            if text.count(before) != 2:
                raise RuntimeError("Independent superclass counterexample anchor changed")
            wrong.write_text(text.replace(before, after))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     "PlainSuperHarness", "1", "1", "0"], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "plain superclass contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "dependency_provider": args.dependency_provider, "generic_ancestor": args.generic_ancestor,
                    "coverage": "partial",
                    "class_signature_and_method_metadata_and_exceptions": "unchanged",
                    "captured_ownership_projection": "precisely disclosed",
                    "original_super_dispatch_and_overload": "matched",
                    "ordered_effects_exceptions_and_mutable_capture_identity": "matched",
                    "independent_dispatch_and_overload_counterexamples": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
