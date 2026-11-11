#!/usr/bin/env python3
"""Compare captured anonymous bodies and explicitly changed source metadata."""
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
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    variants = parser.add_mutually_exclusive_group()
    variants.add_argument("--generic", action="store_true",
                        help="Capture a type variable from the exact enclosing method")
    variants.add_argument("--constructor", action="store_true",
                          help="Retain the exact enclosing constructor scope")
    variants.add_argument("--static-members", action="store_true",
                          help="Retain static helpers inside the generated local class")
    variants.add_argument("--nested", action="store_true",
                          help="Retain two nested anonymous scopes and their enclosing type variable")
    variants.add_argument("--field-members", action="store_true",
                          help="Retain anonymous field initializer classes and explicit outer captures")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-local-captures-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        inputs = sorted((FIXTURES / "local_captures/java").rglob("*.java"))
        if args.generic or args.constructor or args.static_members:
            inputs = [source for source in inputs if source.name != "CaptureHost.java"]
            variant = ("constructor_captures" if args.constructor else
                       "local_captures_static" if args.static_members else "local_captures_generic")
            inputs += sorted((FIXTURES / variant / "java").rglob("*.java"))
        if args.nested:
            inputs = sorted((FIXTURES / "local_captures_nested/java").rglob("*.java"))
        if args.field_members:
            inputs = sorted((FIXTURES / "field_anonymous_members/java").rglob("*.java"))
        harness_name = ("ConstructorCapturesHarness" if args.constructor else
                        "FieldAnonymousHarness" if args.field_members else
                        "NestedCapturesHarness" if args.nested else "LocalCapturesHarness")
        harness = FIXTURES / ("harness/" + harness_name + ".java")
        originals = work / "originals"
        original_release = 17 if args.static_members else 8
        verify.compile_local(inputs, originals, release=original_release)
        classes, methods = compiler_inventory(verify, originals)
        baseline_dir = work / "baseline"
        verify.compile_local([*inputs, harness], baseline_dir, release=original_release)
        harness_args = ["generic"] if args.generic else []
        baseline = verify.run([verify.java, "-cp", baseline_dir, harness_name, *harness_args], "baseline").splitlines()
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", originals)
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial"
                or coverage.get("minimum_java_release") != 25):
            raise RuntimeError("Captured-class inventory or conservative status changed")
        bindings = coverage["class_source_bindings"]
        if (args.generic or args.nested) and not any(row.get("generic_signature") == "()TT;"
                                    for row in coverage["methods"]):
            raise RuntimeError("The anonymous method lost its enclosing type-variable signature")
        locals_ = [binding for binding in bindings
                   if binding["binding_kind"] == "anonymous-to-named-local"]
        members = [binding for binding in bindings
                   if binding["binding_kind"] == "member-of-projected-class"]
        anonymous_members = [binding for binding in bindings
                             if binding["binding_kind"] == "anonymous-to-named-member"]
        if (len(locals_) != (2 if args.nested else 1)
                or len(members) != (1 if args.static_members else 0)
                or len(anonymous_members) != (1 if args.field_members else 0)
                or len(locals_) + len(members) + len(anonymous_members) != len(bindings)
                or any(binding["binding_kind"] != "anonymous-to-named-local"
                       or binding["binary_name_status"] != "changed"
                       or not binding["metadata_differences"] for binding in locals_)
                or any(binding["binary_name_status"] != "changed"
                       or not binding["metadata_differences"]
                       for binding in [*members, *anonymous_members])):
            raise RuntimeError("Known source metadata differences are not explicit")
        sources = sorted((output / "sources").rglob("*.java"))
        rebuilt = work / "rebuilt"
        verify.compile_local([*sources, harness], rebuilt, release=25)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        renamed_classes = {}
        for binding in bindings:
            candidates = [name for name in rebuilt_classes if name.endswith(binding["source_name"] + ";")]
            if len(candidates) != 1:
                raise RuntimeError("Recompiled local class binding is not unique")
            renamed_classes[binding["class"]] = candidates[0]
        def rename(identity):
            for original, replacement in renamed_classes.items():
                identity = identity.replace(original, replacement)
            return identity
        renamed = {rename(method) for method in methods}
        hidden_constructor_parameters = 0
        if args.constructor or args.nested:
            for binding in bindings:
                if args.constructor and binding.get("enclosing_kind") != "constructor":
                    raise RuntimeError("Constructor context is not recorded")
                for method in list(renamed - rebuilt_methods.keys()):
                    if not method.startswith(renamed_classes[binding["class"]] + "-><init>("):
                        continue
                    with_outer = method.replace(
                        "-><init>(", "-><init>(" + rename(binding["enclosing_method"]["owner"]), 1)
                    if with_outer in rebuilt_methods:
                        renamed.remove(method)
                        renamed.add(with_outer)
                        hidden_constructor_parameters += 1
        if not renamed.issubset(rebuilt_methods):
            raise RuntimeError("Recompiled source lost original method descriptors after the recorded rename")
        actual = verify.run([verify.java, "-cp", rebuilt, harness_name, *harness_args], "recovered-execution").splitlines()
        expected_before = "anonymous=true,local=false"
        expected_after = "anonymous=false,local=true"
        if args.field_members:
            expected_before = ("parentAnonymous=true,parentLocal=false,childAnonymous=true,"
                               "childMember=false,childStatic=false")
            expected_after = ("parentAnonymous=false,parentLocal=true,childAnonymous=false,"
                              "childMember=true,childStatic=true")
            for binding in anonymous_members:
                expected_class = (renamed_classes[binding["source_enclosing_class"]][:-1]
                                  + "$" + binding["source_name"] + ";")
                if renamed_classes[binding["class"]] != expected_class:
                    raise RuntimeError("Recompiled captured member has the wrong enclosure")
        if (actual[:-1] != baseline[:-1] or baseline[-1] != expected_before
                or actual[-1] != expected_after):
            raise RuntimeError("Captured body or documented metadata difference changed")
        evidence = {"status": "success", "class_count": len(classes),
                    "method_count": len(methods), "integer_probes": 5,
                    "java_release": 25, "jvm_behavior": "matched",
                    "lexical_scope": "constructor" if args.constructor else "method",
                    "anonymous_to_named_metadata_difference": "confirmed",
                    "coverage": "partial"}
        if args.generic:
            evidence["enclosing_method_type_variable"] = "retained"
        if args.constructor:
            evidence["lexical_constructor_scope"] = "retained"
            evidence["additional_compiler_outer_parameters"] = hidden_constructor_parameters
        if args.static_members:
            evidence["static_local_helper"] = "retained"
            evidence["static_member_class"] = "retained"
        if args.nested:
            evidence["nested_method_scopes"] = "retained"
            evidence["enclosing_method_type_variable"] = "retained at both levels"
            evidence["additional_compiler_outer_parameters"] = hidden_constructor_parameters
        if args.field_members:
            evidence["field_initializer_captures"] = "retained"
            evidence["anonymous_to_static_member_metadata_difference"] = "confirmed"
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
