#!/usr/bin/env python3
"""Verify closed anonymous guards, captured host identity and original scalar superclass calls."""
from __future__ import annotations

import argparse
import copy
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def hashes(root):
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


def encode_final_anonymous(root):
    edits = []
    for relative in ("fixture/Host$1.class", "fixture/Host.class"):
        path = root / relative
        before = path.read_bytes()
        shape = class_records(before)
        data = bytearray(before)
        if relative.endswith("Host$1.class"):
            offset = shape["pool_end"]
            flags = int.from_bytes(before[offset:offset + 2], "big")
            assert flags == 0x20
            data[offset:offset + 2] = (flags | 0x10).to_bytes(2, "big")
        found = 0
        for name, start, stop in shape["rows"]:
            if name != b"InnerClasses":
                continue
            count = int.from_bytes(before[start + 6:start + 8], "big")
            assert stop == start + 8 + count * 8
            for i in range(count):
                at = start + 8 + i * 8
                inner = int.from_bytes(before[at:at + 2], "big")
                if shape["strings"][shape["classes"][inner]] != b"fixture/Host$1":
                    continue
                flags = int.from_bytes(before[at + 6:at + 8], "big")
                assert flags == 0
                data[at + 6:at + 8] = (flags | 0x10).to_bytes(2, "big")
                found += 1
        assert found == 1 and method_code(before) == method_code(bytes(data))
        path.write_bytes(data)
        edits.append({"class": relative, "final_class_and_inner_flags": "encoded",
                      "all_Code_Signatures_and_descriptors": "unchanged"})
    return edits


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-root", action="store_true")
    parser.add_argument("--nullable", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-anonymous-guarded-super-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = FIXTURES / "anonymous_guarded_super_adapters/java"
        harness_source = FIXTURES / "harness/AnonymousGuardedSuperAdaptersHarness.java"
        if args.nullable:
            authored = work / "nullable-java"
            shutil.copytree(fixture, authored)
            host = authored / "fixture/Host.java"
            text = host.read_text()
            original = "if (!(value instanceof String))"
            assert text.count(original) == 2
            text = text.replace(original, "boolean accepted; if (value == null) accepted = true; else accepted = value instanceof String; if (!accepted)")
            host.write_text(text)
            fixture = authored
            harness_source = work / "nullable-harness/AnonymousGuardedSuperAdaptersHarness.java"
            harness_source.parent.mkdir()
            text = (FIXTURES / "harness/AnonymousGuardedSuperAdaptersHarness.java").read_text()
            changes = {
                "new Object[]{null, new Object(), Integer.valueOf(9)}": "new Object[]{new Object(), Integer.valueOf(9)}",
                "new String[]{first, second, first}": "new String[]{null, first, second, first}",
                "value(-1, receiver.index(null)); check(captured, receiver, marker, 1, 1, 0);":
                    "value(7, receiver.index(null)); check(captured, receiver, marker, 2, 1, 1);",
            }
            for before, after in changes.items():
                assert text.count(before) == 1
                text = text.replace(before, after)
            harness_source.write_text(text)
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        local = originals / "fixture/Host$1.class"
        final_receipts = encode_final_anonymous(originals)
        receipts = [change_method_flags(local, descriptor, 0x11, 0x51, name=name)
                    for descriptor, name in ((b"(Ljava/lang/Object;)Z", b"contains"),
                                             (b"(Ljava/lang/Object;)I", b"index"))]
        receipts += [change_method_flags(local, descriptor, 0x1, 0x41, name=name)
                     for descriptor, name in ((b"(Ljava/lang/String;)Z", b"contains"), (b"(Ljava/lang/String;)I", b"index"))]
        (work / "original-bridge-flags.json").write_text(json.dumps(final_receipts + receipts, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Api;"} if args.dependency_root else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records = class_identity.compiler_classes(originals)
        verify.compile_local([harness_source], harness,
                             classpath=originals, release=11)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "AnonymousGuardedSuperAdaptersHarness", "original"], "baseline")
        if baseline.splitlines() != ["values=102,identities=436,states=490,failures=20",
                "closed-anonymous-scope=retained,captured-host-aliases-and-super-dispatch=original,bridge-and-local-metadata=reported"]:
            raise RuntimeError("Independent local nominal guard oracle missed authored outcomes: " + baseline)
        (work / "baseline.txt").write_text(baseline)
        dependency = work / "api.jar"
        with zipfile.ZipFile(dependency, "x") as archive:
            archive.writestr("fixture/Api.class", (originals / "fixture/Api.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()] + (["--classpath", dependency] if args.dependency_root else [])

        def dex(root, destination):
            destination.mkdir()
            paths = [p for p in sorted(root.rglob("*.class"))
                     if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned local guard fixture did not produce one DEX")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes
                or {r["identity"] for r in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed original local guard runtime inventory")
        (work / "inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage.get("generic_signature_projections") or coverage.get("projected_method_count") != 7):
            raise RuntimeError("Local guard recovery changed original input inventory")
        owner = "Lfixture/Host$1;"
        projected = set()
        for name, result in (("contains", "Z"), ("index", "I")):
            outer = owner + "->" + name + "(Ljava/lang/Object;)" + result
            typed = owner + "->" + name + "(Ljava/lang/String;)" + result
            inherited = "Lfixture/Api;->" + name + "(Ljava/lang/Object;)" + result
            for identity, kind, target in ((outer, "guarded-class-forwarding-bridge", typed),
                                           (typed, "scalar-superclass-forwarding-bridge", inherited)):
                projected.add(identity)
                row = rows[identity]
                if (row.get("projection_kind") != kind or row.get("bridge_target") != target
                        or row.get("source_identity") != identity or not row.get("metadata_differences")):
                    raise RuntimeError("Anonymous bridge lost its complete original dispatch proof")
            if rows[outer].get("inherited_class_contract") != inherited:
                raise RuntimeError("Anonymous guard lost its original class contract")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_root else None,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        bindings = coverage.get("class_source_bindings", [])
        if (len(bindings) != 1 or bindings[0].get("class") != owner
                or bindings[0].get("binding_kind") != "anonymous-to-named-local"
                or not bindings[0].get("metadata_differences")):
            raise RuntimeError("Anonymous local guard lost exact ownership/metadata disclosure")
        binding = bindings[0]
        actual_records = class_identity.compiler_classes(rebuilt)
        candidates = [name for name, row in actual_records.items() if row["inner_class"]
                      and row["inner_class"]["name"] == binding["source_name"]
                      and row["enclosing_method"] == records[owner]["enclosing_method"]]
        if len(candidates) != 1:
            raise RuntimeError("Rebuilt local class has no unique reported source binding")
        renamed = candidates[0]
        original_constructor = owner + "-><init>(Lfixture/Host;)V"
        source_constructor = renamed + "-><init>(Lfixture/Host;Lfixture/Host;)V"
        if not any("constructor parameters may differ" in text for text in binding["metadata_differences"]):
            raise RuntimeError("Missing enclosing-instance constructor metadata disclosure")
        def rename(text):
            return source_constructor if text == original_constructor else text.replace(owner, renamed)
        helpers = {rename(r["class"] + "->" + r["name"] + r["prototype"])
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != {rename(c) for c in classes} or set(actual_methods) != {rename(m) for m in methods} | helpers:
            raise RuntimeError("Rebuilt local guard lost or invented an unreported runtime slot")
        for name in classes:
            expected = copy.deepcopy(records[name])
            expected["fields"] = {rename(k): v for k, v in expected["fields"].items()}
            expected["methods"] = {rename(k): v for k, v in expected["methods"].items()}
            if name == owner:
                expected["inner_class"] = {"inner": renamed, "outer": None,
                                           "name": binding["source_name"], "access": 0x10}
                expected["fields"][rename(owner + "->this$0:Lfixture/Host;")]["access"] &= ~0x1000
                expected["methods"][source_constructor]["prototype"] = "(Lfixture/Host;Lfixture/Host;)V"
            actual = actual_records[rename(name)]
            for key in ("access", "signature", "superclass", "interfaces", "inner_class", "enclosing_method", "fields"):
                if expected[key] != actual[key]:
                    raise RuntimeError("Original local guard class metadata changed: " + name + ":" + key)
            for identity, method in expected["methods"].items():
                expected_access = method["access"]
                if identity in {rename(m) for m in projected}:
                    expected_access &= ~0x40
                if actual["methods"][identity]["access"] != expected_access:
                    raise RuntimeError("Original method flags changed beyond reported bridge flag: " + identity)
                if identity == rename(owner + "-><init>(Lfixture/Host;)V") and method["signature"] is not None:
                    receipt = rows[owner + "-><init>(Lfixture/Host;)V"]
                    if (method["signature"] != "()V" or receipt.get("generic_signature") != "()V"
                            or receipt.get("generic_signature_omitted_parameters") != 1
                            or not any("constructor" in text for text in binding["metadata_differences"])):
                        raise RuntimeError("Missing exact implicit host capture constructor receipt")
                    method["signature"] = None
                for key in ("prototype", "signature", "code", "deprecated_attribute", "runtime_visible_annotations"):
                    if method[key] != actual["methods"][identity][key]:
                        raise RuntimeError("Original local method metadata changed: " + identity + ":" + key)
        runtime_classpath = [rebuilt, harness] + ([dependency] if args.dependency_root else [])
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime_classpath)),
                            "AnonymousGuardedSuperAdaptersHarness", "rebuilt"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Anonymous captured host, superclass dispatch, aliases or failures changed")
        for label in ("missing-final", "extra-adapter-effect"):
            negative = work / (label + "-classes")
            if label == "missing-final":
                shutil.copytree(originals, negative)
                receipt = change_method_flags(negative / "fixture/Host$1.class", b"(Ljava/lang/Object;)Z", 0x51, 0x41, name=b"contains")
            else:
                bad_source = work / "extra-effect-java"
                shutil.copytree(fixture, bad_source)
                host_source = bad_source / "fixture/Host.java"
                text = host_source.read_text()
                original = "return super.contains(value);"
                assert text.count(original) == 1
                host_source.write_text(text.replace(original, "Host.this.calls++; return super.contains(value);"))
                verify.compile_local(sorted(bad_source.rglob("*.java")), negative)
                receipt = encode_final_anonymous(negative)
                for name, result in ((b"contains", b"Z"), (b"index", b"I")):
                    for parameter, before, after in ((b"Ljava/lang/Object;", 0x11, 0x51), (b"Ljava/lang/String;", 0x1, 0x41)):
                        receipt.append(change_method_flags(negative / "fixture/Host$1.class", b"(" + parameter + b")" + result, before, after, name=name))
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "-o", str(rejected)],
                                    capture_output=True, text=True, env=verify.environment, timeout=300)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Anonymous guard accepted an unproved original body: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original local input, harness or interface JAR changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_profile": args.dependency_root, "nullable": args.nullable, "jvm_output": baseline.splitlines(),
                    "counterexamples": ["same-Code missing-final", "extra adapter effect"],
                    "source_metadata_differences": "named local class, four bridge flags, capture field synthetic flag and one implicit enclosing constructor parameter",
                    "original_input_harness_dependency_and_sdk_bytes": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
