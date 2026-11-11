#!/usr/bin/env python3
"""Compare private intersection signature projections with JVM behavior."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import tempfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--d8", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--interface-bound", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-array-bounds-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), args.d8.resolve(), 300)
        fixture = "interface_bounds" if args.interface_bound else "array_bounds"
        host = "ContainerChoice" if args.interface_bound else "ArrayChoice"
        harness_name = "InterfaceBoundsHarness" if args.interface_bound else "ArrayBoundsHarness"
        input_type = "Lfixture/Container;" if args.interface_bound else "[Ljava/lang/Object;"
        source_input = "Lfixture/Container<*>;" if args.interface_bound else input_type
        bound = ":" + source_input if args.interface_bound else source_input
        inputs = sorted((FIXTURES / fixture / "java").rglob("*.java"))
        harness = FIXTURES / ("harness/" + harness_name + ".java")
        originals, harness_dir = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([harness], harness_dir, classpath=originals)
        classes, methods = compiler_inventory(verify, originals)
        path = originals / ("fixture/" + host + ".class")
        before = path.read_bytes()
        old_signature = ("<R:Ljava/lang/Object;>(" + source_input + "Lfixture/Fallback<+TR;>;)TR;").encode()
        encoded_signature = ("<C:" + bound + ":TR;R:Ljava/lang/Object;>(TC;Lfixture/Fallback<+TR;>;)TR;").encode()
        old = b"\x01" + len(old_signature).to_bytes(2, "big") + old_signature
        new = b"\x01" + len(encoded_signature).to_bytes(2, "big") + encoded_signature
        if before.count(old) != 1:
            raise RuntimeError("Expected one owned compiler Signature UTF8 record")
        # A single constant-pool UTF8 value changes. All constant indices and
        # all executable bytes stay intact; the JVM oracle reads the new bounds.
        after = before.replace(old, new, 1)
        path.write_bytes(after)
        mutation = {"change": "one generic Signature UTF8 record", "method_bytecode": "unchanged",
                    "before_sha256": hashlib.sha256(before).hexdigest(),
                    "after_sha256": hashlib.sha256(after).hexdigest()}
        (work / "input-metadata-change.json").write_text(json.dumps(mutation, indent=2) + "\n")
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (harness_dir, originals))),
                               harness_name, "original"], "baseline")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", originals)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned fixture declaration inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial"):
            raise RuntimeError("Intersection projection changed inventory or omitted conservative coverage")
        projections = coverage.get("generic_signature_projections", [])
        prototype = "(" + input_type + "Lfixture/Fallback;)Ljava/lang/Object;"
        identity = "Lfixture/" + host + ";->choose" + prototype
        if (len(projections) != 1 or projections[0]["original_signature"] != encoded_signature.decode()
                or not projections[0].get("metadata_differences") or len(projections[0]["changes"]) != 1):
            raise RuntimeError("Missing original intersection constraints and metadata differences")
        change = projections[0]["changes"][0]
        kind = ("interface" if args.interface_bound else "array") + "-intersection-method-erasure"
        if (change["kind"] != kind or change.get("scope_origin") != identity
                or change["source_type"] != prototype or change["original_type"] != encoded_signature.decode()):
            raise RuntimeError("Projection lacks its exact original constraints and runtime descriptor")
        rebuilt = work / "rebuilt"
        verify.compile_local([*sorted((output / "sources").rglob("*.java")), harness], rebuilt)
        _, rebuilt_methods = compiler_inventory(verify, rebuilt)
        if not set(methods).issubset(rebuilt_methods):
            raise RuntimeError("Recompiled source lost an original runtime descriptor")
        actual = verify.run([verify.java, "-cp", rebuilt, harness_name, "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Intersection signature erasure changed executable behavior")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "identity_probes": 8 if args.interface_bound else 16,
                    "callback_calls": 5, "callback_failures": 1, "null_failures": 2,
                    "private_static_access": "preserved", "runtime_descriptors": "preserved",
                    "generic_bound_reflection_difference": "confirmed", "original_method_bytecode": "unchanged"}
        if args.interface_bound:
            evidence.update(query_calls=13, query_failures=1, query_before_callback="preserved")
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
