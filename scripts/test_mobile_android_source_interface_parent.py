#!/usr/bin/env python3
"""Verify source parent interface proofs, protected same-slot overrides and external defaults."""
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
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-source-interface-parent-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "source_interface_parent/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness_dir = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        classes, methods = compiler_inventory(verify, originals)
        input_classes = set(classes)
        declarations = {"Lfixture/Contract;", "Lfixture/Trace;"}
        input_classes -= declarations
        dependency = work / "owned-default-declarations.jar"
        with zipfile.ZipFile(dependency, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(originals.rglob("*.class")):
                relative = path.relative_to(originals).as_posix()
                if "L" + relative[:-6] + ";" in declarations:
                    archive.writestr(relative, path.read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        input_methods = {identity for identity in methods if identity.split("->", 1)[0] in input_classes}

        def owned_dex(directory: Path, output: Path) -> Path:
            paths = [path for path in sorted(directory.rglob("*.class"))
                     if "L" + path.relative_to(directory).as_posix()[:-6] + ";" in input_classes]
            output.mkdir(parents=True)
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--classpath", dependency, "--output", output, *paths], "d8-" + output.name)
            dex = output / "classes.dex"
            if not dex.is_file() or list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned external interface partition did not produce exactly one DEX")
            return dex

        classpath_args = ["--classpath", args.android_jar.resolve(), "--classpath", dependency]
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        verify.compile_local([FIXTURES / "harness/SourceInterfaceParentHarness.java"], harness_dir,
                             classpath=originals)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               "SourceInterfaceParentHarness"], "baseline")
        if baseline.splitlines() != ["identities=43,failures=7,bridges=1",
                                     "parent-interface=retained,protected-same-slot=retained,external-default-dispatch=matched"]:
            raise RuntimeError("Owned generic inheritance baseline did not exercise every expected case")
        dex = owned_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != input_classes
                or {row["identity"] for row in inventory["methods"]} != input_methods):
            raise RuntimeError("D8 changed the independently authored generic inheritance inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath_args,
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != input_methods
                or coverage["class_count"] != len(input_classes) or coverage["projected_method_count"] != 1
                or coverage["method_count"] != len(input_methods) or coverage["unrecovered_method_count"] != 0 or coverage.get("generic_signature_projections")
                or coverage["status"] != "partial"):
            raise RuntimeError("Owned generic inheritance lost an original descriptor or source formal")
        for row in coverage["methods"]:
            if row["status"] == "source-projected" and (
                    row["projection_kind"] != "generic-superclass-bridge"
                    or row["bridge_target"] not in methods or row["bridge_dispatch"] not in methods):
                raise RuntimeError("Bounded generic bridge lost its checked superclass delegation")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if actual_classes != input_classes or not input_methods.issubset(actual_methods):
            raise RuntimeError("javac did not regenerate every original generic member and bridge descriptor")
        rebuilt_classpath = [rebuilt, harness_dir, dependency]
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, rebuilt_classpath)),
                             "SourceInterfaceParentHarness"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Owned source changed reflection, inherited dispatch or ordered effects")
        negative = work / "negative-originals"
        shutil.copytree(originals, negative)
        target = negative / "fixture/Child.class"
        before = target.read_bytes()
        signature = b"(TI;)Lfixture/Cell<TI;TJ;>;"
        changed_signature = b"(TJ;)Lfixture/Cell<TI;TJ;>;"
        old = b"\x01" + len(signature).to_bytes(2, "big") + signature
        new = b"\x01" + len(changed_signature).to_bytes(2, "big") + changed_signature
        if before.count(old) != 1:
            raise RuntimeError("Missing exact owned generic target Signature")
        changed = before.replace(old, new, 1)
        if method_code(before) != method_code(changed):
            raise RuntimeError("Generic scope counterexample changed Code bytes or method descriptors")
        target.write_bytes(changed)
        (work / "negative-signature.json").write_text(json.dumps(
            {"original_signature": signature.decode(), "changed_signature": changed_signature.decode(),
             "code_and_descriptors": "unchanged", "changed_sha256": hashlib.sha256(changed).hexdigest()},
            indent=2) + "\n")
        negative_dex = owned_dex(negative, work / "negative-dex")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                 *map(str, classpath_args), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if (result.returncode == 0 or rejected.exists()
                or "generic target parameter differs from the full inherited substitution" not in result.stderr):
            raise RuntimeError("Erasure hid a target parameter from the wrong owned generic formal")
        negative_sources = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "Child.java":
                declaration = "protected Cell<I, J> lookup(I key)"
                if content.count(declaration) != 1:
                    raise RuntimeError("Missing owned source override declaration")
                content = content.replace(declaration, "@Override\n    " + declaration.replace("I key", "J key"), 1)
            destination = negative_sources / source.relative_to(fixture)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or "does not override" not in compiler.stderr:
            raise RuntimeError("Independent javac did not reject the different owned formal")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted(originals.rglob("*.class"))}:
            raise RuntimeError("Original independently authored class bytes changed")
        if hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash:
            raise RuntimeError("Owned declaration JAR changed")
        evidence = {"status": "success", "class_count": len(input_classes), "method_count": len(input_methods),
                    "declaration_dependency_mode": True,
                    "declaration_jar_sha256": dependency_hash,
                    "bridge_count": 1, "identity_cases": 43, "ordered_failure_cases": 7,
                    "source_parent_interface_and_owned_scopes": "retained", "generic_reflection": "unchanged",
                    "protected_same_slot_and_external_default_dispatch": "matched",
                    "original_descriptors": "retained", "scope_counterexample": "rejected-by-neverd-and-javac",
                    "original_class_sha256": hashes, "coverage": "partial", "jvm_output": "matched"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
