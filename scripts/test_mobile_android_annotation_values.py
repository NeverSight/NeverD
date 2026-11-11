#!/usr/bin/env python3
"""Compare D8 annotation/default and blank-final recovery with JVM reflection."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import json
import os
from pathlib import Path
import shutil
import tempfile

try:
    from scripts.test_mobile_android_internal import (
        FIXTURES, Verify, choose_d8, choose_jdk,
    )
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from test_mobile_android_internal import (
        FIXTURES, Verify, choose_d8, choose_jdk,
    )


def compiler_inventory(verify: Verify, directory: Path):
    """Read the JDK's method descriptors without interpreting annotation values."""
    classes, methods = set(), {}
    for path in sorted(directory.rglob("*.class")):
        name = path.relative_to(directory).with_suffix("").as_posix()
        owner = "L" + name + ";"
        classes.add(owner)
        dump = verify.run([Path(verify.java).with_name("javap"), "-classpath",
                           directory, "-p", "-s", name.replace("/", ".")], "javap-inventory")
        declaration = ""
        for raw in dump.splitlines():
            line = raw.strip()
            if line.startswith("descriptor:"):
                descriptor = line.split(":", 1)[1].strip()
                if not descriptor.startswith("("):
                    continue
                member = declaration.split("(", 1)[0].split()[-1]
                if member == name.replace("/", "."):
                    member = "<init>"
                if declaration == "static {};":
                    member = "<clinit>"
                identity = owner + "->" + member + descriptor
                if identity in methods:
                    raise RuntimeError("Duplicate javap method: " + identity)
                methods[identity] = "declaration" if any(
                    flag in declaration.split() for flag in ("abstract", "native")) else "body"
            elif line.endswith(";"):
                declaration = line
    if not classes or not methods:
        raise RuntimeError("Empty compiler inventory")
    return classes, methods


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--initializers", action="store_true",
                        help="Check annotation companion initialization and cyclic field reads")
    modes.add_argument("--targets", action="store_true",
                       help="Check Java module and record-component annotation target metadata")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-annotation-values-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals = work / "originals"
        fixture = ("annotation_initializers" if args.initializers else
                   "annotation_targets" if args.targets else "values")
        release = 17 if args.targets else 8
        inputs = sorted((FIXTURES / fixture / "java").rglob("*.java"))
        harness_name = ("AnnotationInitializersHarness" if args.initializers else
                        "AnnotationTargetsHarness" if args.targets else "AnnotationValuesHarness")
        harness = FIXTURES / ("harness/" + harness_name + ".java")
        if args.initializers:
            # Force javac to emit a real field read, then link it to the actual
            # constant-bearing annotation. The compile-only declaration never
            # enters the DEX, original runtime, or recovered compiler classpath.
            compile_only = work / "compile-only"
            verify.compile_local(sorted((FIXTURES / fixture / "compile_only").rglob("*.java")), compile_only)
            reader_classes = work / "reader-classes"
            verify.compile_local([source for source in inputs if source.name == "Trace.java"],
                                 reader_classes, classpath=compile_only)
            reader_dump = verify.run([Path(verify.java).with_name("javap"), "-c", "-classpath",
                                      reader_classes, "fixture.Trace"], "original-field-read")
            if "getstatic" not in reader_dump or "fixture/Units.Z_VALUE:I" not in reader_dump:
                raise RuntimeError("Compiler oracle did not preserve the original field read")
            verify.compile_local([source for source in inputs if source.name != "Trace.java"],
                                 originals, classpath=reader_classes)
            shutil.copy2(reader_classes / "fixture/Trace.class", originals / "fixture/Trace.class")
        else:
            verify.compile_local(inputs, originals, release=release)
        expected_classes, expected_methods = compiler_inventory(verify, originals)
        original_run = work / "original-run"
        if args.initializers:
            verify.compile_local([harness], original_run, classpath=originals)
        else:
            verify.compile_local([*inputs, harness], original_run, release=release)
        baseline_cp = os.pathsep.join(map(str, (original_run, originals)))
        baseline = verify.run([verify.java, "-cp", baseline_cp,
                               harness_name], "original-reflection")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", originals)
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "-o", output], "recover")
        report = json.loads((output / "report.json").read_text())
        coverage = report["android_method_recovery"]
        rows = coverage["methods"]
        if ({row["identity"] for row in rows} != set(expected_methods)
                or {row["class"] for row in rows} != expected_classes
                or coverage["unrecovered_method_count"] != 0):
            raise RuntimeError("Recovery changed the independently compiled method inventory")
        for row in rows:
            expected = "declaration-only" if expected_methods[row["identity"]] != "body" else "recovered"
            if args.initializers and row["identity"] == "Lfixture/Units;-><clinit>()V":
                expected = "source-projected"
                if row.get("projection_kind") != "annotation-field-initializer":
                    raise RuntimeError("Annotation class initializer has no field projection binding")
            if row["status"] != expected:
                raise RuntimeError("Wrong recovery status: " + row["identity"])
        generated = sorted((output / "sources").rglob("*.java"))
        rebuilt = work / "rebuilt"
        if args.targets and coverage.get("minimum_java_release") != 16:
            raise RuntimeError("New annotation targets did not set the minimum Java release")
        verify.compile_local([*generated, harness], rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        actual = verify.run([verify.java, "-cp", rebuilt,
                             harness_name], "recovered-reflection")
        if actual != baseline:
            raise RuntimeError("Recovered annotations or field initialization changed behavior")
        evidence = {"status": "success", "class_count": len(expected_classes),
                    "method_count": len(expected_methods), "reflection_sites": 5,
                    "integer_probes": 5, "final_reference_identity": "matched"}
        if args.initializers:
            if coverage.get("status") != "partial" or coverage.get("projected_method_count") != 1:
                raise RuntimeError("Annotation initializer projection was reported as complete recovery")
            _, rebuilt_methods = compiler_inventory(verify, rebuilt)
            if not expected_methods.keys() <= rebuilt_methods.keys():
                raise RuntimeError("Generated annotation source lost an original method descriptor")
            evidence = {"status": "success", "class_count": len(expected_classes),
                        "method_count": len(expected_methods), "coverage": "partial",
                        "annotation_default": "matched", "initialization_effects": "matched",
                        "cyclic_initialization_values": "matched", "singleton_identity": "matched"}
        if args.targets:
            evidence = {"status": "success", "class_count": len(expected_classes),
                        "method_count": len(expected_methods), "minimum_java_release": 16,
                        "annotation_targets": "matched", "annotation_defaults": "matched"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
