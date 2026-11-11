#!/usr/bin/env python3
"""Verify wildcard bounds through declared inheritance with owned classpath fixtures."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_classpath import jar
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_classpath import jar
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk


def invariant_dependency(source: Path, target: Path) -> dict:
    original = b"<C::Lfixture/dependency/Root<+Lfixture/dependency/Tag;>;>Ljava/lang/Object;"
    changed = original.replace(b"<+", b"<", 1)
    old = b"\x01" + len(original).to_bytes(2, "big") + original
    new = b"\x01" + len(changed).to_bytes(2, "big") + changed
    witness = None
    with zipfile.ZipFile(source) as input_jar, zipfile.ZipFile(target, "x", zipfile.ZIP_DEFLATED) as output:
        for name in input_jar.namelist():
            data = input_jar.read(name)
            if name == "fixture/dependency/Covariant.class":
                if data.count(old) != 1:
                    raise RuntimeError("Missing exact owned covariant class bound")
                before = data
                data = data.replace(old, new, 1)
                witness = {"original_signature": original.decode(), "changed_signature": changed.decode(),
                           "original_sha256": hashlib.sha256(before).hexdigest(),
                           "changed_sha256": hashlib.sha256(data).hexdigest(),
                           "change": "one Signature UTF8 record; original executable bytes retained"}
            output.writestr(name, data)
    if witness is None:
        raise RuntimeError("Missing owned covariant dependency")
    return witness


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-wildcard-inheritance-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), args.d8.resolve(), 300)
        fixture = FIXTURES / "wildcard_inheritance"
        dependencies, originals, harness_dir = work / "dependencies", work / "originals", work / "harness"
        verify.compile_local(sorted((fixture / "dependency").rglob("*.java")), dependencies)
        library = work / "dependency.jar"
        jar(dependencies, library)
        inputs = sorted((fixture / "input").rglob("*.java"))
        verify.compile_local(inputs, originals, classpath=library)
        classes, methods = compiler_inventory(verify, originals)
        harness = FIXTURES / "harness/WildcardInheritanceHarness.java"
        verify.compile_local([harness], harness_dir,
                             classpath=Path(os.pathsep.join(map(str, (originals, library)))), release=17)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, library, harness_dir))),
                               "WildcardInheritanceHarness"], "baseline")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", library)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned wildcard input inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", library, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["unrecovered_method_count"] != 0
                or coverage.get("generic_signature_projections") or coverage["status"] != "recovered"):
            raise RuntimeError("Wildcard bound validation changed declarations or generic metadata")
        sources = sorted((output / "sources").rglob("*.java"))
        if len(sources) != len(classes):
            raise RuntimeError("Dependency classes entered the original input source denominator")
        rebuilt = work / "rebuilt"
        verify.compile_local(sources, rebuilt, classpath=library)
        _, actual_methods = compiler_inventory(verify, rebuilt)
        if not set(methods).issubset(actual_methods):
            raise RuntimeError("Wildcard source lost an original runtime descriptor")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, library, harness_dir))),
                             "WildcardInheritanceHarness"], "recovered-execution")
        if baseline != actual:
            raise RuntimeError("Wildcard source changed choices, identity, exceptions or generic reflection")
        negative = work / "invariant-dependency.jar"
        witness = invariant_dependency(library, negative)
        (work / "negative-metadata-change.json").write_text(json.dumps(witness, indent=2) + "\n")
        rejected = work / "rejected"
        command = [str(args.neverd.resolve()), "mobile", str(dex), "--classpath", str(negative), "-o", str(rejected)]
        result = subprocess.run(command, text=True, capture_output=True, timeout=300)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "type argument bound is not proven" not in result.stderr:
            raise RuntimeError("Wildcard inheritance silently became an invariant type argument")
        compiler_command = [str(verify.javac), "--release", "8", "-cp", str(negative),
                            "-d", str(work / "negative-compiler"), *map(str, inputs)]
        compiler = subprocess.run(compiler_command, text=True, capture_output=True, timeout=300)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Independent javac accepted the invariant counterexample")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "identity_choices": 54, "ordinary_reads": 36, "exact_exception_cases": 2,
                    "upper_lower_and_unbounded_paths": "matched", "mutable_identity": "matched",
                    "generic_field_and_method_reflection": "unchanged", "runtime_descriptors": "preserved",
                    "dependency_sources_emitted": 0, "invariant_counterexample": "rejected-by-neverd-and-javac"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
