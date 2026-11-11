#!/usr/bin/env python3
"""Verify disclosed plain interface return names beside unchanged SDK scalar slots."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
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
    from scripts.test_mobile_android_final_getter_return_slots import rename_owned_entry
    from scripts.test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from scripts.test_mobile_android_upcast_result_bridges import result_cast
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_final_getter_return_slots import rename_owned_entry
    from test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from test_mobile_android_upcast_result_bridges import result_cast
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity


def hashes(directory: Path) -> dict[str, str]:
    return {p.relative_to(directory).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(directory.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--interface-covariance", action="store_true")
    parser.add_argument("--inherited-default", action="store_true")
    parser.add_argument("--source-prefix", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-plain-sdk-return-slots-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "plain_sdk_return_slots/java"
        inputs = sorted(fixture.rglob("*.java"))
        if args.interface_covariance or args.inherited_default:
            authored = fixture
            fixture = work / "owned-covariant-java"
            for source in inputs:
                content = source.read_text()
                if args.interface_covariance and source.name == "Api.java":
                    content = content.replace("    State sourceState();", "    State sourceState();\n    Base create();", 1)
                if args.interface_covariance and source.name == "Reader.java":
                    if not content.endswith("\n}\n"):
                        raise RuntimeError("Missing original plain SDK consumer anchor")
                    content = content[:-3] + "\n    public Leaf covariance;\n    public int covarianceCalls;\n" \
                        + "    public Leaf create() {\n        covarianceCalls += 3;\n" \
                        + "        if (failure != null) throw failure;\n        return covariance;\n    }\n" \
                        + "    public static Base checked(Object value) { return (Base) value; }\n}\n"
                if args.inherited_default and source.name == "Reader.java":
                    anchor = "implements Api {"
                    if content.count(anchor) != 1:
                        raise RuntimeError("Owned default consumer anchor changed")
                    content = content.replace(anchor, "implements Api, DefaultApi {")
                destination = fixture / source.relative_to(authored)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
            extras = (["sdk_parent_interface_covariance"] if args.interface_covariance else []) + (["default_return_slot_names"] if args.inherited_default else [])
            for extra in extras:
                for source in sorted((FIXTURES / extra / "java/fixture").glob("*.java")):
                    shutil.copyfile(source, fixture / "fixture" / source.name)
            inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        receipts = {}
        for owner in ("Api", "Reader", "Overriding", "Caller"):
            receipts[owner] = rename_owned_entry(originals / f"fixture/{owner}.class",
                                                b"sourceState", b"getState")
        (work / "owned-return-name-edits.json").write_text(json.dumps(receipts, indent=2) + "\n")
        if args.interface_covariance:
            receipt = result_cast(originals / "fixture/Reader.class")
            (work / "owned-interface-bridge-upcast.json").write_text(json.dumps(receipt, indent=2) + "\n")
        records = class_identity.compiler_classes(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        sdk_classes = {"Lfixture/SdkRoot;", "Lfixture/SdkWidget;"}
        external = {"Lfixture/SdkRoot;"} if args.source_prefix else sdk_classes
        classes = all_classes - external
        methods = {key: value for key, value in all_methods.items() if key.split("->", 1)[0] in classes}
        dependency = work / "owned-sdk-return-contract.jar"
        with zipfile.ZipFile(dependency, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for owner in sorted(sdk_classes):
                name = owner[1:-1] + ".class"
                archive.write(originals / name, name)
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        harness_inputs = [FIXTURES / "harness/PlainSdkReturnSlotsHarness.java"]
        harness_name = "PlainSdkReturnSlotsHarness"
        if args.interface_covariance:
            harness_inputs.append(FIXTURES / "harness/SdkParentInterfaceCovarianceHarness.java")
            harness_name = "SdkParentInterfaceCovarianceHarness"
        if args.inherited_default:
            observer = FIXTURES / "harness/DefaultReturnSlotNamesHarness.java"
            if args.interface_covariance:
                combined = work / "owned-harness/DefaultReturnSlotNamesHarness.java"
                combined.parent.mkdir()
                combined.write_text(observer.read_text().replace("PlainSdkReturnSlotsHarness.main(args);", "SdkParentInterfaceCovarianceHarness.main(args);"))
                observer = combined
            harness_inputs.append(observer)
            harness_name = "DefaultReturnSlotNamesHarness"
        verify.compile_local(harness_inputs, harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        harness_args = ["interface-covariance"] if args.interface_covariance else []
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_name, "getState", *harness_args], "baseline")
        expected = ["values=31,identities=205,states=126,failures=33",
                    "distinct-input-reference-and-unchanged-sdk-scalar-slots=matched,source-name-and-abi-differences=reported"]
        if args.interface_covariance:
            expected.extend(["sdk-parent-interface-covariance-identities=140,states=70,failures=10,original-bridge=1",
                             "plain-sdk-ancestry-and-original-upcast=retained,independent-subclass-and-erased-dispatch=matched"])
        if args.inherited_default:
            expected.append("default-calls=120,failures=80,original-provider-and-class-dispatch=matched")
        if baseline.splitlines() != expected:
            raise RuntimeError("Original separate return slots missed expected JVM cases: " + baseline)

        def make_dex(source: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                "--classpath", dependency, "--output", destination,
                *[p for p in sorted(source.rglob("*.class"))
                  if "L" + p.relative_to(source).as_posix()[:-6] + ";" in classes]], "d8-" + destination.name)
            dex = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Separate return-slot fixture changed original DEX inventory")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed original separate return-slot entries")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "--classpath", dependency, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        named = {row["identity"]: row for row in coverage["methods"] if row.get("source_name")}
        wanted = {"Lfixture/" + owner + ";->getState()Lfixture/State;" for owner in ("Api", "Reader", "Overriding")}
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial"
                or coverage["projected_method_count"] != (4 if args.interface_covariance else 3) or set(named) != wanted
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Plain return-slot recovery lost inventory or omitted source projection disclosure")
        aliases = {row["source_name"] for row in named.values()}
        if len(aliases) != 1 or "__neverdReturnSlot" in aliases:
            raise RuntimeError("Plain return-slot family selected inconsistent or colliding source names")
        alias = next(iter(aliases))
        for row in named.values():
            if (row.get("projection_kind") != "incompatible-inherited-return-slot"
                    or row.get("source_name_declaration") != "Lfixture/Api;->getState()Lfixture/State;"
                    or row.get("conflicting_inherited_contracts") != ["Lfixture/SdkRoot;->getState()I"]
                    or not row.get("metadata_differences")):
                raise RuntimeError("Plain return-slot projection lacks original branch and unchanged SDK ownership")
        if args.interface_covariance:
            row = next(row for row in coverage["methods"] if row["identity"] == "Lfixture/Reader;->create()Lfixture/Base;")
            if row.get("bridge_target") != "Lfixture/Reader;->create()Lfixture/Leaf;" or not row.get("projection_kind"):
                raise RuntimeError("Original plain interface covariant bridge lacks its complete forwarding receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        expected_methods = {row.get("source_identity", row["identity"]) for row in coverage["methods"]}
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != expected_methods | helpers:
            raise RuntimeError("javac changed an unreported separate return-slot entry")
        after = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                if records[owner].get(key) != after[owner].get(key):
                    raise RuntimeError("Original return-slot class metadata changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                target = named.get(identity, {}).get("source_identity", identity)
                expected = dict(metadata)
                if identity in named:
                    expected["name"] = alias
                if after[owner]["methods"][target] != expected:
                    raise RuntimeError("Return-slot method metadata changed beyond reported name: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             harness_name, alias, *harness_args], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Separate slots changed virtual dispatch, alias identity, effects or failure order")
        if args.inherited_default:
            wrong = work / "wrong-default-java/fixture/DefaultApi.java"
            wrong.parent.mkdir(parents=True)
            content = (fixture / "fixture/DefaultApi.java").read_text()
            if content.count("Object value = values[0];") != 1:
                raise RuntimeError("Owned default array-read anchor changed")
            wrong.write_text(content.replace("Object value = values[0];", "Object value = null;"))
            compiled = work / "wrong-default-classes"
            verify.compile_local([wrong], compiled, classpath=originals)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                harness_name, "getState", *harness_args], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "wrong-default-counterexample.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "default return-slot contract" not in result.stderr:
                raise RuntimeError("Independent observer missed changed inherited default effects")
        negative = work / "getter-formal-originals"
        shutil.copytree(originals, negative)
        receipt = add_parameter_signature(negative / "fixture/Reader.class", "<X:Ljava/lang/Object;>()Lfixture/State;",
                                         method_name="getState", prototype="()Lfixture/State;")
        (work / "getter-formal-same-code.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = make_dex(negative, work / "getter-formal-dex")
        rejected = work / "getter-formal-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
            str(args.android_jar.resolve()), "--classpath", str(dependency), "-o", str(rejected)],
            text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "getter-formal-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Changed full getter formal escaped closed return-slot validation")
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "Reader.java":
                declaration = "public State sourceState()"
                if content.count(declaration) != 1:
                    raise RuntimeError("Missing original owned reference getter")
                content = content.replace(declaration, "public <X> State sourceState()", 1)
            destination = work / "getter-formal-java" / source.relative_to(fixture)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
            str(work / "getter-formal-compiler"), *compiler_inputs], text=True, capture_output=True,
            timeout=300, env=verify.environment)
        (work / "getter-formal-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or not any(message in compiler.stderr for message in ("does not override", "name clash")):
            raise RuntimeError("Independent javac accepted incompatible plain getter formal")
        if args.interface_covariance:
            negative = work / "wrong-interface-upcast-originals"
            shutil.copytree(originals, negative)
            receipt = result_cast(negative / "fixture/Reader.class", wrong_type=True)
            (work / "wrong-interface-upcast-edit.json").write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = make_dex(negative, work / "wrong-interface-upcast-dex")
            rejected = work / "wrong-interface-upcast-rejected"
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                str(args.android_jar.resolve()), "--classpath", str(dependency), "-o", str(rejected)],
                text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "wrong-interface-upcast-recovery.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic bridge" not in result.stderr:
                raise RuntimeError("Changed original interface result cast escaped forwarding validation")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original return-slot input, immutable harness or SDK dependency changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
            "value_cases": 31, "identity_cases": 205, "state_observations": 126, "ordered_failure_cases": 33,
            "source_named_methods": 3, "coverage": "partial", "original_runtime_inventory": "retained",
            "complete_metadata": "unchanged except three disclosed source names",
            "sdk_scalar_and_reference_family_dispatch": "matched", "mutable_aliases_and_ordered_failure_identity": "matched",
            "unchanged_sdk_and_immutable_harness": "retained", "method_formal_counterexample": "same Code; rejected by NeverD and javac"}
        evidence["source_prefix"] = args.source_prefix
        if args.inherited_default:
            evidence["inherited_default"] = "120 calls and 80 expected failures; original provider, subclass dispatch and wide bits matched"
            evidence["changed_default_counterexample"] = "detected by immutable observer"
        if args.interface_covariance:
            evidence["plain_sdk_parent_interface_covariance"] = "complete original nominal result and exact upcast bridge retained"
            evidence["interface_covariant_dispatch"] = "140 identities, 70 state observations and ten ordered failures matched"
            evidence["original_covariant_bridges"] = 1
            evidence["wrong_interface_result_cast"] = "rejected by the common bridge proof"
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
