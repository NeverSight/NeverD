#!/usr/bin/env python3
"""Verify original generic scalar-result bridges, boxing factories and interface defaults."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_final_getter_return_slots import rename_owned_entry
    from scripts.test_mobile_android_nested_return import change_signature, method_code, class_records
    from scripts.test_mobile_android_empty_original_bridge_throws import exception_inventory
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_final_getter_return_slots import rename_owned_entry
    from test_mobile_android_nested_return import change_signature, method_code, class_records
    from test_mobile_android_empty_original_bridge_throws import exception_inventory
    from test_mobile_android_protected_array_callbacks import change_method_flags


def hashes(root: Path) -> dict:
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(root.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-root", action="store_true")
    parser.add_argument("--abstract-owner", action="store_true")
    parser.add_argument("--mangled-target", action="store_true")
    parser.add_argument("--deferred-plain-slot", action="store_true")
    args = parser.parse_args()
    if args.mangled_target and (args.abstract_owner or args.deferred_plain_slot):
        parser.error("--mangled-target requires the final concrete owner profile")
    if args.deferred_plain_slot and not args.abstract_owner:
        parser.error("--deferred-plain-slot requires --abstract-owner")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-boxed-generic-interface-results-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture_name = "abstract_boxed_generic_results" if args.abstract_owner else "boxed_generic_interface_results"
        if args.deferred_plain_slot:
            fixture_name = "abstract_boxed_deferred_slots"
        harness_name = "AbstractBoxedGenericResultsHarness" if args.abstract_owner else "BoxedGenericInterfaceResultsHarness"
        fixture = FIXTURES / fixture_name / "java"
        inputs = sorted(fixture.rglob("*.java"))
        observer = FIXTURES / "harness" / (harness_name + ".java")
        source_hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in [*inputs, observer]}
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness" / (harness_name + ".java")], harness,
                             classpath=originals)
        owner = "Lfixture/Core;"
        runtime_method = owner + "->next()Ljava/lang/Object;"
        source_signature = b"Ljava/lang/Object;Lfixture/Api<Ljava/lang/Object;>;Lfixture/Marker;"
        input_signature = b"Ljava/lang/Object;Lfixture/Api<Lfixture/Box;>;Lfixture/Marker;"
        metadata_receipt = change_signature(originals / "fixture/Core.class", source_signature, input_signature)
        flags_receipt = change_method_flags(originals / "fixture/Core.class", b"()Ljava/lang/Object;",
                                           0x1, 0x1041, name=b"next")
        (work / "original-boxed-result-bridge-encoding.json").write_text(
            json.dumps({"metadata": metadata_receipt, "flags": flags_receipt}, indent=2) + "\n")
        typed_method = owner + "->next()Lfixture/Box;"
        if args.abstract_owner:
            receipt = rename_owned_entry(originals / "fixture/Core.class", b"typedNext", b"next")
            (work / "original-final-typed-entry-name.json").write_text(json.dumps(receipt, indent=2) + "\n")
        target_name = "read-owned-result" if args.mangled_target else "readValue"
        target_method = owner + "->" + target_name + "()J"
        if args.mangled_target:
            path = originals / "fixture/Core.class"
            before = path.read_bytes()
            receipt = rename_owned_entry(path, b"readValue", target_name.encode())
            after = path.read_bytes()
            expected_code = {key.replace(b"readValue()J", target_name.encode() + b"()J"): value
                             for key, value in method_code(before).items()}
            if (method_code(after) != expected_code
                    or before[class_records(before)["pool_end"]:] != after[class_records(after)["pool_end"]:]):
                raise RuntimeError("Scalar target spelling changed Code, descriptors, flags or attributes")
            (work / "encoding-witness.json").write_text(json.dumps({"target_name": receipt, "all_original_Code": "unchanged"}, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        if args.mangled_target:
            (work / "frozen-input-hashes.json").write_text(json.dumps({"originals": original_hashes, "harness": harness_hashes, "sources": source_hashes}, indent=2) + "\n")
        cli_hash = hashlib.sha256(args.neverd.read_bytes()).hexdigest()
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Api;"} if args.dependency_root else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        original_exceptions = exception_inventory(originals)
        dependency = work / "owned-root.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("Api",):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_root:
            classpath += ["--classpath", dependency]
        java_profile = ["-Downed.deferred.slot=true"] if args.deferred_plain_slot else []
        baseline = verify.run([verify.java, *java_profile, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_name, "original"], "baseline")
        suffix = "original-scalar-target-and-factory=retained,mutable-factory-results-and-default-callback-order=retained,object-interface-argument-and-bridge-flags=reported"
        if args.abstract_owner:
            suffix = suffix.replace("object-interface-argument-and-bridge-flags=reported",
                                    "object-interface-argument-bridge-flags-and-final-typed-name=reported")
        lines = baseline.splitlines()
        matched = re.fullmatch(r"values=(\d+),identities=(\d+),states=(\d+),failures=(\d+)", lines[0]) if len(lines) == 2 else None
        if not matched or lines[1] != suffix:
            raise RuntimeError("Owned scalar result baseline missed a boxed result, default or failure case: " + baseline)
        counts = tuple(map(int, matched.groups()))
        if counts != ((246, 804, 636, 42) if args.abstract_owner else (186, 610, 484, 34)):
            raise RuntimeError("Owned boxed result baseline changed its independent case matrix: " + baseline)

        if args.mangled_target:
            (work / "baseline.json").write_text(json.dumps({"output": baseline, "counts": counts}, indent=2) + "\n")

        wrong_programs = []
        if args.mangled_target:
            controls = [
                ("Core", "calls++;", "calls += 2;"),
                ("Core", "observed = marker;", "observed = null;"),
                ("Core", "if (failure != null) throw failure;", "if (false) throw failure;"),
                ("Core", "return values[index++];", "return (int) values[index++];"),
                ("Box", "calls++;", "calls += 2;"),
                ("Box", "lastInput = value;", "lastInput = 0L;"),
                ("Box", "if (returnNull) last = null;", "if (returnNull) last = new Box(value);"),
                ("Box", "else if (cached != null) last = cached;", "else if (cached != null) last = new Box(value);"),
            ]
            for index, (name, anchor, replacement) in enumerate(controls):
                source = (fixture / ("fixture/" + name + ".java")).read_text()
                if source.count(anchor) != 1:
                    raise RuntimeError("Owned boxed-target wrong-program anchor changed")
                wrong_source = work / (f"wrong-{index}-source/" + name + ".java")
                wrong_source.parent.mkdir()
                wrong_source.write_text(source.replace(anchor, replacement))
                wrong = work / f"wrong-{index}-classes"
                verify.compile_local([wrong_source], wrong, classpath=originals)
                if name == "Core":
                    path = wrong / "fixture/Core.class"
                    change_signature(path, source_signature, input_signature)
                    change_method_flags(path, b"()Ljava/lang/Object;", 0x1, 0x1041, name=b"next")
                    rename_owned_entry(path, b"readValue", target_name.encode())
                rejected = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness))),
                                           harness_name, "original"], text=True, capture_output=True,
                                          timeout=300, env=verify.environment)
                (work / f"wrong-{index}-counterexample.log").write_text(rejected.stdout + rejected.stderr)
                if rejected.returncode == 0 or "AssertionError" not in rejected.stderr:
                    raise RuntimeError("Independent boxed result observer missed wrong program " + str(index))
                wrong_programs.append(name + ":" + anchor)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if not result.is_file() or list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned boxed generic interface result fixture did not produce one DEX")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the original boxed generic interface result runtime inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-boxed-result-bridge-disassembly")
        witnesses = {}
        blocks = re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly)
        for block in blocks:
            declaration = block[2]
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", declaration)
            prototype = re.search(r"(?m)^\s+type\s+: '([^']+)'", declaration)
            flags = re.search(r"(?m)^\s+access\s+: 0x([0-9a-fA-F]+)", declaration)
            if not name or not prototype or not prototype[1].startswith("("):
                continue
            identity = block[1] + "->" + name[1] + prototype[1]
            if args.mangled_target and identity == target_method:
                opcodes = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", declaration)
                if (identity in witnesses or not flags or int(flags[1], 16) != 1
                        or "return-wide" not in opcodes or "aget-wide" not in opcodes
                        or "iput" not in opcodes or "throw" not in opcodes):
                    raise RuntimeError("Original effectful scalar provider witness changed")
                witnesses[identity] = opcodes
                continue
            if identity not in ({runtime_method, typed_method} if args.abstract_owner else {runtime_method}):
                continue
            opcodes = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", declaration)
            if (identity in witnesses or not flags or int(flags[1], 16) != (0x11 if identity == typed_method else 0x1041)
                    or len(opcodes) != 5 or opcodes[0] not in ("invoke-virtual", "invoke-virtual/range")
                    or opcodes[1] != "move-result-wide" or opcodes[2] not in ("invoke-static", "invoke-static/range")
                    or opcodes[3:] != ["move-result-object", "return-object"]
                    or ("Lfixture/Core;." + target_name + ":()J") not in declaration
                    or "Lfixture/Box;.box:(J)Lfixture/Box;" not in declaration):
                raise RuntimeError("D8 changed the original scalar getter, wide factory call or five-op result bridge")
            witnesses[identity] = opcodes
        if set(witnesses) != ({runtime_method} | ({typed_method} if args.abstract_owner else set()) | ({target_method} if args.mangled_target else set())):
            raise RuntimeError("Missing complete original scalar result and boxing opcode witness")
        (work / "original-boxed-result-bridge-opcodes.json").write_text(json.dumps(witnesses, indent=2) + "\n")
        if args.mangled_target:
            (work / "input-method-witness.json").write_text(json.dumps({"dex_sha256": hashlib.sha256(source_dex.read_bytes()).hexdigest(), "opcodes": witnesses}, indent=2) + "\n")

        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projections = coverage.get("generic_signature_projections", [])
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != (1 + int(args.abstract_owner) + int(args.mangled_target)) or coverage["status"] != "partial"
                or len(projections) != 1 or coverage.get("class_source_bindings")
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != ({runtime_method} | ({typed_method} if args.abstract_owner else set()) | ({target_method} if args.mangled_target else set()))):
            raise RuntimeError("Checked boxed generic interface result changed original inventory or projection receipt")
        projection = projections[0]
        changes = projection.get("changes", [])
        if (projection.get("declaration") != owner or projection.get("original_signature") != input_signature.decode()
                or len(changes) != 1 or changes[0].get("kind") != "boxed-interface-result-argument"
                or changes[0].get("source_type") != "Lfixture/Api<Ljava/lang/Object;>;"
                or changes[0].get("scope_origin") != runtime_method
                or rows[runtime_method].get("projection_kind") != "boxed-generic-interface-result-bridge"
                or rows[runtime_method].get("inherited_class_contract") != "Lfixture/Api;->next()Ljava/lang/Object;"
                or rows[runtime_method].get("source_identity") != runtime_method
                or rows[runtime_method].get("bridge_target") != target_method
                or rows[runtime_method].get("box_method") != "Lfixture/Box;->box(J)Lfixture/Box;"
                or not rows[runtime_method].get("metadata_differences")):
            raise RuntimeError("Boxed generic result lost its complete original edge, scalar target or factory")
        alias = None
        if args.abstract_owner:
            typed = rows[typed_method]
            alias = typed.get("source_name")
            if (typed.get("projection_kind") != "incompatible-inherited-return-slot" or not alias
                    or typed.get("source_name_declaration") != typed_method
                    or typed.get("conflicting_inherited_contracts") != ["Lfixture/Api;->next()Ljava/lang/Object;"]
                    or typed.get("source_identity") != owner + "->" + alias + "()Lfixture/Box;"
                    or not typed.get("metadata_differences")):
                raise RuntimeError("Original final typed entry lacks its exact body/name receipt")
        if args.mangled_target:
            named = rows[target_method]
            if (named.get("projection_kind") != "virtual-method-identifier" or not named.get("source_name")
                    or named.get("source_name_declaration") != target_method
                    or named.get("source_name_family") != [target_method]
                    or named.get("source_identity") != owner + "->" + named["source_name"] + "()J"
                    or len(named.get("metadata_differences", [])) != 3
                    or named["metadata_differences"][2] != "method access flags, descriptors, Signatures and bodies remain unchanged; retained compiler annotations still describe original runtime names"):
                raise RuntimeError("Boxed result target lost its exact independent source-name receipt")
            (work / "disclosed-metadata-differences.json").write_text(json.dumps({"scope": projection, "target": named, "bridge": rows[runtime_method]}, indent=2) + "\n")
        source_ids = {identity: row.get("source_identity", identity) for identity, row in rows.items()}
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_root else None)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(source_ids.values()) | helpers:
            raise RuntimeError("Recovered boxed generic interface result invented a bridge or runtime entry")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                wanted = source_signature.decode() if identity == owner and key == "signature" else before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Original boxed generic interface result class metadata changed beyond its reported Object argument: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = dict(metadata)
                if method == runtime_method:
                    if wanted["access"] != 0x1041:
                        raise RuntimeError("Original boxed generic interface result lost bridge/synthetic flags")
                    wanted["access"] = 0x1
                if args.abstract_owner and method == typed_method:
                    wanted["name"] = alias
                if args.mangled_target and method == target_method:
                    wanted["name"] = rows[target_method]["source_name"]
                if after["methods"][source_ids[method]] != wanted:
                    raise RuntimeError("Original boxed generic interface result method metadata changed beyond reported flags: " + method)
        if args.mangled_target:
            expected_helpers = {identity + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                                for identity in classes if any(m["code"] for m in records[identity]["methods"].values())}
            if helpers != expected_helpers:
                raise RuntimeError("Boxed target changed the exact generated helper inventory")
            rebuilt_exceptions = exception_inventory(rebuilt)
            for identity in methods:
                if rebuilt_exceptions[source_ids[identity]] != original_exceptions[identity]:
                    raise RuntimeError("Boxed scalar target changed original Exceptions")
            for identity in helpers:
                helper = actual_records[identity.split("->", 1)[0]]["methods"][identity]
                if (helper["access"] != 0xA or not helper["code"]
                        or helper["signature"] != "<E:Ljava/lang/Throwable;>(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;^TE;"):
                    raise RuntimeError("Boxed scalar target changed a reported exception helper")
        rebuilt_path = (rebuilt, harness, dependency) if args.dependency_root else (rebuilt, harness)
        actual = verify.run([verify.java, *java_profile, *(["-Downed.typed.name=" + alias] if alias else []), "-cp", os.pathsep.join(map(str, rebuilt_path)),
                             harness_name, "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Direct, interface, reflected or SDK inlined bridge execution changed")

        negatives = ["complete-argument-drift", "missing-synthetic-flag"]
        if args.abstract_owner:
            negatives.append("missing-typed-final-flag")
        if args.deferred_plain_slot:
            negatives.append("private-child-slot")
        for label in negatives:
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "complete-argument-drift":
                receipt = change_signature(negative / "fixture/Core.class", input_signature,
                    b"Ljava/lang/Object;Lfixture/Api<Ljava/lang/String;>;Lfixture/Marker;")
            elif label == "private-child-slot":
                receipt = change_method_flags(negative / "fixture/Child.class", b"()Z",
                                              0x1, 0x2, name=b"hasNext")
            elif label == "missing-typed-final-flag":
                receipt = change_method_flags(negative / "fixture/Core.class", b"()Lfixture/Box;",
                                              0x11, 0x1, name=b"next")
            else:
                receipt = change_method_flags(negative / "fixture/Core.class", b"()Ljava/lang/Object;",
                                              0x1041, 0x41, name=b"next")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Boxed generic interface result admitted changed complete metadata or original flags: " + label)
        if args.abstract_owner:
            wrong_java = work / "wrong-result-java/fixture/Wrong.java"
            wrong_java.parent.mkdir(parents=True)
            wrong_java.write_text(
                "package fixture; public abstract class Wrong implements Api<String> {\n"
                "  public Box next() { return Box.box(0L); }\n}\n")
            rejected = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-classpath",
                                       str(originals), "-d", str(work / "wrong-result-compiled"), str(wrong_java)],
                                      text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "wrong-result-javac.log").write_text(rejected.stdout + rejected.stderr)
            if rejected.returncode == 0:
                raise RuntimeError("Independent javac accepted a conflicting complete boxed result")
        if args.deferred_plain_slot:
            wrong_child = work / "wrong-child-java/fixture/WrongChild.java"
            wrong_child.parent.mkdir(parents=True)
            wrong_child.write_text(
                "package fixture; public final class WrongChild extends Core {\n"
                "  public WrongChild() { super(new long[0]); }\n"
                "  public long readValue() { return 0L; }\n"
                "  private boolean hasNext() { return true; }\n}\n")
            rejected = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-classpath",
                                       str(originals), "-d", str(work / "wrong-child-compiled"), str(wrong_child)],
                                      text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "wrong-child-javac.log").write_text(rejected.stdout + rejected.stderr)
            if rejected.returncode == 0:
                raise RuntimeError("Independent javac accepted a private concrete deferred provider")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in [*inputs, observer]} != source_hashes
                or hashlib.sha256(args.neverd.read_bytes()).hexdigest() != cli_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "projected_methods": 1 + int(args.abstract_owner) + int(args.mangled_target), "value_cases": counts[0], "identity_cases": counts[1],
                    "state_cases": counts[2], "failure_cases": counts[3],
                    "profile": "dependency-root" if args.dependency_root else "source-root",
                    "owner": "abstract with original final typed entry" if args.abstract_owner else "final",
                    "deferred_plain_slot": "original child implementation without parent declaration; same-Code private provider and independent javac private provider rejected" if args.deferred_plain_slot else "not applicable",
                    "original_scalar_target_and_factory": "retained without a new typed runtime entry",
                    "factory_mutable_results_and_inherited_default_callback_order": "matched",
                    "complete_original_metadata_and_runtime_inventory": "unchanged except reported Object interface argument, flags, final typed source name and helpers"
                    if args.abstract_owner else "unchanged except reported Object interface argument, flags and helpers",
                    "same_code_and_descriptor_counterexamples": "complete argument drift, missing original synthetic flag"
                    + (" and missing typed final flag rejected; conflicting complete result independently rejected by javac" if args.abstract_owner else " rejected"),
                    "original_and_immutable_harness_hashes": original_hashes,
                    "harness_hashes": harness_hashes, "mangled_target": args.mangled_target,
                    "independent_wrong_programs": len(wrong_programs),
                    "target_name_and_original_Exceptions": "checked" if args.mangled_target else "not applicable"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
