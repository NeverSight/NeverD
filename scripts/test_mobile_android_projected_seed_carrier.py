#!/usr/bin/env python3
"""Verify independent method seeds after a proven static wrapper scope projection."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
from copy import deepcopy
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
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags


OWNER = "Lfixture/Carrier;"
SEND = OWNER + "->send(Ljava/lang/Object;Ljava/lang/Object;Lfixture/Callback;)V"
SOURCE_SIGNATURE = (b"<V:Ljava/lang/Object;>(Ljava/lang/Object;TV;"
                    b"Lfixture/Callback<-Ljava/lang/Object;-TV;Ljava/lang/Object;>;)V")
INPUT_SIGNATURE = SOURCE_SIGNATURE.replace(b"-Ljava/lang/Object;", b"-TT;", 1)
SYNTHETICS = ("<init>(Ljava/lang/Object;)V", "wrap(Ljava/lang/Object;)Lfixture/Carrier;",
              "unwrap()Ljava/lang/Object;")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-callback", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-projected-seed-carrier-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "projected_seed_carrier/java"
        authored, originals, harness = work / "authored", work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), authored, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("ProjectedSeedCarrierHarness", "ProjectedSeedCarrierCaller")], harness,
                             classpath=authored, release=8)
        authored_hashes, harness_hashes = hashes(authored), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, authored)
        if len(all_classes) != 4 or len(all_methods) != 9:
            raise RuntimeError("Owned projected seed inventory changed: " + repr((len(all_classes), len(all_methods))))

        def execute(runtime, label, metadata):
            outputs = []
            for route in range(4):
                for mode in range(8):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "ProjectedSeedCarrierHarness", str(route), str(mode), metadata], f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Original projected seed matrix changed: " + output)
                    outputs.append(output)
            return outputs

        baseline = execute((authored, harness), "baseline", "source")
        shutil.copytree(authored, originals)
        target = originals / "fixture/Carrier.class"
        receipt = {"signature": change_signature(target, SOURCE_SIGNATURE, INPUT_SIGNATURE), "flags": []}
        for slot, before in zip(SYNTHETICS, (2, 0x19, 0x11)):
            name, descriptor = slot.split("(", 1)
            receipt["flags"].append(change_method_flags(target, ("(" + descriptor).encode(), before,
                                                        before | 0x1000, name=name.encode()))
        (work / "owned-input-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        original_hashes = hashes(originals)
        if execute((originals, harness), "input", "input") != baseline:
            raise RuntimeError("Independent Signature/flag encoding changed original execution")
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        if records[OWNER]["methods"][SEND]["signature"] != INPUT_SIGNATURE.decode():
            raise RuntimeError("Encoded input lost the two distinct formal owners")
        classes = all_classes - ({"Lfixture/Callback;"} if args.dependency_callback else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        dependency = work / "owned-callback.jar"
        with zipfile.ZipFile(dependency, "x") as archive:
            archive.writestr("fixture/Callback.class", (originals / "fixture/Callback.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_callback:
            classpath += ["--classpath", dependency]
        dex_dir = work / "dex"
        dex_dir.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex_dir, *paths], "d8-original")
        source_dex = dex_dir / "classes.dex"
        if list(dex_dir.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned projected seed fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original projected seed runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial"
                or coverage.get("class_source_bindings") or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Projected seed recovery changed original inventory or disclosure")
        projections = coverage.get("generic_signature_projections", [])
        if (len(projections) != 1 or projections[0]["original_signature"] != INPUT_SIGNATURE.decode()
                or projections[0]["declaration"] != SEND
                or not projections[0].get("metadata_differences")
                or projections[0]["changes"] != [{"kind": "static-carrier-type-variable",
                    "original_type": OWNER + "::T", "scope_origin": OWNER, "source_type": "Ljava/lang/Object;",
                    "offset": INPUT_SIGNATURE.index(b"TT;")}]):
            raise RuntimeError("Missing exact class-formal projection or changed independent method formal")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=8, classpath=dependency if args.dependency_callback else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes - {"Lfixture/Callback;"}}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Projected seed recovery invented or omitted a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original payload class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                expected = deepcopy(metadata)
                if identity == SEND:
                    expected["signature"] = SOURCE_SIGNATURE.decode()
                if identity in {OWNER + "->" + slot for slot in SYNTHETICS}:
                    expected["access"] &= ~0x1000
                if after["methods"].get(identity) != expected or actual_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Undisclosed method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_callback else (rebuilt, harness)
        if execute(runtime, "rebuilt", "source") != baseline:
            raise RuntimeError("Projected seed call changed effects, aliases or failure identity")
        mutations = (
            ("repeated-callback", "Carrier", "Trace.hit(3);", "callback.apply(carrier, value); Trace.hit(3);"),
            ("discarded-result", "Carrier", "Trace.result = result;", "Trace.result = null;"),
            ("swapped-arguments", "Core", "Carrier.send(carrier, value, callback);", "Carrier.send(value, carrier, callback);"),
        )
        for label, owner, old, new in mutations:
            text = (fixture / ("fixture/" + owner + ".java")).read_text()
            if text.count(old) != 1:
                raise RuntimeError("Independent projected seed counterexample anchor changed")
            wrong = work / label / ("fixture/" + owner + ".java")
            wrong.parent.mkdir(parents=True)
            wrong.write_text(text.replace(old, new))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=authored, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, authored, harness))),
                                    "ProjectedSeedCarrierHarness", "1", "0", "source"], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned projected seed contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(authored) != authored_hashes
                or hashes(harness) != harness_hashes or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_callback": args.dependency_callback, "fresh_jvm_cases_per_stage": len(baseline),
                    "stages": 3, "attempts_per_case": 2, "class_scope_projection": "exact-Object-only",
                    "independent_method_seed_metadata": "unchanged", "other_metadata_and_exceptions": "unchanged",
                    "synthetic_flags": "three fixture-encoded flags removed",
                    "mutable_callback_aliases_and_failure_identity": "matched",
                    "independent_compiler_counterexamples": len(mutations), "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
