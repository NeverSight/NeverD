#!/usr/bin/env python3
"""Verify class-owned generic implementation flags and complete recursive static receiver calls."""
from __future__ import annotations

import argparse
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
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory

OWNER = "Lfixture/Core;"
BRIDGES = {OWNER + "->contains(Ljava/lang/Comparable;)Z", OWNER + "->isEmpty()Z"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-root", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-class-formal-implementation-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted((FIXTURES / "class_formal_implementation_bridges/java").rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/ClassFormalImplementationBridgesHarness.java"], harness, classpath=originals)
        receipts = [change_method_flags(originals / "fixture/Core.class", prototype, 0x1, 0x41, name=name)
                    for name, prototype in ((b"contains", b"(Ljava/lang/Comparable;)Z"), (b"isEmpty", b"()Z"))]
        (work / "original-encoding.json").write_text(json.dumps(receipts, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        declarations = {"Lfixture/Api;", "Lfixture/Dispatch;"}
        classes = all_classes - (declarations if args.dependency_root else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        original_exceptions = exception_inventory(originals)
        dependency = work / "owned-helper.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Api", "Dispatch"):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_root:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "ClassFormalImplementationBridgesHarness", "original"], "baseline")
        lines = baseline.splitlines()
        match = re.fullmatch(r"values=(\d+),identities=(\d+),states=(\d+),failures=(\d+)", lines[0]) if len(lines) == 2 else None
        counts = tuple(map(int, match.groups())) if match else ()
        if counts != (112, 535, 416, 80) or lines[1] != "complete-class-and-method-scopes=retained,original-helper-and-virtual-callback-order=retained,bridge-flag-difference=reported":
            raise RuntimeError("Owned class-formal oracle changed: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned class-formal fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original class-formal runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 2 or coverage["status"] != "partial"
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != BRIDGES
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Class-formal implementation changed its original runtime inventory or scope")
        for identity in BRIDGES:
            row = rows[identity]
            if (row.get("projection_kind") != "full-contract-implementation-bridge"
                    or row.get("source_identity") != identity or not row.get("inherited_contract")
                    or row.get("bridge_target") or not row.get("metadata_differences")):
                raise RuntimeError("Original class-formal bridge lacks its complete original contract")
        if rows[OWNER + "->contains(Ljava/lang/Comparable;)Z"].get("generic_signature") != "(TT;)Z":
            raise RuntimeError("Original class-owned parameter Signature changed")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_root else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {entry["class"] + "->" + entry["name"] + entry["prototype"]
                   for entry in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != methods | helpers:
            raise RuntimeError("Rebuilt class-formal implementation invented or renamed a runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original class or field metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if method in BRIDGES:
                    wanted["access"] &= ~0x40
                if after["methods"][method] != wanted:
                    raise RuntimeError("Original method metadata changed beyond disclosed bridge flags: " + method)
        rebuilt_exceptions = exception_inventory(rebuilt)
        if any(rebuilt_exceptions[method] != original_exceptions[method] for method in methods):
            raise RuntimeError("Original exception metadata changed")
        runtime = (rebuilt, harness, dependency) if args.dependency_root else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             "ClassFormalImplementationBridgesHarness", "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original helper scopes, virtual callbacks, aliases or failure order changed")
        negative = work / "wrong-scope-originals"
        shutil.copytree(originals, negative)
        receipt = change_signature(negative / "fixture/Core.class", b"(TT;)Z", b"(TU;)Z")
        (work / "wrong-scope-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        bad_dex = dex(negative, work / "wrong-scope-dex")
        rejected = work / "wrong-scope-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(bad_dex), *map(str, classpath), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-scope-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists():
            raise RuntimeError("An unrelated same-erasure class formal was accepted")
        wrong_java = work / "wrong-java/fixture/Wrong.java"
        wrong_java.parent.mkdir(parents=True)
        wrong_java.write_text("package fixture; public abstract class Wrong<T extends Comparable<? super T>, U extends Comparable<? super U>> implements Api<T> {\n"
                             "  public boolean contains(U value) { return false; }\n}\n")
        invalid = subprocess.run([str(verify.javac), "--release", "8", "-proc:none", "-classpath", str(originals),
                                  "-d", str(work / "wrong-compiled"), str(wrong_java)],
                                 text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-javac.log").write_text(invalid.stdout + invalid.stderr)
        if invalid.returncode == 0 or "name clash" not in invalid.stderr:
            raise RuntimeError("Independent javac accepted unrelated class formals with identical erasures")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness, helper or SDK changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 2, "value_cases": counts[0], "identity_cases": counts[1],
                    "state_cases": counts[2], "failure_cases": counts[3],
                    "profile": "dependency-root" if args.dependency_root else "source-root",
                    "original_runtime_slots_scopes_bodies_and_exceptions": "unchanged",
                    "source_metadata_differences": "two original ACC_BRIDGE flags",
                    "same_code_scope_and_independent_javac_counterexamples": "rejected",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
