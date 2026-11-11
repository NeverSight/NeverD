#!/usr/bin/env python3
"""Verify plain interface covariance through original class bridges and SDK edges."""
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
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from scripts.test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--interface-fields", action="store_true")
    parser.add_argument("--interface-bridge", action="store_true")
    parser.add_argument("--reabstract-default", action="store_true")
    args = parser.parse_args()
    if args.reabstract_default and not args.interface_bridge:
        parser.error("--reabstract-default requires --interface-bridge")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-plain-interface-covariance-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "plain_interface_covariance/java"
        if args.interface_fields or args.interface_bridge:
            authored = work / "owned-fields-java"
            for source in sorted(fixture.rglob("*.java")):
                text = source.read_text()
                if args.interface_fields and source.name in {"Root.java", "Api.java"}:
                    flags = 11 if source.name == "Root.java" else 23
                    label = "owned-root" if source.name == "Root.java" else "owned-api"
                    anchor = "public interface Root {" if source.name == "Root.java" else "public interface Api extends Root {"
                    if text.count(anchor) != 1:
                        raise RuntimeError("Owned interface constant anchor changed")
                    text = text.replace(anchor, anchor + '\n    int FLAGS = ' + str(flags) + ';\n    String LABEL = "' + label + '";')
                if args.interface_bridge and source.name == "Api.java":
                    anchor = "@Override Root update"
                    if text.count(anchor) != 1:
                        raise RuntimeError("Owned abstract covariance anchor changed")
                    text = text.replace(anchor, "@Override Api update")
                if args.reabstract_default and source.name == "Root.java":
                    anchor = "Root update(int code, Object[] values, long wide, double real);"
                    if text.count(anchor) != 1:
                        raise RuntimeError("Owned root default anchor changed")
                    text = text.replace(anchor, "default Root update(int code, Object[] values, long wide, double real) { Trace.parentDefaultCalls++; return this; }")
                target = authored / source.relative_to(fixture)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text)
            fixture = authored
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("PlainInterfaceCovarianceDerived", "PlainInterfaceCovarianceViaDefault", "PlainInterfaceCovarianceHarness")], harness, classpath=originals, release=8)
        # Inherit the original Worker bridges while independently overriding
        # only their typed targets. Otherwise javac's subclass bridges would
        # hide the exact receiver dispatch this frozen observer must witness.
        edits = [omit_owned_interface_bridge(harness / "PlainInterfaceCovarianceDerived.class", identity)
                 for identity in (b"update(I[Ljava/lang/Object;JD)Lfixture/Root;",
                                  b"update(Ljava/lang/Object;)Lfixture/Root;")]
        if args.interface_bridge:
            edits.append(omit_owned_interface_bridge(harness / "PlainInterfaceCovarianceViaDefault.class",
                b"update(I[Ljava/lang/Object;JD)Lfixture/Root;"))
        (work / "frozen-subclass-bridge-omissions.json").write_text(json.dumps(edits, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 4 or len(all_methods) != 12 + int(args.interface_bridge):
            raise RuntimeError("Owned plain interface covariance inventory changed")
        interface_owners = {"Lfixture/Root;", "Lfixture/Api;"}
        # The abstract source interface retains its original bridge Code;
        # only the plain root declaration is frozen in that profile.
        providers = {"Lfixture/Root;"} if args.interface_bridge else interface_owners
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        if any(record["signature"] for record in records.values()):
            raise RuntimeError("Plain covariance fixture acquired a class Signature")
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        expected_calls = 280 if args.interface_bridge else 180
        expected_failures = 140 if args.interface_bridge else 80
        harness_mode = ("fields" if args.interface_fields else "plain") + ("-interface" if args.interface_bridge else "")
        def execute(runtime, label):
            output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                "PlainInterfaceCovarianceHarness", harness_mode], label)
            if output.splitlines() != [f"calls={expected_calls},failures={expected_failures},original-bridges-and-subclass-dispatch=matched"]:
                raise RuntimeError("Original plain covariance matrix changed: " + output)
            return output

        def make_dex(source, label):
            destination = work / label
            destination.mkdir()
            paths = [p for p in sorted(source.rglob("*.class"))
                     if "L" + p.relative_to(source).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", destination, *paths], "d8-" + label)
            dex = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned plain covariance fixture changed DEX inventory")
            return dex

        baseline = execute((originals, harness), "baseline")
        source_dex = make_dex(originals, "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed original plain covariance identities")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-bridge-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            prototype = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if block[1] not in ({"Lfixture/Worker;", "Lfixture/Api;"} if args.interface_bridge else {"Lfixture/Worker;"}) or not name or name[1] != "update" or not prototype or not prototype[1].endswith("Lfixture/Root;"):
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            invokes = [op for op in ops if op.startswith("invoke-")]
            opcode = "invoke-interface" if block[1] == "Lfixture/Api;" else "invoke-virtual"
            if (len(invokes) != 1 or invokes[0] not in {opcode, opcode + "/range"}
                    or ops[-2:] != ["move-result-object", "return-object"] or "check-cast" in ops
                    or block[1] + ".update:" not in block[2]):
                raise RuntimeError("D8 changed the original forwarding bridge")
            witnessed.add(block[1] + "->update" + prototype[1])
        expected_bridges = {"Lfixture/Worker;->update(Ljava/lang/Object;)Lfixture/Root;",
                            "Lfixture/Worker;->update(I[Ljava/lang/Object;JD)Lfixture/Root;"}
        if args.interface_bridge:
            expected_bridges.add("Lfixture/Api;->update(I[Ljava/lang/Object;JD)Lfixture/Root;")
        if witnessed != expected_bridges:
            raise RuntimeError("Missing original plain covariance bridge witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if (coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or {row["identity"] for row in coverage["methods"]} != methods
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage["projected_method_count"] != 2 + int(args.interface_bridge) or coverage.get("generic_signature_projections")
                or coverage.get("class_source_bindings")):
            raise RuntimeError("Plain covariance recovery lost entries or invented metadata")
        rows = {row["identity"]: row for row in coverage["methods"]}
        for identity in witnessed:
            target = identity[:-len("Lfixture/Root;")] + "Lfixture/Api;"
            if rows[identity].get("bridge_target") != target or not rows[identity].get("projection_kind"):
                raise RuntimeError("Original forwarding bridge lacks its exact target receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8), classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != methods | helpers:
            raise RuntimeError("Plain covariance recovery invented or lost runtime entries")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            expected_fields = json.loads(json.dumps(before["fields"]))
            if args.interface_fields and owner in interface_owners:
                initializer = owner + "-><clinit>()V"
                if (initializer not in helpers or not any(row["class"] == owner and row["name"] == "<clinit>"
                        and row["kind"] == "field-initializer" for row in coverage.get("generated_source_helpers", []))):
                    raise RuntimeError("Source interface constants lack their reported field initializer")
                expected_flags = {"tag": 3, "value": "0000000b" if owner == "Lfixture/Root;" else "00000017"}
                expected_label = {"tag": 8, "value": "owned-root" if owner == "Lfixture/Root;" else "owned-api"}
                for name, descriptor, value in (("FLAGS", "I", expected_flags), ("LABEL", "Ljava/lang/String;", expected_label)):
                    field = expected_fields[owner + "->" + name + ":" + descriptor]
                    if field["constant_value"] != value:
                        raise RuntimeError("Original interface constant value changed")
                    field["constant_value"] = None
            if after["fields"] != expected_fields:
                raise RuntimeError("Interface fields changed beyond reported constant initialization")
            for key in ("access", "signature", "superclass", "interfaces", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original plain covariance metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original covariance method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Original bridge, subclass dispatch, argument bits or failure order changed")
        if args.reabstract_default:
            missing = work / "missing-interface-bridge"
            shutil.copytree(originals, missing)
            edit = omit_owned_interface_bridge(missing / "fixture/Api.class",
                b"update(I[Ljava/lang/Object;JD)Lfixture/Root;")
            (work / "missing-interface-bridge-encoding.json").write_text(json.dumps(edit, indent=2) + "\n")
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (missing, harness))),
                "PlainInterfaceCovarianceHarness", harness_mode], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "missing-interface-bridge-oracle.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "plain interface covariance contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect fallback to the parent default")
            encoded = make_dex(missing, "missing-interface-bridge-dex")
            rejected = work / "missing-interface-bridge-rejected"
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(encoded), *map(str, classpath),
                "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "missing-interface-bridge-recovery.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Reabstracted default admitted a missing original forwarding bridge")
        negative = work / "method-formal-originals"
        shutil.copytree(originals, negative)
        receipt = add_parameter_signature(negative / "fixture/Worker.class",
            "<T:Ljava/lang/Object;>(Ljava/lang/Object;)Lfixture/Api;", method_name="update", prototype="(Ljava/lang/Object;)Lfixture/Api;")
        (work / "method-formal-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        encoded = make_dex(negative, "method-formal-dex")
        rejected = work / "method-formal-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(encoded), *map(str, classpath),
            "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "method-formal-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Plain covariance admitted a changed full method contract")
        wrong = work / "changed-result/fixture/Worker.java"
        wrong.parent.mkdir(parents=True)
        text = (fixture / "fixture/Worker.java").read_text()
        if text.count("return replacement;") != 2:
            raise RuntimeError("Independent covariance counterexample anchor changed")
        wrong.write_text(text.replace("return replacement;", "return this;"))
        compiled = work / "changed-result-classes"
        verify.compile_local([wrong], compiled, classpath=originals, release=8)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                 "PlainInterfaceCovarianceHarness", harness_mode], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "changed-result-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "plain interface covariance contract" not in result.stderr:
            raise RuntimeError("Independent oracle did not detect changed result identity")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "runtime_calls": expected_calls, "expected_failures": expected_failures, "original_bridge_count": 2 + int(args.interface_bridge),
                    "dependency_provider": args.dependency_provider, "coverage": "partial",
                    "interface_fields": args.interface_fields, "abstract_interface_bridge": args.interface_bridge,
                    "reabstracted_parent_default": args.reabstract_default,
                    "parent_default_counterexample": "detected and rejected" if args.reabstract_default else "not applicable",
                    "interface_constant_value_difference": "reported field initializers" if args.interface_fields and bool(classes & interface_owners) else "none",
                    "class_signatures_and_method_metadata_and_exceptions": "unchanged",
                    "original_bridge_dispatch_and_wide_arguments": "matched",
                    "ordered_effects_exceptions_and_result_identity": "matched",
                    "independent_result_counterexample": "detected", "same_code_formal_counterexample": "rejected",
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
