#!/usr/bin/env python3
"""Verify exact enclosing generic scopes in captured anonymous superclass overrides."""
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

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path, required=True, help="JDK 25 or later")
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-enclosed-inheritance-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "enclosed_inheritance/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness_dir = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        classes, methods = compiler_inventory(verify, originals)
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        verify.compile_local([FIXTURES / "harness/EnclosedInheritanceHarness.java"], harness_dir,
                             classpath=originals)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               "EnclosedInheritanceHarness"], "baseline")
        if baseline.splitlines() != ["identities=75,failures=18,bridges=1",
                                     "enclosing-method-formals=retained,class-formal-shadowing=matched,capture-and-cast-order=matched",
                                     "class-reflection=original-anonymous"]:
            raise RuntimeError("Owned enclosed generic baseline missed an expected scope or ordered failure")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", args.android_jar.resolve())
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the independently authored enclosing-scope inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["projected_method_count"] != 9
                or coverage["unrecovered_method_count"] != 0 or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release") != 25 or coverage["status"] != "partial"):
            raise RuntimeError("Enclosed generic source lost inventory, formals or pre-super capture ordering")
        for row in coverage["methods"]:
            if row["status"] != "source-projected":
                continue
            if row["projection_kind"] == "anonymous-to-named-local":
                continue
            if (row["projection_kind"] != "generic-superclass-bridge"
                    or row["bridge_target"] not in methods or row["bridge_dispatch"] not in methods):
                raise RuntimeError("Bounded enclosed bridge lost its exact original delegation")
        bindings = coverage["class_source_bindings"]
        if (len(bindings) != 2 or any(row["binding_kind"] != "anonymous-to-named-local"
                                    or row["binary_name_status"] != "changed"
                                    or len(row["metadata_differences"]) < 4 for row in bindings)):
            raise RuntimeError("Anonymous class identity and reflection differences were not reported")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=25)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        replacements = {}
        for binding in bindings:
            owner = binding["enclosing_method"]["owner"][:-1]
            candidates = [name for name in actual_classes
                          if name.startswith(owner + "$") and name.endswith(binding["source_name"] + ";")]
            if len(candidates) != 1:
                raise RuntimeError("Reported named local class does not identify one recompiled binary")
            replacements[binding["class"]] = candidates[0]

        def mapped(text: str) -> str:
            for original, replacement in replacements.items():
                text = text.replace(original, replacement)
            return text

        if ({mapped(name) for name in classes} != actual_classes
                or not {mapped(name) for name in methods}.issubset(actual_methods)):
            raise RuntimeError("Recompiled source lost original descriptors after the reported local class mapping")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir))),
                             "EnclosedInheritanceHarness", "source"], "recovered-execution")
        if actual.splitlines() != [*baseline.splitlines()[:-1], "class-reflection=reported-named-local"]:
            raise RuntimeError("Enclosed source changed formal ownership, captures, bridge order or reported reflection")
        negative = work / "negative-originals"
        shutil.copytree(originals, negative)
        target = negative / "fixture/Factory$1.class"
        before = target.read_bytes()
        signature = b"(TT;Lfixture/Option;)TT;"
        changed_signature = b"(TU;Lfixture/Option;)TT;"
        old = b"\x01" + len(signature).to_bytes(2, "big") + signature
        new = b"\x01" + len(changed_signature).to_bytes(2, "big") + changed_signature
        if before.count(old) != 1:
            raise RuntimeError("Missing exact owned enclosing-scope Signature")
        changed = before.replace(old, new, 1)
        if method_code(before) != method_code(changed):
            raise RuntimeError("Enclosing-scope counterexample changed method Code or descriptors")
        target.write_bytes(changed)
        (work / "negative-signature.json").write_text(json.dumps(
            {"original_signature": signature.decode(), "changed_signature": changed_signature.decode(),
             "code_and_descriptors": "unchanged"}, indent=2) + "\n")
        negative_dex = verify.dex(sorted(negative.rglob("*.class")), work / "negative-dex",
                                  args.android_jar.resolve())
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if (result.returncode == 0 or rejected.exists()
                or "generic target parameter differs from the full inherited substitution" not in result.stderr):
            raise RuntimeError("Erasure hid the wrong enclosing method formal")
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "Factory.java":
                declaration = "public T echo(T value, Option option)"
                if content.count(declaration) != 2:
                    raise RuntimeError("Missing exact owned anonymous override declarations")
                content = content.replace(declaration, declaration.replace("T value", "U value"), 1)
            destination = work / "negative-java" / source.relative_to(fixture)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or "does not override" not in compiler.stderr:
            raise RuntimeError("Independent javac did not reject the wrong enclosing formal")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted(originals.rglob("*.class"))}:
            raise RuntimeError("Original independently authored class bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "bridge_count": 1, "identity_cases": 75, "ordered_failure_cases": 18,
                    "enclosing_formals_shadowing_and_bounds": "retained", "capture_and_bridge_cast_order": "matched",
                    "class_reflection_and_binary_name_differences": "reported-and-confirmed",
                    "original_descriptors": "retained after reported class mapping", "minimum_java_release": 25,
                    "scope_counterexample": "rejected-by-neverd-and-javac", "original_class_sha256": hashes,
                    "coverage": "partial", "jvm_output": "matched"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
