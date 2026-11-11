#!/usr/bin/env python3
"""Verify a witnessed nested return variance projection with unchanged bytecode."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code

SOURCE_SIGNATURE = b"()Ljava/util/Iterator<Lfixture/Pair<+TK;+TV;>;>;"
INPUT_SIGNATURE = b"()Ljava/util/Iterator<Lfixture/Pair<TK;TV;>;>;"
IDENTITY = "Lfixture/View;->values()Ljava/util/Iterator;"


def change_signature(path: Path, original: bytes, changed: bytes) -> dict:
    before = path.read_bytes()
    old = b"\x01" + len(original).to_bytes(2, "big") + original
    new = b"\x01" + len(changed).to_bytes(2, "big") + changed
    if before.count(old) != 1:
        raise RuntimeError("Missing exact independently authored return Signature")
    after = before.replace(old, new, 1)
    if (method_code(before) != method_code(after)
            or before[class_records(before)["pool_end"]:] != after[class_records(after)["pool_end"]:]):
        raise RuntimeError("Return Signature edit changed descriptors or Code")
    path.write_bytes(after)
    return {"original_signature": original.decode(), "input_signature": changed.decode(),
            "code_and_descriptors": "unchanged", "before_sha256": hashlib.sha256(before).hexdigest(),
            "after_sha256": hashlib.sha256(after).hexdigest()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--class-ancestor", action="store_true")
    parser.add_argument("--dependency-root", action="store_true")
    args = parser.parse_args()
    args.class_ancestor |= args.dependency_root
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nested-return-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "nested_return_variance/java"
        inputs = sorted(fixture.rglob("*.java"))
        declarations = {"Lfixture/Contract;"}
        if args.class_ancestor:
            inherited = FIXTURES / "nested_return_ancestors/java"
            authored = work / "authored/fixture/View.java"
            authored.parent.mkdir(parents=True)
            authored.write_text((fixture / "fixture/View.java").read_text().replace(
                "implements Contract<Pair<? extends K,? extends V>>",
                "extends NestedMiddle<K,V> implements NestedBranch<K,V>"))
            inputs = [p for p in inputs if p.name != "View.java"]
            inputs += [authored, *sorted(inherited.rglob("*.java"))]
            declarations.add("Lfixture/Marker;")
        if args.dependency_root:
            declarations.update({"Lfixture/NestedRoot;", "Lfixture/NestedBranch;"})
        originals, harness_dir = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - declarations
        methods = {k: v for k, v in all_methods.items() if k.split("->", 1)[0] in classes}
        harness_source = FIXTURES / "harness/NestedReturnVarianceHarness.java"
        if args.class_ancestor:
            text = harness_source.read_text()
            text = text.replace(
                '        ParameterizedType contract = (ParameterizedType) View.class.getGenericInterfaces()[0];\n'
                '        ParameterizedType required = (ParameterizedType) contract.getActualTypeArguments()[0];',
                '        if (!View.class.getGenericSuperclass().getTypeName().equals("fixture.NestedMiddle<K, V>")\n'
                '                || !View.class.getGenericInterfaces()[0].getTypeName().equals("fixture.NestedBranch<K, V>")\n'
                '                || !NestedMiddle.class.getGenericSuperclass().getTypeName().equals("fixture.NestedRoot<M, N>")\n'
                '                || !NestedRoot.class.getGenericInterfaces()[0].getTypeName().equals("fixture.NestedBranch<R, S>")\n'
                '                || !NestedBranch.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Contract<fixture.Pair<? extends X, ? extends Y>>")\n'
                '                || NestedBranch.class.getInterfaces()[1] != Marker.class)\n'
                '            throw new AssertionError("complete ancestor metadata changed");')
            text = text.replace(
                '            Type expected = required.getActualTypeArguments()[i];\n'
                '            if (!(expected instanceof WildcardType)) throw new AssertionError("interface wildcard lost");\n'
                '            if (!((WildcardType) expected).getUpperBounds()[0].equals(formals[i]))\n'
                '                throw new AssertionError("interface scope changed");\n', '')
            text = text.replace('if (!expected.equals(actual))',
                'if (!(actual instanceof WildcardType) || !((WildcardType) actual).getUpperBounds()[0].equals(formals[i]))')
            harness_source = work / "authored/NestedReturnVarianceHarness.java"
            harness_source.write_text(text)
        verify.compile_local([harness_source], harness_dir, classpath=originals)
        harness_hashes = {p.relative_to(harness_dir).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sorted(harness_dir.rglob("*.class"))}
        dependency = work / "owned-contract.jar"
        with zipfile.ZipFile(dependency, "x", zipfile.ZIP_DEFLATED) as archive:
            for descriptor in sorted(declarations):
                path = descriptor[1:-1] + ".class"
                archive.writestr(path, (originals / path).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        receipt = change_signature(originals / "fixture/View.class", SOURCE_SIGNATURE, INPUT_SIGNATURE)
        (work / "input-metadata-change.json").write_text(json.dumps(receipt, indent=2) + "\n")
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               "NestedReturnVarianceHarness", "original"], "baseline")
        expected = ["values=10,identities=14,failures=4",
                    "nested-return=matched,metadata-difference=reported,no-bridges=retained"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned nested return baseline missed expected cases: " + baseline)

        def dex(directory: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--classpath", dependency, "--output", destination,
                        *(directory / (descriptor[1:-1] + ".class")
                          for descriptor in sorted(classes))], "d8-" + destination.name)
            return destination / "classes.dex"

        owned = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", owned], "sdk-inventory")
        inventory = parse_dexdump(dump, owned.name, dex_header(owned.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes
                or {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned input inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", owned, "--classpath", args.android_jar.resolve(),
                    "--classpath", dependency, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        projections = coverage.get("generic_signature_projections", [])
        if ({r["identity"] for r in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage.get("projected_method_count", 0) != 0 or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial" or len(projections) != 1
                or projections[0]["declaration"] != IDENTITY
                or projections[0]["original_signature"] != INPUT_SIGNATURE.decode()
                or len(projections[0]["changes"]) != 1
                or projections[0]["changes"][0]["kind"] != "inherited-nested-return-variance"
                or projections[0]["changes"][0]["source_type"] != SOURCE_SIGNATURE.decode()
                or projections[0]["changes"][0]["scope_origin"] != (
                    "Lfixture/NestedRoot;->values()Ljava/util/Iterator;" if args.class_ancestor
                    else "Lfixture/Contract;->values()Ljava/util/Iterator;")):
            raise RuntimeError("Return variance projection lost its exact witness or body")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("javac changed the complete original/helper inventory")
        original_records = class_identity.compiler_classes(originals)
        actual_records = class_identity.compiler_classes(rebuilt)
        for descriptor in classes:
            original_record = original_records[descriptor]
            actual_record = actual_records[descriptor]
            for key in ("signature", "superclass", "interfaces", "fields"):
                if actual_record.get(key) != original_record.get(key):
                    raise RuntimeError("Original class metadata changed: " + descriptor + ":" + key)
            for identity, original in original_record["methods"].items():
                expected_signature = SOURCE_SIGNATURE.decode() if identity == IDENTITY else original.get("signature")
                if actual_record["methods"][identity].get("signature") != expected_signature:
                    raise RuntimeError("Original generic method metadata changed: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir, dependency))),
                             "NestedReturnVarianceHarness", "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Return projection changed identity, effects or ordered failures")
        negative = work / "negative-originals"
        (negative / "fixture").mkdir(parents=True)
        for p in originals.glob("fixture/*.class"):
            (negative / "fixture" / p.name).write_bytes(p.read_bytes())
        receipt = change_signature(negative / "fixture/View.class", INPUT_SIGNATURE,
                                   INPUT_SIGNATURE.replace(b"TV;", b"TK;"))
        (work / "negative-metadata-change.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = dex(negative, work / "negative-dex")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "--classpath", str(dependency), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic inheritance" not in result.stderr:
            raise RuntimeError("Same-erasure return formal drift was silently accepted")
        # Independently confirm that the input Signature cannot be spelled as the original Java override.
        invalid = work / "invalid-java/fixture/View.java"
        invalid.parent.mkdir(parents=True)
        source = next(p for p in inputs if p.name == "View.java").read_text()
        invalid.write_text(source.replace("Iterator<Pair<? extends K,? extends V>>", "Iterator<Pair<K,V>>"))
        compiler = subprocess.run([str(verify.javac), "--release", "8", "-cp", str(originals),
                                   "-d", str(work / "invalid-compiler"), str(invalid)],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "invalid-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("javac unexpectedly accepted the unprojected nested override")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted(originals.rglob("*.class"))}:
            raise RuntimeError("Original owned class bytes changed")
        if hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash:
            raise RuntimeError("Original dependency declaration changed")
        if harness_hashes != {p.relative_to(harness_dir).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in sorted(harness_dir.rglob("*.class"))}:
            raise RuntimeError("Original independent harness changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "complete_generic_ancestors": args.class_ancestor, "unchanged_dependency_root": args.dependency_root,
                    "identity_cases": 14, "value_cases": 10, "ordered_failure_cases": 4,
                    "runtime_descriptors_and_bodies": "retained", "dependency_sources_emitted": 0,
                    "generic_field_and_interface_reflection": "unchanged", "return_reflection_difference": "reported",
                    "invented_bridges": 0, "coverage": "partial", "same_erasure_counterexample": "rejected",
                    "unprojected_source": "rejected-by-javac", "jvm_output": "matched"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
