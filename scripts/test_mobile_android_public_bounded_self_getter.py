#!/usr/bin/env python3
"""Verify a public erased getter with a complete recursive nominal result bound."""
from __future__ import annotations

import argparse
import copy
from contextlib import nullcontext
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
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_protected_closed_getter import encode_public_erased_entry
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_protected_closed_getter import encode_public_erased_entry


def hashes(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parent", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-public-bounded-self-getter-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = FIXTURES / "public_bounded_self_getter/java"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/PublicBoundedSelfGetterHarness.java"], harness,
                             classpath=originals)
        owner = "Lfixture/OwnedSelf;"
        bridge = owner + "->self()Lfixture/Base;"
        target_method = owner + "->self()Lfixture/OwnedSelf;"
        bridges = {bridge, target_method}

        def encode(root: Path) -> list:
            target = root / "fixture/OwnedSelf.class"
            return [encode_public_erased_entry(target, name=b"self", prototype=b"()Lfixture/Base;")]

        (work / "owned-input-flags.json").write_text(json.dumps(encode(originals), indent=2) + "\n")
        records = class_identity.compiler_classes(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - {"Lfixture/Base;"} if args.dependency_parent else all_classes
        methods = {k: v for k, v in all_methods.items() if k.split("->", 1)[0] in classes}
        if classes != ({owner} if args.dependency_parent else {owner, "Lfixture/Base;"}) or len(methods) != (4 if args.dependency_parent else 7):
            raise RuntimeError("Owned bounded getter declaration inventory changed")
        dependency = work / "owned-parent.jar"
        with zipfile.ZipFile(dependency, "x", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("fixture/Base.class", (originals / "fixture/Base.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "PublicBoundedSelfGetterHarness", "original"], "baseline")
        lines = baseline.splitlines()
        cases = re.fullmatch(r"values=(\d+),identities=(\d+),failures=(\d+)", lines[0]) if lines else None
        if (not cases or int(cases[1]) != 845 or int(cases[2]) != 604 or int(cases[3]) != 5 or
                lines[1:] != ["public-erased-nominal-entry-and-recursive-bound=matched,target-access=reported"]):
            raise RuntimeError("Owned bounded getter baseline missed identities, effects or failures: " + baseline)

        def make_dex(root: Path, output: Path, extra_classes=frozenset()) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--classpath", dependency, "--output", output,
                        *[p for p in sorted(root.rglob("*.class"))
                          if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes | set(extra_classes)]], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned protected-getter DEX inventory changed")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original protected-getter declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        classpath = ["--classpath", str(args.android_jar.resolve())]
        if args.dependency_parent:
            classpath += ["--classpath", str(dependency)]
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] or coverage["status"] != "partial" or
                coverage.get("generic_signature_projections") or
                {r["identity"] for r in rows.values() if r["status"] == "source-projected"} != bridges or
                rows[target_method].get("projection_kind") != "public-erased-protected-getter-target" or
                any(not rows[key].get("metadata_differences") for key in bridges)):
            raise RuntimeError("Protected-getter recovery lost its exact superclass or forwarding receipts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_parent else None,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("javac changed inventory beyond reported helpers")
        actual_records = class_identity.compiler_classes(rebuilt)
        for original_owner in classes:
            wanted = copy.deepcopy(records[original_owner])
            if original_owner == owner:
                wanted["methods"][target_method]["access"] = 0x1
            after = actual_records[original_owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != wanted.get(key):
                    raise RuntimeError("Original bounded getter class metadata changed: " + original_owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original bounded getter method metadata changed beyond target access: " + identity)
        runtime_path = (rebuilt, harness, dependency) if args.dependency_parent else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime_path)),
                             "PublicBoundedSelfGetterHarness", "rebuilt"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Recursive bound, public erased dispatch, result identity or exceptions changed")

        def reject(root: Path, label: str) -> None:
            negative_dex = make_dex(root, work / ("negative-" + label + "-dex"),
                                    {"Lfixture/GetterChild;"} if label == "owner" else set())
            rejected = work / ("rejected-" + label)
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *classpath, "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / ("negative-" + label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Protected-getter proof accepted changed inherited arguments or a conflicting source descendant")

        negative = work / "negative-scope"
        shutil.copytree(originals, negative)
        receipt = change_signature(negative / "fixture/OwnedSelf.class",
                                   b"Lfixture/Base<Lfixture/OwnedSelf;>;",
                                   b"Lfixture/Base<Ljava/lang/String;>;")
        (work / "negative-scope-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        reject(negative, "scope")
        negative_sources = work / "negative-owner-java"
        shutil.copytree(fixture, negative_sources)
        target = negative_sources / "fixture/OwnedSelf.java"
        text = target.read_text()
        statement = "public final class OwnedSelf"
        if text.count(statement) != 1:
            raise RuntimeError("Missing exact independently authored final getter owner")
        target.write_text(text.replace(statement, "public class OwnedSelf", 1))
        (negative_sources / "fixture/GetterChild.java").write_text(
            "package fixture; public final class GetterChild extends OwnedSelf {\n"
            "  @Override protected OwnedSelf self() { return this; }\n}\n")
        negative_owner = work / "negative-owner"
        verify.compile_local(sorted(negative_sources.rglob("*.java")), negative_owner)
        (work / "negative-owner-flags.json").write_text(json.dumps(encode(negative_owner), indent=2) + "\n")
        reject(negative_owner, "owner")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or
                hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash or
                hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original input, SDK declarations or immutable harness bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods), "dependency_parent": args.dependency_parent,
                    "value_cases": int(cases[1]), "identity_cases": int(cases[2]), "failure_cases": 5,
                    "original_public_erased_nominal_getter_and_recursive_bound": "matched",
                    "target_access_projection": "protected to public",
                    "full_metadata_and_complete_inventory": "unchanged except reported target access and helpers",
                    "scope_and_source_descendant_counterexamples": "rejected",
                    "input_sdk_and_immutable_harness": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
