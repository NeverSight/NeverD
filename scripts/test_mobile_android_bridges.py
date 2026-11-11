#!/usr/bin/env python3
"""Check generic bridge inventory, JVM casts, arrays and subclass dispatch."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import json
from pathlib import Path
import tempfile

try:
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    variants = parser.add_mutually_exclusive_group()
    variants.add_argument("--anonymous", action="store_true",
                        help="Exercise a creator allocated in a static initializer")
    variants.add_argument("--marker", action="store_true",
                          help="Retain javac's constructorless anonymous access marker")
    variants.add_argument("--marker-callback", action="store_true",
                          help="Retain one anonymous class used as both callback and access marker")
    variants.add_argument("--anonymous-base", action="store_true",
                          help="Retain generic superclass constructor arguments and checked bridges")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-generic-bridges-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        directory = ("anonymous_base/java" if args.anonymous_base else
                     "anonymous_marker_callback/java" if args.marker_callback else
                     "anonymous_marker/java" if args.marker else
                     "anonymous/java" if args.anonymous else "bridges/java")
        inputs = sorted((FIXTURES / directory).rglob("*.java"))
        originals = work / "originals"
        verify.compile_local(inputs, originals)
        classes, methods = compiler_inventory(verify, originals)
        harness_name = ("AnonymousBaseHarness" if args.anonymous_base else
                        "AnonymousMarkerCallbackHarness" if args.marker_callback else
                        "AnonymousMarkerHarness" if args.marker else
                        "AnonymousCreatorHarness" if args.anonymous else "GenericBridgeHarness")
        harness = FIXTURES / ("harness/" + harness_name + ".java")
        baseline_dir = work / "baseline"
        verify.compile_local([*inputs, harness], baseline_dir)
        harness_args = ["fixture.MarkerHost$1"] if args.marker or args.marker_callback else []
        baseline = verify.run([verify.java, "-cp", baseline_dir, harness_name, *harness_args], "baseline")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", originals)
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods)
                or coverage["projected_method_count"] != (5 if args.marker_callback else 6 if args.anonymous_base else
                                                         2 if args.marker else 8 if args.anonymous else 3)
                or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial"):
            raise RuntimeError("Bridge inventory or conservative coverage status changed")
        for row in coverage["methods"]:
            if args.anonymous_base and row["status"] == "source-projected":
                if row["projection_kind"] == "anonymous-to-named-member":
                    continue
                if (row["projection_kind"] != "generic-superclass-bridge"
                        or row["bridge_target"] not in methods or row["bridge_dispatch"] not in methods):
                    raise RuntimeError("Superclass bridge is not tied to original methods")
                continue
            if (args.marker or args.marker_callback) and row["status"] == "source-projected":
                if row["projection_kind"] != "anonymous-to-named-member":
                    raise RuntimeError("Marker references lost their source mapping")
                continue
            if row["status"] == "source-projected" and row["projection_kind"] != "anonymous-interface" and (
                    row["projection_kind"] != "generic-interface-bridge"
                    or row["bridge_target"] not in methods
                    or row["bridge_dispatch"] not in methods):
                raise RuntimeError("Bridge mapping is not tied to original method identities")
        rebuilt = work / "rebuilt"
        sources = sorted((output / "sources").rglob("*.java"))
        verify.compile_local([*sources, harness], rebuilt)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        expected_methods = set(methods)
        if args.anonymous_base:
            bindings = coverage["class_source_bindings"]
            if (len(bindings) != 1 or bindings[0]["binding_kind"] != "anonymous-to-named-member"
                    or bindings[0]["binary_name_status"] != "changed"
                    or not bindings[0]["metadata_differences"]):
                raise RuntimeError("Anonymous superclass source rename is not explicit")
            binding = bindings[0]
            replacement = binding["source_enclosing_class"][:-1] + "$" + binding["source_name"] + ";"
            if replacement not in rebuilt_classes:
                raise RuntimeError("Anonymous superclass binding has the wrong enclosure")
            expected_methods = {method.replace(binding["class"], replacement) for method in methods}
        if args.marker or args.marker_callback:
            bindings = coverage["class_source_bindings"]
            if (len(bindings) != 1 or bindings[0]["class"] != "Lfixture/MarkerHost$1;"
                    or bindings[0]["binding_kind"] != "anonymous-to-named-member"
                    or bindings[0]["binary_name_status"] != "changed"
                    or len(bindings[0]["metadata_differences"]) < (4 if args.marker_callback else 5)):
                raise RuntimeError("Anonymous marker metadata differences are not explicit")
            binding = bindings[0]
            replacement = binding["source_enclosing_class"][:-1] + "$" + binding["source_name"] + ";"
            if replacement not in rebuilt_classes:
                raise RuntimeError("Marker source class has the wrong enclosing class")
            expected_methods = {method.replace(binding["class"], replacement) for method in methods}
            harness_args = [replacement[1:-1].replace("/", ".")]
            added_constructor = any(row["class"] == binding["class"] and row["kind"] == "default-constructor"
                                    for row in coverage["generated_source_helpers"])
            if added_constructor != (not args.marker_callback):
                raise RuntimeError("Added source marker constructor is not recorded")
        if not expected_methods.issubset(rebuilt_methods):
            raise RuntimeError("javac did not regenerate every original bridge descriptor")
        actual = verify.run([verify.java, "-cp", rebuilt, harness_name, *harness_args], "recovered-execution")
        if args.anonymous_base:
            before, after = baseline.splitlines(), actual.splitlines()
            if (before[-1] != "anonymous=true,member=false,bridges=2"
                    or after[-1] != "anonymous=false,member=true,bridges=2"):
                raise RuntimeError("Superclass bridge or anonymous metadata differs from the report")
            baseline, actual = "\n".join(before[:-1]), "\n".join(after[:-1])
        if args.marker:
            before, after = baseline.splitlines(), actual.splitlines()
            if (before[-1] != "anonymous=true,member=false,constructors=0"
                    or after[-1] != "anonymous=false,member=true,constructors=1"):
                raise RuntimeError("Constructorless marker reflection differs from the report")
            baseline, actual = "\n".join(before[:-1]), "\n".join(after[:-1])
        if args.marker_callback:
            before, after = baseline.splitlines(), actual.splitlines()
            if (before[-1] != "anonymous=true,member=false"
                    or after[-1] != "anonymous=false,member=true"):
                raise RuntimeError("Shared marker reflection differs from the report")
            baseline, actual = "\n".join(before[:-1]), "\n".join(after[:-1])
        if actual != baseline:
            raise RuntimeError("Regenerated bridges changed JVM behavior")
        evidence = {"status": "success", "class_count": len(classes),
                    "method_count": len(methods), "bridge_count": 2 if args.anonymous_base else 0 if args.marker or args.marker_callback else 3,
                    "coverage": "partial", "jvm_output": "matched",
                    "original_descriptors": "retained"}
        if args.anonymous:
            evidence["anonymous_class_reflection"] = "matched"
        if args.marker:
            evidence["original_descriptors"] = "retained after recorded class rename"
            evidence["constructorless_marker"] = "retained"
            evidence["additional_constructor_helper"] = "recorded"
            evidence["metadata_difference"] = "confirmed"
        if args.marker_callback:
            evidence["original_descriptors"] = "retained after recorded class rename"
            evidence["shared_marker_and_callback_identity"] = "matched"
            evidence["original_constructor"] = "retained"
            evidence["metadata_difference"] = "confirmed"
        if args.anonymous_base:
            evidence["original_descriptors"] = "retained after recorded class rename"
            evidence["generic_superclass_binding"] = "retained"
            evidence["checked_void_bridge"] = "matched"
            evidence["inherited_concrete_method"] = "matched"
            evidence["metadata_difference"] = "confirmed"
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
