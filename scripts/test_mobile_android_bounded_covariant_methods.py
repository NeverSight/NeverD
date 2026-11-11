#!/usr/bin/env python3
"""Compare original bounded covariant methods and interface bridge projections."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def omit_owned_interface_bridge(path: Path, identity: bytes = b"make(Lfixture/Pair;)Lfixture/ResultBase;",
                                *, original_flags: int = 0x1041) -> dict:
    """Remove only one owned compiler method, preserving every other Code attribute."""
    original = path.read_bytes()
    records = class_records(original)
    offset = records["pool_end"] + 6
    offset += 2 + 2 * int.from_bytes(original[offset:offset + 2], "big")
    def skip_attributes(position: int, size: int) -> int:
        for _ in range(size):
            position += 6 + int.from_bytes(original[position + 2:position + 6], "big")
        return position
    fields = int.from_bytes(original[offset:offset + 2], "big")
    offset += 2
    for _ in range(fields):
        offset = skip_attributes(offset + 8, int.from_bytes(original[offset + 6:offset + 8], "big"))
    count_at = offset
    count = int.from_bytes(original[offset:offset + 2], "big")
    offset += 2
    selected = []
    for _ in range(count):
        start = offset
        flags, name, prototype, size = struct.unpack_from(">HHHH", original, offset)
        offset = skip_attributes(offset + 8, size)
        if records["strings"][name] + records["strings"][prototype] == identity:
            if original_flags not in (0x1041, 0x1044) or flags != original_flags:
                raise RuntimeError("Owned compiler bridge flags changed")
            selected.append((start, offset))
    if len(selected) != 1:
        raise RuntimeError("Owned class lacks one exact compiler bridge")
    start, end = selected[0]
    changed = (original[:count_at] + (count - 1).to_bytes(2, "big") +
               original[count_at + 2:start] + original[end:])
    before, after = method_code(original), method_code(changed)
    if set(before) - set(after) != {identity} or any(before[key] != after[key] for key in after):
        raise RuntimeError("Owned compiler method omission changed another body or descriptor")
    if class_records(changed)["strings"] != records["strings"]:
        raise RuntimeError("Owned compiler method omission changed Signature or constants")
    path.write_bytes(changed)
    return {"removed_original_compiler_method": identity.decode(), "other_code_and_descriptors": "unchanged",
            "constant_pool_and_metadata": "unchanged", "owned_input_edit": "explicit owned compiler bridge omission"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--omit-interface-bridge", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-bounded-covariant-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "bounded_covariant_methods/java"
        inputs = sorted(fixture.rglob("*.java"))
        inputs += [FIXTURES / ("nominal_method_formal_inheritance/java/fixture/" + name + ".java")
                   for name in ("Bound", "OtherBound", "Pair")]
        inputs.append(FIXTURES / "value_carrier_bridges/java/fixture/Trace.java")
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/BoundedCovariantMethodHarness.java"], harness, classpath=originals)
        if args.omit_interface_bridge:
            receipt = omit_owned_interface_bridge(originals / "fixture/Contract.class")
            (work / "owned-interface-method-edit.json").write_text(json.dumps(receipt, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "BoundedCovariantMethodHarness", "omitted" if args.omit_interface_bridge else "original"], "baseline")
        expected = ["identities=103,failures=6,bridges=2",
                    "bounded-covariant-scopes=retained,original-bridge-dispatch-and-identity=matched"]
        if baseline.splitlines() != expected + ["interface-bridge-count=" + str(int(not args.omit_interface_bridge))]:
            raise RuntimeError("Owned bounded covariant baseline missed cases")
        def make_dex(source: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", destination, *sorted(source.rglob("*.class"))], "d8-" + destination.name)
            dex = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned bounded covariant fixture did not retain one DEX")
            return dex
        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the original bounded covariant inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(), "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["projected_method_count"] != 3 or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial" or coverage.get("generic_signature_projections")):
            raise RuntimeError("Bounded covariant recovery lost original generic or runtime contracts")
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        added = [row for row in coverage.get("generated_source_helpers", [])
                 if row["kind"] == "compiler-covariant-interface-bridge"]
        if len(added) != int(args.omit_interface_bridge):
            raise RuntimeError("Compiler interface bridge declaration differences were not disclosed")
        if args.omit_interface_bridge:
            projected = [row for row in coverage["methods"]
                         if row.get("projection_kind") == "interface-covariant-runtime-bridges"]
            if (len(projected) != 1 or not projected[0].get("metadata_differences")
                    or projected[0]["generated_bridges"] != ["Lfixture/Contract;->make(Lfixture/Pair;)Lfixture/ResultBase;"]
                    or added[0]["access_flags"] != ["public", "bridge", "synthetic"]):
                raise RuntimeError("Abstract interface projection omitted its exact added slot or metadata difference")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if (actual_classes != classes or not set(methods).issubset(actual_methods)
                or set(actual_methods) - set(methods) != helpers):
            raise RuntimeError("Bounded covariant source added an unreported method or changed ownership")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "BoundedCovariantMethodHarness", "regenerated" if args.omit_interface_bridge else "original"], "recovered-execution")
        if actual.splitlines() != expected + ["interface-bridge-count=1"]:
            raise RuntimeError("Bounded covariant source changed identity, dispatch, bounds or failures")
        negative = work / "negative-originals"
        shutil.copytree(originals, negative)
        receipt = change_signature(negative / "fixture/ResultLeaf.class",
                                   b"<V:Lfixture/Bound;>Ljava/lang/Object;Lfixture/ResultMid<TV;>;",
                                   b"<V:Lfixture/Bound;>Ljava/lang/Object;Lfixture/ResultMid<Lfixture/OtherBound;>;")
        (work / "negative-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = make_dex(negative, work / "negative-dex")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic inheritance" not in result.stderr:
            raise RuntimeError("Same-erasure changed generic result ancestry was accepted")
        negative_sources = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "ResultLeaf.java":
                content = content.replace("implements ResultMid<V>", "implements ResultMid<OtherBound>")
            destination = negative_sources / "fixture" / source.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Javac did not reject the changed complete covariant ancestry")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted(originals.rglob("*.class"))}:
            raise RuntimeError("Owned original bounded covariant input changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "identity_cases": 103, "ordered_failure_cases": 6, "original_class_bridges": 2,
                    "complete_method_bounds_and_scopes": "retained", "original_descriptors": "retained",
                    "generic_return_ancestry_counterexample": "unchanged Code, rejected by NeverD and javac",
                    "compiler_interface_bridges": "explicitly reported" if args.omit_interface_bridge else "original retained",
                    "jvm_output": "matched except reported interface bridge reflection", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
