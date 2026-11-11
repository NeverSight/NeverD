#!/usr/bin/env python3
"""Verify scalar interface-super bridges share proven defaults and source names."""
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
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def encode_name(path):
    before = path.read_bytes()
    old, new = b"\x01\x00\x07measure", b"\x01\x00\x07meas-ur"
    if before.count(old) > 1:
        raise RuntimeError("Duplicate owned scalar name constant")
    after = before.replace(old, new)
    shape = class_records(before)
    expected = {k.replace(b"measure(", b"meas-ur("): v for k, v in method_code(before).items()}
    if (method_code(after) != expected or class_records(after)["pool_end"] != shape["pool_end"]
            or before[shape["pool_end"]:] != after[shape["pool_end"]:]):
        raise RuntimeError("Name encoding changed original Code or other attributes")
    path.write_bytes(after)
    return before.count(old)


def encode_bridge(path, overloaded=False):
    descriptors = (b"(F)I", b"(I)F", b"(J)D") if overloaded else (b"(F)I",)
    return [change_method_flags(path, descriptor, 1, 0x41, name=b"meas-ur") for descriptor in descriptors]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-trace", action="store_true")
    parser.add_argument("--baseline-only", action="store_true")
    parser.add_argument("--overloaded", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-interface-super-scalars-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / (("interface_super_scalar_overloads" if args.overloaded else "interface_super_scalars") + "/java")
        harness_name = "OverloadedInterfaceSuperHarness" if args.overloaded else "InterfaceSuperScalarsHarness"
        authored, encoded, harness = (work / n for n in ("authored", "encoded", "harness"))
        verify.compile_local(sorted(fixture.rglob("*.java")), authored, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              (("InterfaceSuperScalarsCaller", "InterfaceSuperScalarsHarness", "OverloadedInterfaceSuperCaller", "OverloadedInterfaceSuperHarness") if args.overloaded else ("InterfaceSuperScalarsCaller", "InterfaceSuperScalarsHarness"))],
                             harness, classpath=authored, release=8)
        authored_hashes, harness_hashes = hashes(authored), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        before = class_identity.compiler_classes(authored)

        def execute(runtime, label, name, bridge):
            results = []
            for route in range(4):
                for mode in range(32):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        harness_name, str(route), str(mode), name, str(bridge).lower()], f"{label}-{route}-{mode}")
                    if len(output.splitlines()) != (6 if args.overloaded else 2) or not all(re.fullmatch(
                            f"route={route},mode={mode},attempt=[01],trace=[0-9]+", line) for line in output.splitlines()):
                        raise RuntimeError("Original scalar dispatch observer changed")
                    results.append(output)
            return results

        baseline = execute((authored, harness), "authored", "measure", False)
        shutil.copytree(authored, encoded)
        names = {p.relative_to(encoded).as_posix(): count for p in sorted(encoded.rglob("*.class"))
                 if (count := encode_name(p))}
        if set(names) != {"fixture/" + n + ".class" for n in ("Root", "Right", "Owner", "Direct", "Child", "Facade")}:
            raise RuntimeError("Original scalar name encoding sites changed")
        flags = {n: encode_bridge(encoded / "fixture" / (n + ".class"), args.overloaded) for n in ("Owner", "Direct")}
        (work / "encoding-witness.json").open("x").write(json.dumps({"names": names, "bridge_flags": flags}, indent=2) + "\n")
        records = class_identity.compiler_classes(encoded)
        for owner, record in before.items():
            wanted = deepcopy(record)
            methods = {}
            for identity, metadata in wanted["methods"].items():
                if metadata["name"] == "measure":
                    metadata["name"] = "meas-ur"
                    if owner in {"Lfixture/Owner;", "Lfixture/Direct;"}:
                        metadata["access"] |= 0x40
                methods[identity.replace("->measure(", "->meas-ur(")] = metadata
            wanted["methods"] = methods
            if any(wanted[key] != records[owner].get(key) for key in wanted if key != "sha256"):
                raise RuntimeError("Encoding changed other original metadata")
        if execute((encoded, harness), "encoded", "meas-ur", True) != baseline:
            raise RuntimeError("Name/flag encoding changed original behavior")
        encoded_hashes, exceptions = hashes(encoded), exception_inventory(encoded)
        all_classes, all_methods = compiler_inventory(verify, encoded)
        if len(all_classes) != 10 or len(all_methods) != (27 if args.overloaded else 14):
            raise RuntimeError("Original scalar bridge inventory changed")
        classes = all_classes - ({"Lfixture/Trace;"} if args.dependency_trace else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        family = {m for m in methods if "->meas-ur(" in m}
        bridges = {"Lfixture/" + n + ";->meas-ur" + descriptor for n in ("Owner", "Direct")
                   for descriptor in (("(F)I", "(I)F", "(J)D") if args.overloaded else ("(F)I",))}
        dependency = work / "owned-trace.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            jar.writestr("fixture/Trace.class", (encoded / "fixture/Trace.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_trace:
            classpath += ["--classpath", dependency]
        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(encoded.rglob("*.class")) if "L" + p.relative_to(encoded).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original scalar bridge DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original scalar entry")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-forwarders")
        witnesses = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            if block[1] not in {"Lfixture/Owner;", "Lfixture/Direct;"} or not re.search(r"(?m)^\s+name\s+: 'meas-ur'", block[2]):
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            target = re.search(r"Lfixture/Api;\.meas-ur:([^\s]+)", block[2])
            descriptor = target[1] if target else ""
            suffix = "-wide" if descriptor == "(J)D" else ""
            if (descriptor not in (("(F)I", "(I)F", "(J)D") if args.overloaded else ("(F)I",))
                    or ops not in [[call, "move-result" + suffix, "return" + suffix]
                                   for call in ("invoke-super", "invoke-super/range")]):
                raise RuntimeError("Original scalar super forwarding witness changed")
            witnesses.add((block[1], descriptor))
        if len(witnesses) != (6 if args.overloaded else 2):
            raise RuntimeError("Missing original scalar interface super call")
        mutants = (("Owner", "int measure(float value) { return Api.super.measure(value);", "int measure(float value) { return 0;", 0),
                   ("Right", "int measure(float value) {\n        Trace.hit(2);", "int measure(float value) {\n        Trace.hit(1);", 0),
                   ("Child", "int measure(float value) { Trace.hit(3);", "int measure(float value) { ", 0),
                   ("Direct", "int measure(float value) { return Api.super.measure(value);", "int measure(float value) { return Api.super.measure(0.0f);", 4))
        if args.overloaded:
            mutants += (("Owner", "float measure(int value) { return Api.super.measure(value);", "float measure(int value) { return (float)Api.super.measure((float)value);", 0),
                        ("Direct", "double measure(long value) { return Api.super.measure(value);", "double measure(long value) { return Api.super.measure((long)(int)value);", 0),
                        ("Root", "return Trace.finishInt(value);", "return (float)value;", 0))
        for index, (name, old, new, mode) in enumerate(mutants):
            content = (fixture / "fixture" / (name + ".java")).read_text()
            if content.count(old) != 1:
                raise RuntimeError("Scalar counterexample source anchor changed")
            changed = work / ("wrong-" + str(index) + "-" + name) / (name + ".java")
            changed.parent.mkdir()
            changed.open("x").write(content.replace(old, new))
            wrong = work / ("wrong-" + str(index) + "-" + name + "-classes")
            verify.compile_local([changed], wrong, classpath=authored, release=8)
            target = wrong / "fixture" / (name + ".class")
            encode_name(target)
            if name in {"Owner", "Direct"}:
                encode_bridge(target, args.overloaded)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, encoded, harness))),
                harness_name, "1", str(mode), "meas-ur", "true"], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / ("wrong-" + str(index) + "-" + name + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned interface super scalar behavior" not in result.stderr:
                raise RuntimeError("Frozen observer missed wrong " + name)
        if not args.baseline_only:
            output = work / "recovered"
            verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
            coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
            rows = {r["identity"]: r for r in coverage["methods"]}
            if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                    or coverage["unrecovered_method_count"] or coverage.get("projected_method_count") != len(family)
                    or coverage["status"] != "partial" or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")):
                raise RuntimeError("Scalar bridge recovery changed original contracts or inventory")
            spelling = rows[next(iter(family))].get("source_name")
            if not spelling or not re.fullmatch(r"__neverdVirtualMethod\d+", spelling):
                raise RuntimeError("Missing complete source name receipt")
            for identity in family:
                row = rows[identity]
                if row.get("source_name") != spelling or set(row.get("source_name_family", [])) != family or row.get("projection_kind") != "virtual-method-identifier":
                    raise RuntimeError("Scalar bridges did not consume one complete naming family")
                if identity in bridges and (row.get("body_projection_kind") != "interface-super-scalar-forwarding-bridge"
                        or row.get("bridge_target") != "Lfixture/Api;->" + identity.split("->", 1)[1] or not row.get("metadata_differences")):
                    raise RuntimeError("Scalar bridge lost its original direct qualifier or flag receipt")
            rebuilt = work / "rebuilt"
            verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                                 classpath=dependency if args.dependency_trace else None, release=coverage.get("minimum_java_release", 8))
            actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
            renamed = {m: m.replace("->meas-ur(", "->" + spelling + "(") for m in family}
            helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
            expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes - {"Lfixture/Left;", "Lfixture/Api;", "Lfixture/Extra;"}}
            if actual_classes != classes or set(actual_methods) != {renamed.get(m, m) for m in methods} | expected_helpers or helpers != expected_helpers:
                raise RuntimeError("Scalar bridge recovery lost or invented entries/helpers")
            after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
            for owner in classes:
                for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method", "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                    if records[owner].get(key) != after[owner].get(key):
                        raise RuntimeError("Original scalar class metadata changed: " + owner + ":" + key)
                for identity, metadata in records[owner]["methods"].items():
                    wanted = deepcopy(metadata)
                    if identity in family:
                        wanted["name"] = spelling
                    if identity in bridges:
                        wanted["access"] &= ~0x40
                    selected = renamed.get(identity, identity)
                    if after[owner]["methods"].get(selected) != wanted or after_exceptions[selected] != exceptions[identity]:
                        raise RuntimeError("Original scalar method metadata or Exceptions changed")
            runtime = (rebuilt, harness, dependency) if args.dependency_trace else (rebuilt, harness)
            if execute(runtime, "rebuilt", spelling, False) != baseline:
                raise RuntimeError("Scalar super dispatch, raw bits, identity, effects or exact failures changed")
        if (hashes(authored) != authored_hashes or hashes(encoded) != encoded_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs or frozen observers changed")
        evidence = {"status": "baseline-only" if args.baseline_only else "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "overloads_per_case": 3 if args.overloaded else 1, "dependency_trace": args.dependency_trace,
                    "independent_wrong_programs": len(mutants), "original_Code_and_other_encoding_attributes": "unchanged",
                    "most_specific_default_original_self_raw_float_bits_and_failures": "matched",
                    "original_metadata": "unchanged except disclosed names and bridge flags"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
