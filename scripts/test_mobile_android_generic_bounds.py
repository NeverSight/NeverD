#!/usr/bin/env python3
"""Compare encoded type-variable bounds with independent JVM behavior and reflection."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import json
from pathlib import Path
import tempfile

try:
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk


def encode_fixture_bounds(path: Path) -> None:
    """Change only the two known UTF8 records in this owned javac fixture."""
    data = path.read_bytes()
    if data[:4] != b"\xca\xfe\xba\xbe":
        raise RuntimeError("Expected a compiled bound fixture")
    for original, encoded in (
            (b"<V:TW;W::Ljava/lang/CharSequence;>Ljava/lang/Object;",
             b"<V::TW;W::Ljava/lang/CharSequence;>Ljava/lang/Object;"),
            (b"<T:TV;>(ZTT;TT;)TT;", b"<T::TV;>(ZTT;TT;)TT;")):
        old = b"\x01" + len(original).to_bytes(2, "big") + original
        new = b"\x01" + len(encoded).to_bytes(2, "big") + encoded
        if data.count(old) != 1:
            raise RuntimeError("Expected one exact generic signature UTF8 record")
        data = data.replace(old, new, 1)
    path.write_bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-generic-bounds-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), work / "unused-d8", 300)
        fixture = FIXTURES / "generic_bounds"
        inputs = sorted((fixture / "java").rglob("*.java"))
        harness = FIXTURES / "harness/GenericBoundsHarness.java"
        originals = work / "originals"
        verify.compile_local(inputs, originals)
        classes, methods = compiler_inventory(verify, originals)
        baseline_dir = work / "baseline"
        verify.compile_local([*inputs, harness], baseline_dir)
        # Execute the same encoded bound positions as the smali input, while
        # keeping javac's independent method bytecode unchanged.
        encode_fixture_bounds(baseline_dir / "fixture/BoundBox.class")
        baseline = verify.run([verify.java, "-cp", baseline_dir, "GenericBoundsHarness"], "baseline")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", fixture / "smali", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes)
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"):
            raise RuntimeError("Bound projection changed method inventory or omitted partial status")
        projections = coverage.get("generic_signature_projections", [])
        expected = {"<V::TW;W::Ljava/lang/CharSequence;>Ljava/lang/Object;",
                    "<T::TV;>(ZTT;TT;)TT;"}
        if ({row["original_signature"] for row in projections} != expected
                or any(len(row["changes"]) != 1
                       or row["changes"][0]["kind"] != "type-variable-bound-position"
                       or not row.get("metadata_differences") for row in projections)):
            raise RuntimeError("Original bound encoding or source projection was not reported")
        sources = sorted((output / "sources").rglob("*.java"))
        rebuilt = work / "rebuilt"
        verify.compile_local([*sources, harness], rebuilt)
        _, rebuilt_methods = compiler_inventory(verify, rebuilt)
        if not set(methods).issubset(rebuilt_methods):
            raise RuntimeError("Recompiled bound projection lost original method descriptors")
        actual = verify.run([verify.java, "-cp", rebuilt, "GenericBoundsHarness"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Type-variable bounds changed JVM behavior or reflection")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "identity_probes": 32, "generic_bound_reflection": "matched",
                    "original_bound_encoding": "executed-on-jvm",
                    "method_descriptors": "preserved", "bound_encoding": "explicitly-projected"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
