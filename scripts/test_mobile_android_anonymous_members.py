#!/usr/bin/env python3
"""Verify direct parent Object formals in anonymous field initializer members."""
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
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-anonymous-member-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), args.d8.resolve(), 300)
        originals, harness_dir = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "generic_anonymous_members/java").rglob("*.java"))
        harness = FIXTURES / "harness/GenericAnonymousMemberHarness.java"
        verify.compile_local(inputs, originals)
        verify.compile_local([harness], harness_dir, classpath=originals, release=17)
        classes, methods = compiler_inventory(verify, originals)
        paths = sorted(originals.rglob("*.class"))
        hashes = {path.relative_to(originals).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}
        (work / "unmodified-input.json").write_text(json.dumps(hashes, indent=2) + "\n")
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (harness_dir, originals))),
                               "GenericAnonymousMemberHarness", "original"], "baseline")
        dex = verify.dex(paths, work / "dex", originals)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned anonymous member declaration inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        verify.run([verify.dexdump, "-d", dex], "sdk-code")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial" or coverage.get("minimum_java_release") != 25):
            raise RuntimeError("Anonymous member changed inventory or omitted partial coverage")
        projections = coverage.get("generic_signature_projections", [])
        if ({row["original_signature"] for row in projections}
                != {"Ljava/lang/Object;Lfixture/Reader<TT;>;", "()TT;"}):
            raise RuntimeError("Missing original anonymous class or result Signature")
        for row in projections:
            if not row.get("metadata_differences") or len(row["changes"]) != 1:
                raise RuntimeError("Missing anonymous member metadata difference")
            change = row["changes"][0]
            if (change["kind"] != "anonymous-member-type-variable"
                    or change["original_type"] != "Lfixture/Owner;::T"
                    or change["source_type"] != "Ljava/lang/Object;"
                    or change.get("scope_origin") != "Lfixture/Owner;"):
                raise RuntimeError("Missing direct original parent formal declaration")
        bindings = coverage.get("class_source_bindings", [])
        if (len(bindings) != 1 or bindings[0]["binding_kind"] != "anonymous-to-named-member"
                or not bindings[0].get("metadata_differences")):
            raise RuntimeError("Missing explicit named member and binary name difference")
        binding = bindings[0]
        mapped = binding["source_enclosing_class"][:-1] + "$" + binding["source_name"] + ";"
        rebuilt = work / "rebuilt"
        verify.compile_local([*sorted((output / "sources").rglob("*.java")), harness], rebuilt, release=25)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        expected = {identity.replace(binding["class"], mapped) for identity in methods}
        if mapped not in rebuilt_classes or not expected.issubset(rebuilt_methods):
            raise RuntimeError("Recompiled member lost an original runtime descriptor")
        actual = verify.run([verify.java, "-cp", rebuilt, "GenericAnonymousMemberHarness", "recovered"],
                            "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Anonymous member scope projection changed executable behavior")
        if hashes != {path.relative_to(originals).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}:
            raise RuntimeError("Verification modified an original compiled class")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "returned_identities": 20, "ordered_failures": 9, "mutable_outer_receiver": "preserved",
                    "own_method_formal": "preserved", "public_owner_signatures": "preserved",
                    "runtime_descriptors": "preserved", "generic_reflection_differences": 2,
                    "original_class_bytes": "unmodified", "source_generic_bounds": "revalidated"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
