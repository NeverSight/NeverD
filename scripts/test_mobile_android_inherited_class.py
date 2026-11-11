#!/usr/bin/env python3
"""Verify interface contracts supplied by inherited class methods with exact composed scopes and no added bridge."""
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
        prefix="neverd-inherited-class-contracts-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "inherited_class_contracts/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness_dir = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - {"Lfixture/Contract;"}
        methods = {identity: row for identity, row in all_methods.items()
                   if identity.split("->", 1)[0] in classes}
        dependency = work / "owned-contract.jar"
        with zipfile.ZipFile(dependency, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.write(originals / "fixture/Contract.class", "fixture/Contract.class")
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        verify.compile_local([FIXTURES / "harness/InheritedClassContractsHarness.java"], harness_dir,
                             classpath=originals)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               "InheritedClassContractsHarness"], "baseline")
        if baseline.splitlines() != ["identities=21,failures=8,bridges=0",
                                     "inherited-class-contracts=retained,composed-class-scopes=matched,raw-calls-and-effects=matched"]:
            raise RuntimeError("Owned generic inheritance baseline did not exercise every expected case")
        def owned_dex(directory: Path, output: Path) -> Path:
            output.mkdir(parents=True)
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath",
                        args.android_jar.resolve(), "--classpath", dependency, "--output", output,
                        *[path for path in sorted(directory.rglob("*.class"))
                          if "L" + path.relative_to(directory).as_posix()[:-6] + ";" in classes]], "d8-" + output.name)
            dex = output / "classes.dex"
            if not dex.is_file() or list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned method-formal fixture did not produce exactly one DEX")
            return dex

        dex = owned_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the independently authored generic inheritance inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "--classpath", dependency, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage.get("projected_method_count", 0) != 0
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"] != 0 or coverage.get("generic_signature_projections")
                or coverage["status"] != "recovered"):
            raise RuntimeError("Owned generic inheritance lost an original descriptor or source formal")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if actual_classes != classes or not set(methods).issubset(actual_methods):
            raise RuntimeError("javac did not regenerate every original generic member and bridge descriptor")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir, dependency))),
                             "InheritedClassContractsHarness"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Owned source changed reflection, inherited dispatch or ordered effects")
        negative = work / "negative-originals"
        shutil.copytree(originals, negative)
        target = negative / "fixture/Root.class"
        before = target.read_bytes()
        signature = b"(TR;)Lfixture/Cell<TR;TS;>;"
        changed_signature = b"(TR;)Lfixture/Cell<TS;TR;>;"
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
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "--classpath", str(dependency), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if (result.returncode == 0 or rejected.exists()
                or "generic inheritance has no exact specialized implementation" not in result.stderr):
            raise RuntimeError("Erasure hid reordered owned method formal arguments")
        negative_sources = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "Root.java":
                declaration = "public Cell<R, S> lookup(R input)"
                if content.count(declaration) != 1:
                    raise RuntimeError("Missing owned source override declaration")
                content = content.replace(declaration, declaration.replace("Cell<R, S>", "Cell<S, R>"), 1)
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
            raise RuntimeError("Owned external method declarations changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "bridge_count": 0, "identity_cases": 21, "ordered_failure_cases": 8,
                    "inherited_class_contracts": "retained", "composed_class_scopes": "matched", "generic_reflection": "unchanged",
                    "unchanged_dependency_and_mutable_state": "matched",
                    "original_descriptors": "retained", "scope_counterexample": "rejected-by-neverd-and-javac",
                    "original_class_sha256": hashes, "coverage": "recovered", "jvm_output": "matched"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
