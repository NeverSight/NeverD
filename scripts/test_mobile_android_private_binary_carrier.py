#!/usr/bin/env python3
"""Verify private members sharing two complete unary input and result carriers."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-api", action="store_true")
    parser.add_argument("--static-helper", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-private-binary-carrier-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "private_binary_carrier/java"
        inputs = []
        for source in sorted(fixture.rglob("*.java")):
            text = source.read_text()
            if args.static_helper and source.name == "Provider.java":
                if text.count("private final <T>") != 1 or text.count("Trace.receiver=this;") != 1:
                    raise RuntimeError("Original private member fixture changed")
                source = work / "static-java/fixture/Provider.java"
                source.parent.mkdir(parents=True)
                text = text.replace("private final <T>", "private static final <T>", 1)
                text = text.replace("Trace.receiver=this;", "Trace.receiver=null;", 1)
                source.write_text(text)
            inputs.append(source)
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals, release=8)
        verify.compile_local([FIXTURES / "harness" / ("PrivateBinaryCarrier" + suffix + ".java")
                              for suffix in ("Caller", "Harness")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != 7:
            raise RuntimeError("Original private binary carrier inventory changed")
        declarations = {"Lfixture/Carrier;"}
        classes = all_classes - (declarations if args.dependency_api else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        dependency = work / "owned-api.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for descriptor in sorted(declarations):
                path = descriptor[1:-1] + ".class"
                jar.writestr(path, (originals / path).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_api:
            classpath += ["--classpath", dependency]
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        (work / "frozen-input-hashes.json").write_text(json.dumps(
            {"originals": original_hashes, "harness": harness_hashes, "sdk": sdk_hash,
             "dependency": dependency_hash}, indent=2) + "\n")

        def execute(runtime: tuple, label: str) -> list[str]:
            outputs = []
            for route in range(4):
                for mode in range(16):
                    for present in (False, True):
                        result = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "PrivateBinaryCarrierHarness", str(route), str(mode), str(present).lower(),
                            str(args.static_helper).lower()], f"{label}-{route}-{mode}-{present}")
                        pattern = f"route={route},mode={mode},peer={str(present).lower()},attempt=[01],trace=[0-9]+"
                        if len(result.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in result.splitlines()):
                            raise RuntimeError("Original private binary carrier matrix changed: " + result)
                        outputs.append(result)
            return outputs

        baseline = execute((originals, harness), "baseline")
        mutations = (
            ("swapped-inputs", "peer.read(left,right)", "peer.read(right,left)", 0),
            ("skipped-second-callback", "right==null?null:right.get()", "null", 0),
            ("duplicated-first-callback", "Object first=", "if(left!=null)left.get();Object first=", 0),
            ("changed-returned-reference", "(Carrier<T>)Trace.result;Trace.hit(5)", "(Carrier<T>)left;Trace.hit(5)", 0),
            ("early-element-cast", "left==null?null:left.get()", "left==null?null:(String)left.get()", 0),
            ("cached-result", "Carrier<T> result=(Carrier<T>)Trace.result;Trace.hit(5);", "Trace.hit(5);", 8),
            ("late-result-read", "return result;", "return (Carrier<T>)Trace.result;", 9))
        provider = next(p for p in inputs if p.name == "Provider.java").read_text()
        for label, before, after, mode in mutations:
            if provider.count(before) != 1:
                raise RuntimeError("Original private binary counterexample anchor changed: " + label)
            text = provider.replace(before, after, 1)
            if label == "cached-result":
                if text.count("Trace.receiver=") != 1:
                    raise RuntimeError("Original result capture anchor changed")
                text = text.replace("Trace.receiver=", "Carrier<T> result=(Carrier<T>)Trace.result;Trace.receiver=", 1)
            changed = work / label / "Provider.java"
            changed.parent.mkdir()
            changed.open("x").write(text)
            wrong = work / (label + "-classes")
            verify.compile_local([changed], wrong, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness))),
                "PrivateBinaryCarrierHarness", "1", str(mode), "true", str(args.static_helper).lower()],
                text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned private binary carrier" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect private binary carrier: " + label)
        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original private binary carrier DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes
                or {r["identity"] for r in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original private binary carrier entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Private binary carrier recovery changed original entries or full generic contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
            classpath=dependency if args.dependency_api else None, release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for c in classes if c not in {"Lfixture/Carrier;"}}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Private binary carrier invented or omitted a runtime entry")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != after[owner].get(key):
                    raise RuntimeError("Original private binary carrier class metadata changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                if after[owner]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original private binary carrier method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_api else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Private binary carrier changed identities, callbacks, cast timing or exact failures")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, SDK or frozen observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
            "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_api": args.dependency_api,
            "static_helper": args.static_helper, "independent_wrong_programs": len(mutations),
            "private_full_generic_signature_and_erased_entry": "unchanged",
            "input_callbacks_opaque_elements_snapshot_and_failure_identity": "matched",
            "all_original_entries_metadata_and_exceptions": "unchanged",
            "original_inputs_and_frozen_callers": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
