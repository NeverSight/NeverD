#!/usr/bin/env python3
"""Verify fixed nominal interface-super bridges preserve original forwarding."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_static_defaults import method_code

DESCRIPTOR = "(Lfixture/Input;)Lfixture/Output;"
BRIDGE = "Lfixture/Owner;->convert" + DESCRIPTOR
TARGET = "Lfixture/Api;->convert" + DESCRIPTOR
compiler_facts = class_identity.compiler_classes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-trace", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nominal-interface-super-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "nominal_interface_super_forwarding/java/fixture"
        authored, originals, harness = (work / name for name in ("authored", "originals", "harness"))
        verify.compile_local(sorted(fixture.glob("*.java")), authored, release=8)
        names = ("NominalInterfaceSuperCaller", "NominalInterfaceSuperHarness")
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in names], harness, classpath=authored, release=8)
        authored_hashes, harness_hashes = hashes(authored), hashes(harness)
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        sdk_hash, cli_hash = sha(args.android_jar), sha(args.neverd)

        def execute(runtime, label, bridge):
            results = []
            for route in range(7):
                for mode in range(12):
                    for future in (False, True):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), "probe." + names[1],
                                             str(route), str(mode), str(future).lower(), str(bridge).lower()],
                                            f"{label}-{route}-{mode}-{future}")
                        if output != "attempts=2,nominal-interface-super=matched\n":
                            raise RuntimeError("Frozen nominal interface default observer changed")
                        results.append(output)
            return results

        baseline = execute((authored, harness), "authored", False)
        shutil.copytree(authored, originals)
        witness = change_method_flags(originals / "fixture/Owner.class", DESCRIPTOR.encode(), 1, 0x41, name=b"convert")
        before, records = compiler_facts(authored), compiler_facts(originals)
        for owner, record in before.items():
            wanted = deepcopy(record)
            if owner == "Lfixture/Owner;":
                wanted["methods"][BRIDGE]["access"] |= 0x40
            if any(wanted[key] != records[owner].get(key) for key in wanted if key != "sha256"):
                raise RuntimeError("Bridge encoding changed another original declaration")
            relative = owner[1:-1] + ".class"
            if method_code((authored / relative).read_bytes()) != method_code((originals / relative).read_bytes()):
                raise RuntimeError("Bridge encoding changed original compiler Code")
        if execute((originals, harness), "originals", True) != baseline:
            raise RuntimeError("Bridge bit changed original behavior")
        original_hashes, exceptions = hashes(originals), exception_inventory(originals)
        (work / "encoding-witness.json").open("x").write(json.dumps(witness, indent=2) + "\n")
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"authored": authored_hashes, "originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        (work / "baseline.json").open("x").write(json.dumps(baseline) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 10 or len(all_methods) != 17:
            raise RuntimeError("Original nominal interface default inventory changed")
        classes = all_classes - ({"Lfixture/Trace;"} if args.dependency_trace else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        dependency = work / "owned-trace.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            jar.writestr("fixture/Trace.class", (originals / "fixture/Trace.class").read_bytes())
        dependency_hash = sha(dependency)
        mutations = [
            ("Owner", "return Api.super.convert(value);", "return null;", 0),
            ("Owner", "return Api.super.convert(value);", "return Api.super.convert(new Input(0));", 0),
            ("Trace", "parent = owner;", "parent = null;", 0),
            ("Trace", "hit(1);", ";", 0),
            ("Trace", "beginAction.run();", "beginAction.run(); beginAction.run();", 7),
            ("Trace", "beginAction.run();", "try { beginAction.run(); } catch (Throwable failure) { throw new IllegalStateException(failure); }", 2),
            ("Input", "Trace.readAction.run();", ";", 3),
            ("Receiver", "return value ^ Trace.mask;", "return Trace.mask;", 0),
            ("Output", "bits = value;", "bits = (short) value;", 0),
            ("Output", "Trace.outputAction.run();", ";", 5),
            ("Trace", "result = value;", "result = null;", 0),
            ("Trace", "afterAction.run();", "try { afterAction.run(); } catch (Throwable failure) { throw new IllegalStateException(failure); }", 6),
            ("Core", "Trace.hit(0);", ";", 0),
            ("Root", "new Output(transform(value.read()))", "new Output(transform(transform(value.read())))", 0),
        ]
        for index, (name, anchor, replacement, mode) in enumerate(mutations):
            text = (fixture / (name + ".java")).read_text()
            if text.count(anchor) != 1:
                raise RuntimeError("Independent nominal counterexample anchor changed")
            source = work / f"wrong-{index}-source" / (name + ".java")
            source.parent.mkdir()
            source.open("x").write(text.replace(anchor, replacement))
            variant = work / f"wrong-{index}-classes"
            verify.compile_local([source], variant, classpath=authored, release=8)
            if name == "Owner":
                change_method_flags(variant / "fixture/Owner.class", DESCRIPTOR.encode(), 1, 0x41, name=b"convert")
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (variant, originals, harness))),
                                     "probe." + names[1], "0", str(mode), "true", "true"],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / f"wrong-{index}-counterexample.log").open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned nominal interface super mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program " + str(index))

        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_trace:
            classpath += ["--classpath", dependency]
        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original nominal bridge DEX count changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original nominal bridge definitions")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-forwarder")
        witness_found = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            if block[1] != "Lfixture/Owner;" or not re.search(r"(?m)^\s+name\s+: 'convert'", block[2]):
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            if (witness_found or ops not in [[call, "move-result-object", "return-object"] for call in ("invoke-super", "invoke-super/range")]
                    or "Lfixture/Api;.convert:" + DESCRIPTOR not in block[2]):
                raise RuntimeError("Original nominal bridge forwarding body changed")
            witness_found = True
        if not witness_found:
            raise RuntimeError("Missing original nominal forwarding witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial"
                or coverage.get("projected_method_count") != 1 or coverage.get("minimum_java_release") != 9
                or rows[BRIDGE].get("projection_kind") != "interface-super-reference-forwarding-bridge"
                or rows[BRIDGE].get("bridge_target") != TARGET or not rows[BRIDGE].get("metadata_differences")
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Nominal bridge recovery changed original entries or contracts")
        for identity, row in rows.items():
            expected = "source-projected" if identity == BRIDGE else "recovered" if records[row["class"]]["methods"][identity]["code"] else "declaration-only"
            if row["status"] != expected:
                raise RuntimeError("Original nominal bridge Code/declaration role changed")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_trace else None, release=9)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes
                            if any(row["code"] for row in records[c]["methods"].values())}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Nominal bridge recovery invented or omitted an original entry/helper")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for c in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if after[c].get(key) != records[c].get(key):
                    raise RuntimeError("Undisclosed nominal bridge class metadata changed: " + c + ":" + key)
            for identity, metadata in records[c]["methods"].items():
                wanted = deepcopy(metadata)
                if identity == BRIDGE:
                    wanted["access"] &= ~0x40
                if after[c]["methods"].get(identity) != wanted or after_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Undisclosed nominal bridge method metadata or Exceptions changed: " + identity)
        for identity in expected_helpers:
            metadata = after[identity.split("->", 1)[0]]["methods"][identity]
            if (metadata["access"] != 0x000A or not metadata["code"]
                    or metadata["signature"] != "<E:Ljava/lang/Throwable;>(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;^TE;"):
                raise RuntimeError("Generated exception helper changed its private contract")
        runtime = (rebuilt, harness, dependency) if args.dependency_trace else (rebuilt, harness)
        if execute(runtime, "rebuilt", False) != baseline:
            raise RuntimeError("Nominal bridge changed dispatch, identities, effects or exact failures")
        if (hashes(authored) != authored_hashes or hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or sha(args.android_jar) != sdk_hash or sha(args.neverd) != cli_hash or sha(dependency) != dependency_hash):
            raise RuntimeError("Original compiler inputs or frozen observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "stages": 3,
                    "independent_wrong_programs": len(mutations), "original_Code_and_encoding_attributes": "unchanged except the exact bridge access bit",
                    "all_generated_Java": "compiled", "original_metadata_and_Exceptions": "matched except the disclosed bridge bit",
                    "default_dispatch_and_nominal_identity": "matched", "dependency_trace": args.dependency_trace,
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
