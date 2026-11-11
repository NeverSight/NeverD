#!/usr/bin/env python3
"""Check dependency JAR binding with owned fixtures and independent JVM execution."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import json
import os
from pathlib import Path
import tempfile
import zipfile

try:
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk


def jar(classes: Path, target: Path, *, omit_member: bool = False):
    with zipfile.ZipFile(target, "x", zipfile.ZIP_DEFLATED) as output:
        for path in sorted(classes.rglob("*.class")):
            if omit_member and path.name == "Base$Member.class":
                continue
            output.write(path, path.relative_to(classes).as_posix())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--android-jar", type=Path)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-classpath-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixtures = FIXTURES / "classpath"
        dependencies = work / "dependencies"
        verify.compile_local(sorted((fixtures / "dependency").rglob("*.java")), dependencies)
        library = work / "dependency.jar"
        jar(dependencies, library)
        originals = work / "originals"
        verify.compile_local(sorted((fixtures / "input").rglob("*.java")), originals,
                             classpath=library)
        classes, methods = compiler_inventory(verify, originals)
        harness = work / "harness"
        verify.compile_local([fixtures / "ClasspathHarness.java"], harness,
                             classpath=Path(os.pathsep.join(map(str, [library, originals]))))
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, [
            originals, library, harness])), "ClasspathHarness"], "baseline")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", library)
        output = work / "recovered"
        command = [args.neverd.resolve(), "mobile", dex, "-o", output,
                   "--classpath", library]
        if args.android_jar:
            command += ["--classpath", args.android_jar.resolve()]
        verify.run(command, "recover")
        report = json.loads((output / "report.json").read_text())
        coverage = report["android_method_recovery"]
        if (coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods)
                or coverage["recovered_method_count"] != len(methods)
                or {r["identity"] for r in coverage["methods"]} != set(methods)):
            raise RuntimeError("Dependency methods contaminated input coverage")
        sources = sorted((output / "sources").rglob("*.java"))
        if len(sources) != len(classes):
            raise RuntimeError("Dependency sources were published as recovered input")
        rebuilt = work / "rebuilt"
        verify.compile_local(sources, rebuilt, classpath=library)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, [
            rebuilt, library, harness])), "ClasspathHarness"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Classpath recovery changed independent JVM results")
        incomplete = work / "incomplete.jar"
        jar(dependencies, incomplete, omit_member=True)
        import subprocess
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(dex),
                                 "-o", str(rejected), "--classpath", str(incomplete)],
                                text=True, capture_output=True, timeout=300)
        (work / "incomplete.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists():
            raise RuntimeError("Missing dependency member declaration was silently accepted")
        evidence = {"status": "success", "class_count": len(classes),
                    "method_count": len(methods), "dependency_sources_emitted": 0,
                    "jvm_output": "matched", "missing_member": "rejected",
                    "android_sdk": bool(args.android_jar)}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
