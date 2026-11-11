#!/usr/bin/env python3
"""Verify named anonymous member allocations with original generic ancestry and effects."""
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
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature


ANONYMOUS = "Lfixture/Owner$1;"
PROVIDERS = {"Lfixture/GenericBase;", "Lfixture/Names;"}
BRIDGES = {
    ANONYMOUS + "->get(Ljava/lang/Object;)Ljava/lang/Object;": ANONYMOUS + "->get(Lfixture/Owner;)Ljava/lang/Float;",
    ANONYMOUS + "->set(Ljava/lang/Object;Ljava/lang/Object;)V": ANONYMOUS + "->set(Lfixture/Owner;Ljava/lang/Float;)V",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--conditional", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-anonymous-member-allocation-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "anonymous_member_allocations/java"
        if args.conditional:
            authored = work / "conditional-java"
            shutil.copytree(fixture, authored)
            owner = authored / "fixture/Owner.java"
            content = owner.read_text()
            if content.count("Float.class, Names.first") != 1:
                raise RuntimeError("Owned anonymous allocation argument anchor changed")
            owner.write_text(content.replace("Float.class, Names.first", "Float.class, Trace.choose ? Names.first : Names.second"))
            fixture = authored
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("AnonymousMemberAllocationCaller", "AnonymousMemberAllocationHarness")],
                             harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (6, 17):
            raise RuntimeError("Owned anonymous member allocation inventory changed: " + repr((len(all_classes), len(all_methods))))
        classes = all_classes - (PROVIDERS if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        shape = records[ANONYMOUS]
        signature = "Lfixture/GenericBase<Lfixture/Owner;Ljava/lang/Float;>;"
        if (shape["fields"] or shape["signature"] != signature or shape["superclass"] != "Lfixture/GenericBase;"
                or shape["enclosing_method"] != {"owner": "Lfixture/Owner;", "name": None, "prototype": None}
                or shape["inner_class"] != {"inner": ANONYMOUS, "outer": None, "name": None, "access": 0}
                or len(shape["methods"]) != 5
                or {identity for identity, metadata in shape["methods"].items() if metadata["access"] & 0x40} != set(BRIDGES)):
            raise RuntimeError("Original anonymous generic member shape changed")
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for owner in sorted(PROVIDERS):
                relative = owner[1:-1] + ".class"
                archive.writestr(relative, (originals / relative).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label, recovered):
            outputs = []
            for route in range(4):
                for mode in range(9):
                    for choose in (False, True):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "AnonymousMemberAllocationHarness", str(route), str(mode), str(choose).lower(),
                            str(args.conditional).lower(), str(recovered).lower()], f"{label}-{route}-{mode}-{choose}")
                        expected = f"route={route},mode={mode},choose={str(choose).lower()},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Original anonymous allocation matrix changed: " + output)
                        outputs.append(output)
            return outputs

        def dex(root, label):
            destination = work / label
            destination.mkdir()
            inputs = [path for path in sorted(root.rglob("*.class"))
                      if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *inputs], "d8-" + label)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned anonymous allocation changed DEX inventory")
            return result

        baseline = execute((originals, harness), "baseline", False)
        source_dex = dex(originals, "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original anonymous allocation runtime entry")
        member = next(row for row in inventory["classes"] if row["descriptor"] == ANONYMOUS)
        if (member["static_fields_size"] or member["instance_fields_size"] or member["direct_methods_size"] != 1
                or member["virtual_methods_size"] != 4):
            raise RuntimeError("D8 changed the field-free anonymous constructor and original bridge inventory")
        for row in inventory["methods"]:
            if row["identity"] in BRIDGES and row["access_flags"] != 0x1041:
                raise RuntimeError("D8 changed an original anonymous bridge flag")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        annotations = verify.run([verify.dexdump, "-a", source_dex], "original-enclosure-annotations")
        if ("Ldalvik/annotation/EnclosingMethod;" in annotations
                or annotations.count("Ldalvik/annotation/EnclosingClass;") != 1
                or not re.search(r"Ldalvik/annotation/EnclosingClass;[^\n]*Lfixture/Owner;", annotations)):
            raise RuntimeError("D8 lost the original class-only anonymous enclosure")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-allocation-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            prototype = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if not name or not prototype:
                continue
            identity = block[1] + "->" + name[1] + prototype[1]
            ops = [(int(pc, 16), op, tail) for pc, op, tail in
                   re.findall(r"\|([0-9a-fA-F]+):\s+(\S+)([^\n]*)", block[2])]
            if identity in BRIDGES:
                calls = [op for pc, op, tail in ops if op in {"invoke-virtual", "invoke-virtual/range"}
                         and ANONYMOUS + "." + name[1] + ":" in tail]
                if len(calls) != 1 or sum(op == "check-cast" for pc, op, tail in ops) != (1 if name[1] == "get" else 2):
                    raise RuntimeError("D8 changed original typed bridge forwarding and cast positions")
                witnessed.add(identity)
            if identity == "Lfixture/Owner;-><clinit>()V":
                allocate = [pc for pc, op, tail in ops if op == "new-instance" and ANONYMOUS in tail]
                reads = [pc for pc, op, tail in ops if op == "sget-object" and "Lfixture/Names;." in tail]
                calls = [pc for pc, op, tail in ops if op in {"invoke-direct", "invoke-direct/range"} and ANONYMOUS + ".<init>:" in tail]
                guards = [pc for pc, op, tail in ops if op.startswith("if-")]
                if (len(allocate) != 1 or len(calls) != 1 or len(reads) != (2 if args.conditional else 1)
                        or any(not allocate[0] < pc < calls[0] for pc in reads)
                        or bool(guards) != args.conditional):
                    raise RuntimeError("D8 changed original anonymous allocation and argument initialization order")
                witnessed.add(identity)
        if witnessed != set(BRIDGES) | {"Lfixture/Owner;-><clinit>()V"}:
            raise RuntimeError("Missing original anonymous allocation or bridge witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = set(shape["methods"]) | {"Lfixture/Owner;-><clinit>()V"}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage["status"] != "partial" or coverage["projected_method_count"] != 6
                or coverage.get("generic_signature_projections")
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != projected):
            raise RuntimeError("Anonymous allocation changed original inventory or complete generic contracts")
        for identity, target in BRIDGES.items():
            if (rows[identity].get("bridge_target") != target
                    or rows[identity].get("bridge_dispatch") != identity.replace(ANONYMOUS, "Lfixture/GenericBase;")
                    or rows[identity].get("projection_kind") != "generic-superclass-bridge"):
                raise RuntimeError("Anonymous allocation lost an original bridge contract receipt")
        for identity in projected - set(BRIDGES):
            if rows[identity].get("projection_kind") != "anonymous-to-named-member":
                raise RuntimeError("Anonymous allocation lost its member source projection disclosure")
        bindings = coverage.get("class_source_bindings", [])
        if (len(bindings) != 1 or bindings[0].get("class") != ANONYMOUS
                or bindings[0].get("source_enclosing_class") != "Lfixture/Owner;"
                or bindings[0].get("binding_kind") != "anonymous-to-named-member"
                or bindings[0].get("binary_name_status") != "changed" or not bindings[0].get("metadata_differences")):
            raise RuntimeError("Anonymous allocation lacks its exact named-member metadata binding")
        binding = bindings[0]
        renamed = binding["source_enclosing_class"][:-1] + "$" + binding["source_name"] + ";"
        def mapped(value):
            return value.replace(ANONYMOUS, renamed)
        rebuilt = work / "rebuilt"
        release = coverage.get("minimum_java_release", 8)
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=release,
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {mapped(row["class"] + "->" + row["name"] + row["prototype"]) for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {mapped(owner) + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if (actual_classes != {mapped(owner) for owner in classes}
                or set(actual_methods) != {mapped(identity) for identity in methods} | expected_helpers or helpers != expected_helpers):
            raise RuntimeError("Anonymous allocation lost or invented runtime entries beyond its explicit binding")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = copy.deepcopy(records[owner]), actual_records[mapped(owner)]
            before["fields"] = {mapped(identity): metadata for identity, metadata in before["fields"].items()}
            before["methods"] = {mapped(identity): metadata for identity, metadata in before["methods"].items()}
            if owner == ANONYMOUS:
                before["enclosing_method"] = None
                before["inner_class"] = {"inner": renamed, "outer": "Lfixture/Owner;", "name": binding["source_name"], "access": 8}
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original anonymous metadata changed beyond named-member binding: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                original = identity.replace(renamed, ANONYMOUS)
                if after["methods"].get(identity) != metadata or actual_exceptions.get(identity) != exceptions[original]:
                    raise RuntimeError("Original anonymous method metadata/Exceptions changed: " + original)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt", True) != baseline:
            raise RuntimeError("Anonymous allocation changed initialization, argument snapshots, bridges or failures")
        negative = work / "wrong-specialization-originals"
        shutil.copytree(originals, negative)
        receipt = change_signature(negative / "fixture/Owner$1.class", signature.encode(),
                                   signature.replace("Ljava/lang/Float;", "Ljava/lang/Double;").encode())
        (work / "wrong-specialization-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex, rejected = dex(negative, "wrong-specialization-dex"), work / "wrong-specialization-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath),
            "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-specialization-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Anonymous allocation accepted an altered complete superclass specialization")
        mutants = (
            ("wrong-name-branch", "Owner", "Names.first", "Names.second", 1),
            ("missing-postlude", "Owner", "Trace.hit(4);", "", 1),
            ("lost-set-value", "Owner", "owner.value = value;", "owner.value = null;", 1),
            ("late-name-read", "GenericBase", "this.type = type; this.name = name;", "this.type = type;", 1),
            ("changed-failure-identity", "Trace", "throw failure;", "throw new IllegalStateException();", 1),
        )
        for label, name, anchor, replacement, count in mutants:
            original = (fixture / "fixture" / (name + ".java")).read_text()
            if original.count(anchor) != count:
                raise RuntimeError("Owned anonymous allocation control anchor changed")
            changed = original.replace(anchor, replacement)
            if label == "late-name-read":
                if changed.count("Trace.hit(3);") != 1:
                    raise RuntimeError("Owned constructor snapshot control changed")
                changed = changed.replace("Trace.hit(3);", "Trace.hit(3); this.name = Names.first;")
            source = work / label / "fixture" / (name + ".java")
            source.parent.mkdir(parents=True)
            source.write_text(changed)
            compiled = work / (label + "-classes")
            verify.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                "AnonymousMemberAllocationHarness", "1", "1" if label == "changed-failure-identity" else "0",
                "true", str(args.conditional).lower(), "false"], text=True, capture_output=True,
                timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "anonymous member allocation contract" not in result.stderr:
                raise RuntimeError("Frozen observer missed the independent " + label + " control")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 6, "fresh_jvm_cases": 72, "attempts_per_case": 2,
                    "dependency_provider": args.dependency_provider, "conditional": args.conditional,
                    "minimum_java_release": release, "class_initialization_and_argument_snapshot_order": "matched",
                    "typed_erased_frozen_reflection_and_method_handle_dispatch": "matched",
                    "original_metadata_Exceptions_and_inventory": "matched_except_reported_named_member_binding",
                    "same_Code_superclass_specialization_counterexample": "rejected",
                    "independent_counterexamples": len(mutants), "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
