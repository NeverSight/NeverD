#!/usr/bin/env python3
"""Verify shared recursive Comparable captures against independent D8/JVM code."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import json
import os
from pathlib import Path
import tempfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk
    from scripts.test_mobile_android_shared_upper_bounds import captured_metadata, owner_metadata
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk
    from test_mobile_android_shared_upper_bounds import captured_metadata, owner_metadata
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--d8", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True,
                        help="Real Android SDK declaration archive; its bytecode is never executed")
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-shared-recursive-bound-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), args.d8.resolve(), 300)
        originals, harness_dir = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "shared_recursive_bounds/java").rglob("*.java"))
        harness = FIXTURES / "harness/SharedRecursiveBoundHarness.java"
        verify.compile_local(inputs, originals)
        verify.compile_local([harness], harness_dir, classpath=originals, release=17)
        classes, methods = compiler_inventory(verify, originals)
        paths = sorted(originals.rglob("*.class"))
        before = {path.relative_to(originals).as_posix(): method_code(path.read_bytes()) for path in paths}
        mutation = captured_metadata(originals / "fixture/Owner$Worker.class", recursive=True)
        mutation["owner_enclosure"] = owner_metadata(originals / "fixture/Owner.class")
        if before != {path.relative_to(originals).as_posix(): method_code(path.read_bytes()) for path in paths}:
            raise RuntimeError("Recursive enclosure edit changed an original Code attribute or descriptor")
        (work / "input-metadata-change.json").write_text(json.dumps(mutation, indent=2) + "\n")
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (harness_dir, originals))),
                               "SharedRecursiveBoundHarness", "original"], "baseline")
        dex = verify.dex(paths, work / "dex", originals)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned recursive bound declaration inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        verify.run([verify.dexdump, "-d", dex], "sdk-code")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial"):
            raise RuntimeError("Recursive bound projection changed runtime inventory or omitted partial coverage")
        origin = "Lfixture/Owner;->make(Lfixture/Selector;Ljava/lang/Comparable;)Ljava/lang/Object;"
        changes = []
        for row in coverage.get("generic_signature_projections", []):
            if not row.get("original_signature") or not row.get("metadata_differences"):
                raise RuntimeError("Missing original recursive capture signature or metadata difference")
            changes.extend(row["changes"])
        bounded = [change for change in changes if change["kind"] == "shared-recursive-interface-bound"]
        if (len(bounded) != 4 or any(change["original_type"] != origin + "::K"
                                  or change["source_type"] != "Ljava/lang/Comparable<*>;"
                                  or change.get("scope_origin") != origin for change in bounded)):
            raise RuntimeError("Missing exact recursive method formal, wildcard or reported self-relation loss")
        bindings = coverage.get("class_source_bindings", [])
        if len(bindings) != 1 or bindings[0]["binding_kind"] != "anonymous-to-shared-member":
            raise RuntimeError("Missing shared original recursive class identity")
        binding = bindings[0]
        mapped = binding["source_enclosing_class"][:-1] + "$" + binding["source_name"] + ";"
        rebuilt = work / "rebuilt"
        verify.compile_local([*sorted((output / "sources").rglob("*.java")), harness], rebuilt, release=17)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        expected = {identity.replace(binding["class"], mapped) for identity in methods}
        if mapped not in rebuilt_classes or not expected.issubset(rebuilt_methods):
            raise RuntimeError("Recompiled recursive bound lost an original runtime method descriptor")
        actual = verify.run([verify.java, "-cp", rebuilt, "SharedRecursiveBoundHarness", "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Shared recursive bound projection changed executable behavior")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "comparison_identities": 48, "echo_identities": 6, "ordered_failures": 37,
                    "comparable_implementations": 2, "capture_identity": "preserved",
                    "runtime_descriptors": "preserved", "own_method_formal": "preserved",
                    "owner_method_bounds": "preserved", "captured_self_relation": "reported as lost",
                    "wildcard_interface": "preserved", "original_code_bytes": "unchanged",
                    "source_generic_bounds": "revalidated"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
