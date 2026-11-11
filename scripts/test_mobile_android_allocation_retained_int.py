#!/usr/bin/env python3
"""Verify retained integer forwarding calls inside ordered nested allocations."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_static_defaults import method_code


BRIDGE = "Lfixture/Producer;->length()I"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-api", action="store_true")
    parser.add_argument("--derived", action="store_true")
    parser.add_argument("--plain-api", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-allocation-retained-int-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "allocation_retained_int/java"
        originals, encoded, harness = (work / name for name in ("originals", "encoded", "harness"))
        inputs = []
        for source in sorted(fixture.rglob("*.java")):
            changes = {"CountView.java": ("CountView<R>", "CountView"),
                       "Producer.java": ("implements CountView<Object>", "implements CountView")}
            if args.plain_api and source.name in changes:
                before, after = changes[source.name]
                text = source.read_text()
                if text.count(before) != 1:
                    raise RuntimeError("Original plain API declaration anchor changed")
                source = work / "plain-java/fixture" / source.name
                source.parent.mkdir(parents=True, exist_ok=True)
                source.open("x").write(text.replace(before, after, 1))
            inputs.append(source)
        verify.compile_local(inputs, originals, release=8)
        verify.compile_local([FIXTURES / "harness" / ("AllocationRetainedInt" + suffix + ".java")
                              for suffix in ("Caller", "Harness")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        shutil.copytree(originals, encoded)
        receipt = change_method_flags(encoded / "fixture/Producer.class", b"()I", 0x11, 0x51, name=b"length")
        for path in sorted(originals.rglob("*.class")):
            relative = path.relative_to(originals)
            before, after = path.read_bytes(), (encoded / relative).read_bytes()
            if relative.as_posix() == "fixture/Producer.class":
                offsets = [i for i, (left, right) in enumerate(zip(before, after)) if left != right]
                if len(before) != len(after) or len(offsets) != 1 or before[offsets[0]] != 0x11 or after[offsets[0]] != 0x51:
                    raise RuntimeError("Owned bridge encoding changed bytes outside its selected access flag")
                if method_code(before) != method_code(after):
                    raise RuntimeError("Owned bridge encoding changed original Code or method attributes")
            elif before != after:
                raise RuntimeError("Owned bridge encoding changed another original class")
        encoded_hashes = hashes(encoded)
        (work / "owned-bridge-flags.json").write_text(json.dumps(receipt, indent=2) + "\n")
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, encoded)
        if len(all_classes) != 7 or len(all_methods) != 14:
            raise RuntimeError("Original retained integer allocation inventory changed")
        classes = all_classes - ({"Lfixture/CountView;"} if args.dependency_api else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        dependency = work / "owned-api.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            jar.writestr("fixture/CountView.class", (originals / "fixture/CountView.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_api:
            classpath += ["--classpath", dependency]
        records, exceptions = class_identity.compiler_classes(encoded), exception_inventory(encoded)

        def execute(runtime: tuple, label: str, is_encoded: bool) -> list[str]:
            outputs = []
            for route in range(4):
                for mode in range(16):
                    result = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "AllocationRetainedIntHarness", str(route), str(mode), str(args.derived).lower(),
                        str(is_encoded).lower(), str(args.plain_api).lower()], f"{label}-{route}-{mode}")
                    pattern = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(result.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in result.splitlines()):
                        raise RuntimeError("Original retained integer allocation matrix changed: " + result)
                    outputs.append(result)
            return outputs

        baseline = execute((originals, harness), "original-baseline", False)
        if execute((encoded, harness), "encoded-baseline", True) != baseline:
            raise RuntimeError("Access-only bridge encoding changed original execution")
        # Independently compiled, legal Java programs must be distinguished by
        # the frozen caller before NeverD sees any input.
        mutations = (
            ("bypassed-wrapper", "Core", "producer.length(),producer.length()", "producer.getCount(),producer.length()", 0),
            ("cached-call", "Core", "return new Outer(new Inner(producer.length(),producer.length()));",
             "return new Outer(new Inner(cache(producer),cached)); } private static int cached; private static int cache(Producer p) { return cached=p.length();", 0),
            ("early-calls", "Core", "return new Outer(new Inner(producer.length(),producer.length()));",
             "int first=producer.length(),second=producer.length(); return new Outer(new Inner(first,second));", 0),
            ("changed-receiver", "Core", "producer.length(),producer.length()", "producer.length(),new Producer().length()", 0),
            ("changed-snapshot", "Producer", "return result;", "return Trace.value;", 0),
            ("early-final-store", "Inner", "Trace.partial=this; Trace.hit(6);", "Trace.partial=this; this.first=first; Trace.hit(6);", 5),
        )
        for label, name, before, after, mode in mutations:
            source = next(p for p in inputs if p.name == name + ".java").read_text()
            if source.count(before) != 1:
                raise RuntimeError("Original retained allocation counterexample anchor changed: " + label)
            text = source.replace(before, after, 1)
            if label == "early-final-store":
                text = text.replace("this.first=first; this.second=second;", "this.second=second;", 1)
            changed = work / label / (name + ".java")
            changed.parent.mkdir()
            changed.write_text(text)
            wrong = work / (label + "-classes")
            verify.compile_local([changed], wrong, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness))),
                "AllocationRetainedIntHarness", "1", str(mode), str(args.derived).lower(), "false",
                str(args.plain_api).lower()],
                text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned retained integer allocation" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect retained integer allocation: " + label)

        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(encoded.rglob("*.class"))
                 if "L" + p.relative_to(encoded).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original retained integer allocation DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes
                or {r["identity"] for r in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original retained integer allocation entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 1 or coverage["status"] != "partial"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or rows[BRIDGE].get("projection_kind") != "plain-interface-forwarding-bridge"
                or rows[BRIDGE].get("bridge_target") != "Lfixture/Producer;->getCount()I"
                or not rows[BRIDGE].get("metadata_differences")):
            raise RuntimeError("Retained integer allocation changed original entries or forwarding contract")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
            classpath=dependency if args.dependency_api else None, release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for c in classes if c != "Lfixture/CountView;"}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Retained integer allocation invented or omitted a runtime entry")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != after[owner].get(key):
                    raise RuntimeError("Original retained allocation class metadata changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                expected = copy.deepcopy(metadata)
                if identity == BRIDGE:
                    expected["access"] &= ~0x40
                if after[owner]["methods"].get(identity) != expected or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original retained allocation method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_api else (rebuilt, harness)
        if execute(runtime, "rebuilt", False) != baseline:
            raise RuntimeError("Retained integer allocation changed values, dispatch, stack frames or ordered effects")
        if (hashes(originals) != original_hashes or hashes(encoded) != encoded_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, access-only encoded inputs or frozen observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
            "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_api": args.dependency_api,
            "derived": args.derived, "plain_api": args.plain_api,
            "original_and_access_only_encoded_baselines": "matched",
            "bridge_encoding": "only ACC_BRIDGE; every Code, descriptor and attribute byte unchanged",
            "wrapper_stack_frames_virtual_dispatch_and_repeated_snapshots": "matched",
            "allocation_class_initialization_partial_finals_and_exact_failures": "matched",
            "all_original_entries_metadata_and_exceptions": "unchanged except reported ACC_BRIDGE flag",
            "independent_wrong_programs": len(mutations), "original_inputs_and_immutable_callers": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
