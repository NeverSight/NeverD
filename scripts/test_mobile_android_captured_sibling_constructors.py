#!/usr/bin/env python3
"""Verify scoped sibling constructor forwarding against frozen owned callers."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_open_scoped_mask import facts
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_open_scoped_mask import facts
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-inputs", action="store_true")
    parser.add_argument("--context-input", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-captured-sibling-constructors-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        fixture = FIXTURES / "captured_sibling_constructors"
        harness_source = FIXTURES / "harness/CapturedSiblingConstructorsHarness.java"
        if args.context_input:
            authored = work / "authored-fixture"
            shutil.copytree(fixture, authored)
            fixture = authored
            path = fixture / "java/fixture/Owner.java"
            text = path.read_text()
            replacements = [
                ("private final class Child extends Base {", "private final class Child extends Base {\n        final Object context;"),
                ("Child(Probe<? super T> sink, int marker)", "Child(Object context, Probe<? super T> sink, int marker)"),
                ("super(sink, marker);", "super(sink, marker);\n            this.context = context;"),
            ]
            for before, changed in replacements:
                assert text.count(before) == 1
                text = text.replace(before, changed)
            path.write_text(text)
            text = harness_source.read_text()
            replacements = [
                ("Owner.class, Probe.class, int.class", "Owner.class, Object.class, Probe.class, int.class"),
                ("Object payload = new Object();", "Object payload = new Object();\n        Object context = new Object();"),
                ("ctor.newInstance(owner, supplied, marker)", "ctor.newInstance(owner, context, supplied, marker)"),
                ('check(field(child, "this$0", partial) == owner);',
                 'check(field(child, "this$0", partial) == owner);\n            check(field(child, "context", partial) == (stage == 0 ? context : null));'),
            ]
            for before, changed in replacements:
                assert text.count(before) == 1
                text = text.replace(before, changed)
            directory = work / "authored-harness"
            directory.mkdir()
            harness_source = directory / harness_source.name
            harness_source.open("x").write(text)
        originals, harness = work / "originals", work / "harness"
        sources = sorted((fixture / "java").rglob("*.java"))
        verify.compile_local(sources, originals, release=8)
        harness_name = "CapturedSiblingConstructorsHarness"
        verify.compile_local([harness_source], harness,
                             classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        (work / "baseline-cli-sha256.txt").open("x").write(sha(args.neverd.resolve()) + "\n")
        records, exceptions = facts(originals), exception_inventory(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (5, 6):
            raise RuntimeError("Authored sibling constructor inventory changed")
        providers = {"Lfixture/Probe;", "Lfixture/Trace;"} if args.dependency_inputs else set()
        classes = all_classes - providers
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        sdk, dependencies = work / "sdk", []
        sdk.mkdir()
        if providers:
            jar = work / "owned-inputs.jar"
            with zipfile.ZipFile(jar, "x") as archive:
                for owner in sorted(providers):
                    name = owner[1:-1] + ".class"
                    path = sdk / name
                    path.parent.mkdir(exist_ok=True)
                    shutil.copyfile(originals / name, path)
                    archive.writestr(name, path.read_bytes())
            dependencies.append(jar)
        input_hashes = {str(p): sha(p) for p in [args.android_jar.resolve(), *dependencies]}
        classpath = [part for p in [args.android_jar.resolve(), *dependencies]
                     for part in ("--classpath", str(p))]

        def execute(runtime, label):
            outputs = []
            for route in range(2):
                for mode in range(12):
                    result = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                         harness_name, str(route), str(mode)], f"{label}-{route}-{mode}")
                    expected_events = (1, 9, 66)[min((mode % 4 or 4) - 1, 2)]
                    expected = [f"route={route},mode={mode},attempt={attempt},events={expected_events}"
                                for attempt in range(2)]
                    if result.splitlines() != expected:
                        raise RuntimeError("Independent constructor state/effect oracle changed: " + result)
                    outputs.append(result)
            return outputs

        baseline = execute((originals, harness), "baseline")
        paths = [p.relative_to(originals) for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]

        def dex(root, name):
            destination = work / name
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", destination,
                        *[root / p for p in paths]], name)
            return destination / "classes.dex"

        source_dex = dex(originals, "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed original sibling definitions")
        (work / "inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial"
                or coverage.get("minimum_java_release") != 25):
            raise RuntimeError("Sibling constructor coverage/disclosure changed")
        members = {"Lfixture/Owner$Base;", "Lfixture/Owner$Child;"}
        bindings = {r["class"]: r for r in coverage.get("class_source_bindings", [])}
        if set(bindings) != members:
            raise RuntimeError("Missing original captured sibling source ownership")
        for owner, binding in bindings.items():
            if (binding.get("binding_kind") != "inner-to-static-member"
                    or binding.get("outer_capture_field") != owner + "->this$0:Lfixture/Owner;"
                    or not any("synthetic capture-field" in text for text in binding["metadata_differences"])):
                raise RuntimeError("Original captured enclosing field was not disclosed")
        for row in coverage.get("generic_signature_projections", []):
            for change in row["changes"]:
                if change["kind"] == "static-inner-type-variable":
                    if (change["original_type"] not in ("Lfixture/Owner;::T", "Lfixture/Owner;::U")
                            or change["source_type"] != "Ljava/lang/Object;"):
                        raise RuntimeError("Sibling projection lost its exact original formal")
                elif change["kind"] != "static-inner-owner-arguments":
                    raise RuntimeError("Unexpected sibling generic projection")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=sdk if providers else None, release=25)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        helper_owners = classes - {"Lfixture/Probe;"}
        if helpers != {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                       for owner in helper_owners} or any(
                r.get("kind") != "throw-helper" or r.get("static") is not True
                for r in coverage.get("generated_source_helpers", [])):
            raise RuntimeError("Unexpected generated sibling helper role")
        if actual_classes != classes or set(actual_methods) != methods | helpers:
            raise RuntimeError("Rebuilt original entry/helper roles changed")
        after, after_exceptions = facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted = deepcopy(records[owner])
            actual = deepcopy(after[owner])
            if wanted["major"] != 52 or actual["major"] != 69:
                raise RuntimeError("Original/rebuilt compiler target changed")
            wanted["major"] = 69
            for identity in helpers:
                if identity.split("->", 1)[0] != owner:
                    continue
                helper = actual["methods"].pop(identity)
                if helper != {"name": "__neverdThrow",
                              "prototype": "(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;",
                              "access": 10, "code": True,
                              "signature": "<E:Ljava/lang/Throwable;>(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;^TE;",
                              "deprecated_attribute": False, "runtime_visible_annotations": [],
                              "runtime_invisible_annotations": [], "runtime_invisible_parameter_annotations": []}:
                    raise RuntimeError("Generated throw helper contract changed")
            if owner in members:
                wanted["inner_class"]["access"] |= 8
                wanted["fields"][owner + "->this$0:Lfixture/Owner;"]["access"] &= ~0x1000
                if owner.endswith("$Child;"):
                    wanted["signature"] = None
                for field in wanted["fields"].values():
                    if field["signature"]:
                        field["signature"] = field["signature"].replace("TT;", "Ljava/lang/Object;")
                for method in wanted["methods"].values():
                    if method["signature"]:
                        method["signature"] = method["signature"].replace("TT;", "Ljava/lang/Object;")
                        method["signature"] = method["signature"].replace("(", "(Lfixture/Owner;", 1)
            if actual != wanted:
                (work / "metadata-difference.json").open("x").write(json.dumps(
                    {"owner": owner, "expected": wanted, "actual": actual}, indent=2) + "\n")
                raise RuntimeError("Undisclosed sibling class/member metadata change: " + owner)
            for identity in records[owner]["methods"]:
                if after_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Original sibling Exceptions changed: " + identity)
        if execute((rebuilt, harness, *([sdk] if providers else [])), "recovered") != baseline:
            raise RuntimeError("Captured sibling argument identity, state or exception order changed")
        controls = [
            ("other-formal", "fixture/Owner$Child.class", b"(Lfixture/Probe<-TT;>;I)V", b"(Lfixture/Probe<-TU;>;I)V"),
            ("opposite-variance", "fixture/Owner$Child.class", b"(Lfixture/Probe<-TT;>;I)V", b"(Lfixture/Probe<+TT;>;I)V"),
            ("parent-formal", "fixture/Owner$Base.class", b"(Lfixture/Probe<-TT;>;I)V", b"(Lfixture/Probe<-TU;>;I)V"),
            ("owner-edge", "fixture/Owner$Child.class", b"Lfixture/Owner<TT;TU;>.Base;", b"Lfixture/Owner<TU;TT;>.Base;"),
        ]
        for name, file, before, changed in controls:
            if args.context_input and file.endswith("$Child.class") and before.startswith(b"("):
                before = before.replace(b"(", b"(Ljava/lang/Object;", 1)
                changed = changed.replace(b"(", b"(Ljava/lang/Object;", 1)
            negative = work / (name + "-classes")
            shutil.copytree(originals, negative)
            original_code = method_code((negative / file).read_bytes())
            receipt = change_signature(negative / file, before, changed)
            if method_code((negative / file).read_bytes()) != original_code:
                raise RuntimeError("Signature-only negative changed original Code")
            (work / (name + "-metadata.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            source = dex(negative, name + "-dex")
            rejected = work / (name + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(source), *classpath,
                                     "-o", str(rejected)], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (name + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Unproven original sibling contract was accepted: " + name)
        wrongs = [("marker", "this.marker = marker;", "this.marker = 0;", "0", "0"),
                  ("sink", "this.sink = sink;", "this.sink = null;", "0", "0"),
                  ("order", "Trace.record(this, 2);", "Trace.record(this, 3);", "0", "2"),
                  ("capture", "super(sink, marker);", "super(sink, marker); Trace.cached = null;", "0", "0")]
        if args.context_input:
            wrongs.append(("context", "this.context = context;", "this.context = null;", "0", "0"))
        for name, before, changed, route, mode in wrongs:
            altered = work / (name + "-wrong-source")
            shutil.copytree(fixture / "java", altered)
            path = altered / "fixture/Owner.java"
            text = path.read_text()
            assert text.count(before) == 1
            path.write_text(text.replace(before, changed))
            wrong = work / (name + "-wrong-classes")
            verify.compile_local(sorted(altered.rglob("*.java")), wrong, release=8)
            result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (wrong, harness))),
                                     harness_name, route, mode], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (name + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "AssertionError" not in result.stderr:
                raise RuntimeError("Frozen caller missed an independently compiled wrong program: " + name)
        if ({"originals": hashes(originals), "harness": hashes(harness)} != frozen
                or any(sha(Path(p)) != h for p, h in input_hashes.items())):
            raise RuntimeError("Frozen compiler/runtime inputs changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_inputs": args.dependency_inputs, "fresh_jvm_cases": 24,
                    "context_input": args.context_input,
                    "attempts_per_case": 2, "same_code_counterexamples": [c[0] for c in controls],
                    "independent_compiler_counterexamples": len(wrongs), "coverage": "partial"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
