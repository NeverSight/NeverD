#!/usr/bin/env python3
"""Verify retained checked reference bridges and separate original JVM return slots."""
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
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_static_defaults import method_code
    from scripts.test_mobile_android_shared_upper_bounds import class_records
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_static_defaults import method_code
    from test_mobile_android_shared_upper_bounds import class_records


PARAMETERS = "(Ljava/lang/Object;Lfixture/Box;J)"


def rename_utf8(path: Path, old: str, new: str) -> None:
    before = path.read_bytes()
    pattern = b"\x01" + len(old.encode()).to_bytes(2, "big") + old.encode()
    replacement = b"\x01" + len(new.encode()).to_bytes(2, "big") + new.encode()
    if before.count(pattern) != 1:
        raise RuntimeError("Owned method name is not one exact constant-pool entry")
    after = before.replace(pattern, replacement, 1)
    wanted = {key.replace((old + "(").encode(), (new + "(").encode()): value
              for key, value in method_code(before).items()}
    if (method_code(after) != wanted
            or before[class_records(before)["pool_end"]:] != after[class_records(after)["pool_end"]:]):
        raise RuntimeError("Name encoding changed method Code or declaration structure")
    path.write_bytes(after)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--mangled", action="store_true")
    parser.add_argument("--dependency-callback", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-checked-reference-return-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "checked_reference_return/java"
        authored, originals, harness = work / "authored", work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), authored, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("CheckedReferenceReturnHarness", "CheckedReferenceReturnCaller")], harness,
                             classpath=authored, release=8)
        authored_hashes, harness_hashes = hashes(authored), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, authored)
        if len(all_classes) != 6 or len(all_methods) != 11:
            raise RuntimeError("Owned checked reference inventory changed: " + repr((len(all_classes), len(all_methods))))

        def execute(runtime, label, name, target):
            outputs = []
            for route in range(4):
                for mode in range(10):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "CheckedReferenceReturnHarness", str(route), str(mode), name, target], f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Original checked reference matrix changed: " + output)
                    outputs.append(output)
            return outputs

        baseline = execute((authored, harness), "baseline", "read", "target")
        shutil.copytree(authored, originals)
        core = originals / "fixture/Core.class"
        original_name = "read-owned" if args.mangled else "read"
        if args.mangled:
            for relative in ("Api", "Core", "Client"):
                rename_utf8(originals / ("fixture/" + relative + ".class"), "read", original_name)
        rename_utf8(core, "target", original_name)
        change_method_flags(core, (PARAMETERS + "Lfixture/Result;").encode(), 1, 0x1041,
                            name=original_name.encode())
        (work / "owned-input-encoding.json").write_text(json.dumps({"target_name": "target to read",
            "shared_name": original_name, "bridge_flags": "public to public/bridge/synthetic",
            "all_method_code_and_descriptors": "unchanged"}, indent=2) + "\n")
        original_hashes = hashes(originals)
        if execute((originals, harness), "input", original_name, original_name) != baseline:
            raise RuntimeError("Independent JVM name/flag encoding changed original execution")
        all_classes, all_methods = compiler_inventory(verify, originals)
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        classes = all_classes - ({"Lfixture/Box;"} if args.dependency_callback else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        owner = "Lfixture/Core;"
        target = owner + "->" + original_name + PARAMETERS + "Ljava/lang/Void;"
        bridge = owner + "->" + original_name + PARAMETERS + "Lfixture/Result;"
        api = "Lfixture/Api;->" + original_name + PARAMETERS + "Lfixture/Result;"
        family = {api, bridge}
        dependency = work / "owned-callback.jar"
        with zipfile.ZipFile(dependency, "x") as archive:
            archive.writestr("fixture/Box.class", (originals / "fixture/Box.class").read_bytes())
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
            raise RuntimeError("Owned checked reference fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original checked reference runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial"
                or coverage.get("projected_method_count") != (3 if args.mangled else 2)
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")):
            raise RuntimeError("Checked reference recovery changed original inventory or disclosure")
        if (rows[target].get("projection_kind") != "checked-reference-target-alias"
                or rows[target].get("original_bridge") != bridge
                or not rows[target].get("metadata_differences")
                or rows[bridge].get("body_projection_kind" if args.mangled else "projection_kind") != "checked-reference-return-bridge"):
            raise RuntimeError("Missing independent return slot and original checked bridge receipts")
        target_name = rows[target]["source_name"]
        source_name = rows[bridge].get("source_name", original_name)
        if args.mangled and any(rows[identity].get("source_name") != source_name
                               or set(rows[identity].get("source_name_family", [])) != family for identity in family):
            raise RuntimeError("Checked bridge lost the complete virtual name family")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=8, classpath=dependency if args.dependency_callback else None)
        rename = {target: target.replace("->" + original_name + "(", "->" + target_name + "(")}
        if args.mangled:
            rename.update({identity: identity.replace("->" + original_name + "(", "->" + source_name + "(") for identity in family})
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes - {"Lfixture/Api;", "Lfixture/Box;"}}
        if (actual_classes != classes or set(actual_methods) != {rename.get(m, m) for m in methods} | expected_helpers
                or helpers != expected_helpers):
            raise RuntimeError("Checked reference recovery invented or omitted a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for c in classes:
            before, after = records[c], actual_records[c]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original payload class metadata changed: " + c + ":" + key)
            for identity, metadata in before["methods"].items():
                expected = deepcopy(metadata)
                selected = rename.get(identity, identity)
                expected["name"] = selected.split("->", 1)[1].split("(", 1)[0]
                if identity == bridge:
                    expected["access"] &= ~0x1040
                if after["methods"].get(selected) != expected or actual_exceptions.get(selected) != exceptions[identity]:
                    raise RuntimeError("Undisclosed method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_callback else (rebuilt, harness)
        if execute(runtime, "rebuilt", source_name, target_name) != baseline:
            raise RuntimeError("Checked reference call changed effects, aliases or failure identity")
        mutations = (
            ("repeated-callback", "Core", "Trace.hit(3);", "callback.apply(input, value); Trace.hit(3);"),
            ("discarded-result", "Core", "Trace.result = result;", "Trace.result = null;"),
            ("narrowed-wide", "Client", "receiver.read(input, callback, value)", "receiver.read(input, callback, (int)value)"),
        )
        for label, name, old, new in mutations:
            text = (fixture / ("fixture/" + name + ".java")).read_text()
            if text.count(old) != 1:
                raise RuntimeError("Independent checked reference counterexample anchor changed")
            wrong = work / label / ("fixture/" + name + ".java")
            wrong.parent.mkdir(parents=True)
            wrong.write_text(text.replace(old, new))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=authored, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, authored, harness))),
                                    "CheckedReferenceReturnHarness", "1", "0", "read", "target"], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned checked reference return contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(authored) != authored_hashes
                or hashes(harness) != harness_hashes or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "mangled": args.mangled, "dependency_callback": args.dependency_callback,
                    "fresh_jvm_cases_per_stage": len(baseline), "stages": 3, "attempts_per_case": 2,
                    "null_return_and_original_checked_bridge": "matched", "other_metadata_and_exceptions": "unchanged",
                    "return_slot_and_name_changes": "disclosed", "mutable_callback_aliases_and_failure_identity": "matched",
                    "independent_compiler_counterexamples": len(mutations), "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
