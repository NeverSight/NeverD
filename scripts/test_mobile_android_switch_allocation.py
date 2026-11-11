#!/usr/bin/env python3
"""Verify allocation precedes switch arguments, including terminal throw arms."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-inputs", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-switch-allocation-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "switch_allocation/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=14)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("SwitchAllocationCaller", "SwitchAllocationHarness")],
                             harness, classpath=originals, release=14)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 12:
            raise RuntimeError("Owned switch allocation inventory changed")
        providers = {"Lfixture/Provider;", "Lfixture/Target;"} if args.dependency_inputs else set()
        classes = all_classes - providers
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if providers:
            classpath += ["--classpath", dependency]
        cases = [(selector, mode, 0) for selector in (-1, 0, 1, 2, 37, -2147483648, 2147483647) for mode in (0,)]
        cases += [(1, mode, 0) for mode in (11, 12)]
        cases += [(0, mode, kind) for mode in (1, 2, 3, 4, 6, 8) for kind in (0, 1)]
        cases += [(-1, mode, kind) for mode in (3, 7) for kind in (0, 1)]

        def execute(runtime, label):
            outputs = []
            for route in range(2):
                for selector, mode, kind in cases:
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "SwitchAllocationHarness", str(route), str(selector), str(mode), str(kind)],
                        f"{label}-{route}-{selector}-{mode}-{kind}")
                    pattern = f"route={route},selector={selector},mode={mode},error={kind},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                        raise RuntimeError("Owned switch allocation observer changed: " + output)
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned switch allocation DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an owned switch allocation entry")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-switch-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Creator;" or not name or name[1] != "make":
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            ops = [op for op in ops if op not in ("nop", "packed-switch-data")]
            if (ops.count("packed-switch") != 1 or ops.count("new-instance") != 3
                    or ops.index("new-instance") > ops.index("packed-switch")
                    or ops.count("throw") != 1 or ops[-2:] != ["invoke-direct", "return-object"]
                    or block[2].count("Lfixture/Provider;.choice:") != 3):
                raise RuntimeError("D8 changed the switch/allocation/terminal-throw witness")
            witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original switch allocation witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release") != 14):
            raise RuntimeError("Switch allocation recovery changed original entries or contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=14,
                             classpath=dependency if providers else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if owner != "Lfixture/Hook;"}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Switch allocation recovery invented or omitted a runtime entry")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != after[owner].get(key):
                    raise RuntimeError("Original switch allocation class metadata changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                if after[owner]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original switch allocation method metadata/Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if providers else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Switch allocation changed ordered effects or reference identities")
        mutations = [
            ("late-allocation", "Creator", "return new Target(captured, switch (selected) {", "Object chosen = switch (selected) {", 0, 0),
            ("lost-capture", "Creator", "new Target(captured,", "new Target(Trace.first,", 0, 0),
            ("lost-default", "Creator", 'default -> throw new IllegalStateException(new StringBuilder().append("owned selector ").append(Provider.invalid(selected)).toString());', "default -> Provider.choice(0);", -1, 0),
            ("lost-message-effect", "Creator", ".append(Provider.invalid(selected))", ".append(selected)", -1, 7),
            ("late-provider-read", "Provider", "return value;", "return Trace.second;", 0, 0),
            ("late-first-store", "Target", "first = one;\n        Trace.hit(6);", "Trace.hit(6);\n        first = one;", 0, 6),
        ]
        for label, name, before, replacement, selector, mode in mutations:
            text = (fixture / ("fixture/" + name + ".java")).read_text()
            if text.count(before) != 1:
                raise RuntimeError("Original switch allocation counterexample anchor changed")
            changed = text.replace(before, replacement)
            if label == "late-allocation":
                changed = changed.replace("        });", "        };\n        return new Target(captured, chosen);")
            path = work / label / (name + ".java")
            path.parent.mkdir()
            path.open("x").write(changed)
            wrong = work / (label + "-classes")
            verify.compile_local([path], wrong, classpath=originals, release=14)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness))),
                "SwitchAllocationHarness", "1", str(selector), str(mode), "0"], text=True, capture_output=True,
                timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned switch allocation contract" not in result.stderr:
                raise RuntimeError("Independent observer missed wrong switch allocation: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original switch allocation inputs or frozen observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_inputs": args.dependency_inputs,
                    "allocation_switch_terminal_throw_and_partial_stores": "matched",
                    "all_original_entries_metadata_and_exceptions": "unchanged", "independent_wrong_programs": len(mutations),
                    "original_inputs_and_immutable_callers": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
