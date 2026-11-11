#!/usr/bin/env python3
"""Verify fixed mixed interface-super bridges preserve original forwarding."""
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

DESCRIPTORS = ("(Lfixture/Context;Lfixture/Input;I)I", "(Lfixture/Context;Lfixture/Input;J)J")
BRIDGES = {"Lfixture/Owner;->measure" + descriptor for descriptor in DESCRIPTORS}
PROVIDERS = {"Lfixture/" + name + ";" for name in ("Shared", "SideA", "SideB", "SideC", "Api", "Host", "Host$Base", "Context", "Input")}
compiler_facts = class_identity.compiler_classes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-providers", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-mixed-interface-super-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "mixed_interface_super_forwarding/java/fixture"
        authored, originals, harness = (work / name for name in ("authored", "originals", "harness"))
        verify.compile_local(sorted(fixture.glob("*.java")), authored, release=8)
        names = ("MixedInterfaceSuperCaller", "MixedInterfaceSuperHarness")
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in names], harness, classpath=authored, release=8)
        authored_hashes, harness_hashes = hashes(authored), hashes(harness)
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        sdk_hash, cli_hash = sha(args.android_jar), sha(args.neverd)

        def execute(runtime, label, bridge):
            results = []
            for route in range(7):
                for mode in range(12):
                    for wide in (False, True):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), "probe." + names[1],
                                             str(route), str(mode), str(wide).lower(), str(bridge).lower()],
                                            f"{label}-{route}-{mode}-{wide}")
                        if output != "attempts=2,mixed-interface-super=matched\n":
                            raise RuntimeError("Frozen mixed interface default observer changed")
                        results.append(output)
            return results

        baseline = execute((authored, harness), "authored", False)
        shutil.copytree(authored, originals)
        witness = [change_method_flags(originals / "fixture/Owner.class", descriptor.encode(), 1, 0x41, name=b"measure") for descriptor in DESCRIPTORS]
        before, records = compiler_facts(authored), compiler_facts(originals)
        for owner, record in before.items():
            wanted = deepcopy(record)
            if owner == "Lfixture/Owner;":
                for identity in BRIDGES:
                    wanted["methods"][identity]["access"] |= 0x40
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
        if len(all_classes) != 13 or len(all_methods) != 26:
            raise RuntimeError("Original mixed interface default inventory changed")
        classes = all_classes - (PROVIDERS if args.dependency_providers else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        dependency = work / "owned-providers.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for owner in sorted(PROVIDERS):
                relative = owner[1:-1] + ".class"
                jar.writestr(relative, (originals / relative).read_bytes())
        dependency_hash = sha(dependency)
        mutations = [
            ("Owner", "return Api.super.measure(context, input, value);", "return 0;", 0, 2),
            ("Owner", "Api.super.measure(context, input, value)", "Api.super.measure((Context) input, (Input) context, value)", 0, 2),
            ("Trace", "parent = owner;", "parent = null;", 0, 1),
            ("Api", "Trace.begin(this, context, input, value);", ";", 0, 2),
            ("Trace", "parameter = value;", "parameter = (int) value;", 0, 1),
            ("Trace", "beginAction.run();", "beginAction.run(); beginAction.run();", 8, 1),
            ("Trace", "beginAction.run();", "try { beginAction.run(); } catch (Throwable failure) { throw new IllegalStateException(failure); }", 3, 1),
            ("Values", "Trace.contextAction.run();", ";", 4, 2),
            ("Host", "Trace.baseAction.run();", ";", 5, 1),
            ("Values", "Trace.observed = value;", "Trace.observed = 0;", 0, 2),
            ("Values", "Trace.inputAction.run();", ";", 6, 2),
            ("Trace", "result = value;", "result = 0;", 0, 1),
            ("Trace", "afterAction.run();", "try { afterAction.run(); } catch (Throwable failure) { throw new IllegalStateException(failure); }", 7, 1),
            ("Core", "Trace.hit(0);", ";", 0, 2),
            ("Host", "Trace.baseOwner = this;", "Trace.baseOwner = null;", 0, 1),
            ("Api", "context.wideOffset()", "(long) context.offset()", 0, 1),
        ]
        for index, (name, anchor, replacement, mode, count) in enumerate(mutations):
            text = (fixture / (name + ".java")).read_text()
            if text.count(anchor) != count:
                raise RuntimeError("Independent mixed counterexample anchor changed")
            source = work / f"wrong-{index}-source" / (name + ".java")
            source.parent.mkdir()
            source.open("x").write(text.replace(anchor, replacement))
            variant = work / f"wrong-{index}-classes"
            verify.compile_local([source], variant, classpath=authored, release=8)
            if name == "Owner":
                for descriptor in DESCRIPTORS:
                    change_method_flags(variant / "fixture/Owner.class", descriptor.encode(), 1, 0x41, name=b"measure")
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (variant, originals, harness))),
                                     "probe." + names[1], "0", str(mode), "true", "true"],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / f"wrong-{index}-counterexample.log").open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned mixed interface super mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program " + str(index))

        host_text = (fixture / "Host.java").read_text()
        controls = [
            ("inherited-qualifier", "Base implements Shared", "Base implements Api", "bad type qualifier Api in default super call"),
            ("final-class-shadow", "protected Base() {}", "protected Base() {} public final int measure(Context c, Input i, int v) { return 0; } public final long measure(Context c, Input i, long v) { return 0; }", "overridden method is final"),
        ]
        for label, anchor, replacement, diagnostic in controls:
            if host_text.count(anchor) != 1:
                raise RuntimeError("Independent superclass control anchor changed")
            source = work / (label + "-source") / "Host.java"
            source.parent.mkdir()
            source.open("x").write(host_text.replace(anchor, replacement))
            variant = work / (label + "-classes")
            verify.compile_local([source], variant, classpath=authored, release=8)
            result = subprocess.run([verify.javac, "-proc:none", "-encoding", "UTF-8", "--release", "8",
                                     "-classpath", os.pathsep.join(map(str, (variant, authored))),
                                     "-d", work / (label + "-owner"), fixture / "Owner.java"],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-compiler-control.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or diagnostic not in result.stderr:
                raise RuntimeError("Independent compiler missed superclass selection conflict")
        (work / "superclass-controls.json").open("x").write(json.dumps({"status": "verified", "controls": [row[0] for row in controls]}) + "\n")

        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_providers:
            classpath += ["--classpath", dependency]
        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original mixed bridge DEX count changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original mixed bridge definitions")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-forwarder")
        witnesses = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            if block[1] != "Lfixture/Owner;" or not re.search(r"(?m)^\s+name\s+: 'measure'", block[2]):
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            target = re.search(r"Lfixture/Api;\.measure:([^\s]+)", block[2])
            descriptor = target[1] if target else ""
            suffix = "-wide" if descriptor.endswith(")J") else ""
            if (descriptor not in DESCRIPTORS or descriptor in witnesses
                    or ops not in [[call, "move-result" + suffix, "return" + suffix] for call in ("invoke-super", "invoke-super/range")]):
                raise RuntimeError("Original mixed bridge forwarding body changed")
            witnesses.add(descriptor)
        if witnesses != set(DESCRIPTORS):
            raise RuntimeError("Missing original mixed forwarding witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial"
                or coverage.get("projected_method_count") != 2
                or coverage.get("minimum_java_release", 8) != (8 if args.dependency_providers else 9)
                or any(rows[identity].get("projection_kind") != "interface-super-mixed-forwarding-bridge"
                       or rows[identity].get("bridge_target") != identity.replace("Lfixture/Owner;", "Lfixture/Api;")
                       or not rows[identity].get("metadata_differences") for identity in BRIDGES)
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Mixed bridge recovery changed original entries or contracts")
        for identity, row in rows.items():
            expected = "source-projected" if identity in BRIDGES else "recovered" if records[row["class"]]["methods"][identity]["code"] else "declaration-only"
            if row["status"] != expected:
                raise RuntimeError("Original mixed bridge Code/declaration role changed")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_providers else None, release=8 if args.dependency_providers else 9)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes
                            if any(row["code"] for row in records[c]["methods"].values())}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Mixed bridge recovery invented or omitted an original entry/helper")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for c in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if after[c].get(key) != records[c].get(key):
                    raise RuntimeError("Undisclosed mixed bridge class metadata changed: " + c + ":" + key)
            for identity, metadata in records[c]["methods"].items():
                wanted = deepcopy(metadata)
                if identity in BRIDGES:
                    wanted["access"] &= ~0x40
                if after[c]["methods"].get(identity) != wanted or after_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Undisclosed mixed bridge method metadata or Exceptions changed: " + identity)
        for identity in expected_helpers:
            metadata = after[identity.split("->", 1)[0]]["methods"][identity]
            if (metadata["access"] != 0x000A or not metadata["code"]
                    or metadata["signature"] != "<E:Ljava/lang/Throwable;>(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;^TE;"):
                raise RuntimeError("Generated exception helper changed its private contract")
        runtime = (rebuilt, harness, dependency) if args.dependency_providers else (rebuilt, harness)
        if execute(runtime, "rebuilt", False) != baseline:
            raise RuntimeError("Mixed bridge changed dispatch, identities, effects or exact failures")
        if (hashes(authored) != authored_hashes or hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or sha(args.android_jar) != sdk_hash or sha(args.neverd) != cli_hash or sha(dependency) != dependency_hash):
            raise RuntimeError("Original compiler inputs or frozen observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "stages": 3,
                    "independent_wrong_programs": len(mutations), "independent_superclass_compiler_controls": len(controls), "original_Code_and_encoding_attributes": "unchanged except the exact bridge access bit",
                    "all_generated_Java": "compiled", "original_metadata_and_Exceptions": "matched except the disclosed bridge bit",
                    "default_dispatch_and_nominal_identity": "matched", "dependency_providers": args.dependency_providers,
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
