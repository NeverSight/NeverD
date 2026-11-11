#!/usr/bin/env python3
"""Check typed interface witnesses for missing adapter signature variables."""
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


def encode_fixture_signatures(path: Path) -> None:
    """Retain javac's method bytecode while encoding the owned metadata defect."""
    data = path.read_bytes()
    for original, encoded in (
            (b"Lfixture/Callback<Lfixture/Input;Lfixture/Signal<-Lfixture/Output;>;Ljava/lang/Object;>;",
             b"Lfixture/Callback<TP;Lfixture/Signal<-TR;>;Ljava/lang/Object;>;"),
            (b"(Lfixture/Callback<-Lfixture/Input;-Lfixture/Signal<-Lfixture/Output;>;+Ljava/lang/Object;>;)V",
             b"(Lfixture/Callback<-TP;-Lfixture/Signal<-TR;>;+Ljava/lang/Object;>;)V")):
        old = b"\x01" + len(original).to_bytes(2, "big") + original
        new = b"\x01" + len(encoded).to_bytes(2, "big") + encoded
        if data.count(old) != 1:
            raise RuntimeError("Expected one exact adapter signature UTF8 record")
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-typed-adapter-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), work / "unused-d8", 300)
        fixture = FIXTURES / "typed_adapters"
        inputs = sorted((fixture / "java").rglob("*.java"))
        harness = FIXTURES / "harness/TypedAdapterHarness.java"
        originals = work / "originals"
        verify.compile_local(inputs, originals)
        classes, methods = compiler_inventory(verify, originals)
        baseline_dir = work / "baseline"
        verify.compile_local([*inputs, harness], baseline_dir)
        encode_fixture_signatures(baseline_dir / "fixture/AdapterHost$Adapter.class")
        baseline = verify.run([verify.java, "-cp", baseline_dir, "TypedAdapterHarness",
                               "fixture.AdapterHost$Adapter", "original"], "baseline")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", fixture / "smali", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial"):
            raise RuntimeError("Typed adapter changed inventory or omitted conservative status")
        projections = coverage.get("generic_signature_projections", [])
        if len(projections) != 2 or any(len(row["changes"]) != 2 for row in projections):
            raise RuntimeError("Adapter field and constructor changes are not explicit")
        witness = "Lfixture/Task;->run(Lfixture/Input;Lfixture/Signal;)Ljava/lang/Object;"
        for row in projections:
            if (not row.get("metadata_differences")
                    or {change["source_type"] for change in row["changes"]}
                    != {"Lfixture/Input;", "Lfixture/Output;"}
                    or any(change["kind"] != "adapter-captured-type-variable" or change.get("scope_origin") != witness
                           for change in row["changes"])):
                raise RuntimeError("Adapter variables have no exact typed interface witness")
        bindings = coverage["class_source_bindings"]
        if (len(bindings) != 1 or bindings[0]["binding_kind"] != "anonymous-to-named-member"
                or not bindings[0].get("metadata_differences")):
            raise RuntimeError("Named adapter projection was not reported")
        binding = bindings[0]
        replacement = binding["source_enclosing_class"][:-1] + "$" + binding["source_name"] + ";"
        rebuilt = work / "rebuilt"
        verify.compile_local([*sorted((output / "sources").rglob("*.java")), harness], rebuilt)
        _, rebuilt_methods = compiler_inventory(verify, rebuilt)
        expected = {method.replace(binding["class"], replacement) for method in methods}
        if not expected.issubset(rebuilt_methods):
            raise RuntimeError("Recompiled source lost original descriptors after the reported class rename")
        actual = verify.run([verify.java, "-cp", rebuilt, "TypedAdapterHarness",
                             replacement[1:-1].replace("/", "."), "recovered"], "recovered-execution")
        before, after = baseline.splitlines(), actual.splitlines()
        if before[-1] != "generic=unresolved" or after[-1] != "generic=typed" or before[:-1] != after[:-1]:
            raise RuntimeError("Typed adapter changed behavior or disagreed with its metadata projection")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "forwarding_probes": 8, "callback_count_and_identity": "matched",
                    "argument_and_result_identity": "matched", "exceptions_and_null_callback": "matched",
                    "generic_binding": "proven-by-original-interface", "generic_reflection_difference": "confirmed",
                    "original_descriptors": "retained after reported class rename"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
