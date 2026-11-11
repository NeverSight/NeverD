#!/usr/bin/env python3
"""Verify anonymous nominal covariance, captured owners and forwarded constructors."""
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
    from scripts.test_mobile_android_inherited_erased_parameters import add_parameter_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_inherited_erased_parameters import add_parameter_signature


ANONYMOUS = "Lfixture/Owner$1;"
BRIDGE = ANONYMOUS + "->getItem()Lfixture/Menu;"
TARGET = ANONYMOUS + "->getItem()Lfixture/Owner;"
CONSTRUCTOR = ANONYMOUS + "-><init>(Lfixture/Owner;Ljava/lang/Object;)V"
PROVIDERS = {"Lfixture/Base;", "Lfixture/Menu;"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-anonymous-covariance-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "anonymous_covariant_bridges/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("AnonymousCovariantCaller", "AnonymousCovariantHarness")], harness,
                             classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 6 or len(all_methods) != 12:
            raise RuntimeError("Owned anonymous covariance inventory changed: " + str((len(all_classes), len(all_methods))))
        classes = all_classes - (PROVIDERS if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        if (any(row["signature"] for row in records.values())
                or records["Lfixture/Owner;"]["fields"]["Lfixture/Owner;->witness:Ljava/lang/Class;"]["signature"] != "Ljava/lang/Class<*>;"
                or CONSTRUCTOR not in methods):
            raise RuntimeError("Owned anonymous covariance lost its raw class scope or generic witness")
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for owner in sorted(PROVIDERS):
                relative = owner[1:-1] + ".class"
                archive.writestr(relative, (originals / relative).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            outputs = []
            for route in range(6):
                for mode in range(4):
                    for absent in range(2):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "AnonymousCovariantHarness", str(route), str(mode), str(absent)],
                            f"{label}-{route}-{mode}-{absent}")
                        trace = 1 if mode == 1 else 122 if mode == 2 else 1223333
                        if output.splitlines() != [f"route={route},mode={mode},absent={absent},trace={trace}"]:
                            raise RuntimeError("Original anonymous covariance oracle changed: " + output)
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
                raise RuntimeError("Owned anonymous covariance changed DEX inventory")
            return result

        baseline = execute((originals, harness), "baseline")
        source_dex = dex(originals, "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed original anonymous covariance entries")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-bridge-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            prototype = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if block[1] != ANONYMOUS or not name or not prototype:
                continue
            identity = block[1] + "->" + name[1] + prototype[1]
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            if identity == BRIDGE:
                if (ops not in (["invoke-virtual", "move-result-object", "return-object"],
                                ["invoke-virtual/range", "move-result-object", "return-object"])
                        or ANONYMOUS + ".getItem:" not in block[2]):
                    raise RuntimeError("D8 changed original anonymous covariance bridge body")
                witnessed.add(identity)
            elif identity == CONSTRUCTOR:
                if (ops not in (["iput-object", "invoke-direct", "return-void"],
                                ["iput-object", "invoke-direct/range", "return-void"])
                        or ANONYMOUS + ".this$0:" not in block[2]
                        or "Lfixture/Base;.<init>:" not in block[2]):
                    raise RuntimeError("D8 changed captured owner and forwarded superclass constructor")
                witnessed.add(identity)
        if witnessed != {BRIDGE, CONSTRUCTOR}:
            raise RuntimeError("Missing original anonymous bridge or constructor witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {identity for identity in methods if identity.startswith(ANONYMOUS + "->")}
        projected.add("Lfixture/Owner;->create(Ljava/lang/Object;)Lfixture/Base;")
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage["projected_method_count"] != len(projected) or coverage["status"] != "partial"
                or coverage.get("minimum_java_release") != 25 or coverage.get("generic_signature_projections")
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != projected):
            raise RuntimeError("Anonymous covariance recovery lost original entries or full contracts")
        if (rows[BRIDGE].get("bridge_target") != TARGET
                or rows[BRIDGE].get("projection_kind") != "generic-superclass-bridge"
                or rows[BRIDGE].get("bridge_dispatch") != "Lfixture/Base;->getItem()Lfixture/Menu;"):
            raise RuntimeError("Original anonymous bridge lost its exact target and inherited contract")
        for identity in projected - {BRIDGE}:
            if rows[identity].get("projection_kind") != "anonymous-to-named-local":
                raise RuntimeError("Anonymous method lost its source ownership disclosure")
        bindings = coverage.get("class_source_bindings", [])
        if (len(bindings) != 1 or bindings[0].get("class") != ANONYMOUS
                or bindings[0].get("binding_kind") != "anonymous-to-named-local"
                or bindings[0].get("binary_name_status") != "changed"
                or not any("constructor parameters may differ" in item for item in bindings[0].get("metadata_differences", []))):
            raise RuntimeError("Anonymous source binding lacks its explicit capture metadata receipt")
        binding = bindings[0]
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=25,
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        candidates = [owner for owner, metadata in actual_records.items()
                      if (metadata.get("inner_class") or {}).get("name") == binding["source_name"]
                      and metadata["enclosing_method"] == records[ANONYMOUS]["enclosing_method"]]
        if len(candidates) != 1:
            raise RuntimeError("Reported anonymous binding does not identify one independently rebuilt class")
        renamed = candidates[0]

        def mapped(value):
            return value.replace(ANONYMOUS, renamed)

        source_ids = {identity: mapped(identity) for identity in methods}
        hidden_outer = mapped(CONSTRUCTOR) not in actual_methods
        if hidden_outer:
            source_ids[CONSTRUCTOR] = mapped(CONSTRUCTOR).replace("-><init>(", "-><init>(Lfixture/Owner;", 1)
        helpers = {mapped(row["class"] + "->" + row["name"] + row["prototype"])
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {mapped(owner) + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes - {"Lfixture/Menu;"}}
        if (actual_classes != {mapped(owner) for owner in classes}
                or set(actual_methods) != set(source_ids.values()) | expected_helpers or helpers != expected_helpers):
            raise RuntimeError("Anonymous covariance changed inventory beyond reported captures and helpers")
        for owner in classes:
            before, after = records[owner], actual_records[mapped(owner)]
            for key in ("access", "signature", "superclass", "interfaces", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original anonymous class metadata changed: " + owner + ":" + key)
            inner = copy.deepcopy(before["inner_class"])
            fields = {mapped(identity): dict(metadata) for identity, metadata in before["fields"].items()}
            if owner == ANONYMOUS:
                inner["inner"], inner["name"] = renamed, binding["source_name"]
                captured = fields[mapped(ANONYMOUS + "->this$0:Lfixture/Owner;")]
                if captured["access"] != 0x1010:
                    raise RuntimeError("Original anonymous outer capture flags changed")
                captured["access"] &= ~0x1000
            if after["inner_class"] != inner:
                raise RuntimeError("Anonymous lexical metadata changed beyond the reported local name")
            actual_fields = dict(after["fields"])
            extra = set(actual_fields) - set(fields)
            if extra:
                if owner != ANONYMOUS or len(extra) != 1 or not hidden_outer:
                    raise RuntimeError("Unreported anonymous source field appeared")
                capture = actual_fields.pop(next(iter(extra)))
                if capture["descriptor"] != "Lfixture/Owner;" or capture["access"] != 0x1010 or capture["signature"]:
                    raise RuntimeError("Additional local capture is not its reported implicit enclosing instance")
            if actual_fields != fields:
                raise RuntimeError("Original capture field metadata changed beyond the disclosed synthetic flag")
            for identity, metadata in before["methods"].items():
                expected = dict(metadata)
                if identity == CONSTRUCTOR and hidden_outer:
                    expected["prototype"] = "(Lfixture/Owner;" + expected["prototype"][1:]
                if after["methods"][source_ids[identity]] != expected or actual_exceptions[source_ids[identity]] != exceptions[identity]:
                    raise RuntimeError("Original method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Anonymous bridge changed captures, constructor arguments, callback order or failures")

        negative = work / "method-formal-originals"
        shutil.copytree(originals, negative)
        receipt = add_parameter_signature(negative / "fixture/Owner$1.class",
            "<T:Ljava/lang/Object;>()Lfixture/Owner;", method_name="getItem", prototype="()Lfixture/Owner;")
        (work / "method-formal-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        encoded, rejected = dex(negative, "method-formal-dex"), work / "method-formal-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(encoded), *map(str, classpath),
                                 "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "method-formal-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Anonymous covariance admitted a changed original complete contract")
        mutants = (
            ("wrong-owner", "Owner", "return Owner.this;", "return new Owner();", 0),
            ("lost-forwarded-token", "Owner", "new Base(token)", "new Base(null)", 0),
            ("missing-constructor-effect", "Base", "Trace.hit(2);", "", 0),
            ("changed-failure-identity", "Trace", "throw failure;", "throw new IllegalStateException();", 3),
        )
        for label, name, anchor, replacement, mode in mutants:
            original = (fixture / "fixture" / (name + ".java")).read_text()
            if original.count(anchor) != 1:
                raise RuntimeError("Owned anonymous covariance control anchor changed")
            source = work / label / "fixture" / (name + ".java")
            source.parent.mkdir(parents=True)
            source.write_text(original.replace(anchor, replacement, 1))
            compiled = work / (label + "-classes")
            verify.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                "AnonymousCovariantHarness", "1", str(mode), "0"], text=True, capture_output=True,
                timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "anonymous covariance contract" not in result.stderr:
                raise RuntimeError("Frozen observer missed the independent " + label + " control")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "coverage": "partial", "class_count": len(classes),
                    "method_count": len(methods), "projected_methods": len(projected),
                    "fresh_jvm_cases": 48, "dependency_provider": args.dependency_provider,
                    "captured_owner_and_forwarded_constructor_identity": "matched",
                    "class_initialization_callbacks_and_first_repeated_failures": "matched",
                    "typed_bridge_frozen_reflection_and_method_handle_dispatch": "matched",
                    "original_metadata_Exceptions_and_inventories": "matched_except_reported_local_capture",
                    "same_code_constraint_counterexample": "rejected", "independent_counterexamples": len(mutants),
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
