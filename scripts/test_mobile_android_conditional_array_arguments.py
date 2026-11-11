#!/usr/bin/env python3
"""Verify ordinary reference array operations in conditional allocation arguments."""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import re
import subprocess
import tempfile
import zipfile
from contextlib import nullcontext
from pathlib import Path

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from scripts.test_mobile_android_visibility_generic_bridge import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_visibility_generic_bridge import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-conditional-array-arguments-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "conditional_array_arguments/java"
        originals, harness = work / "originals", work / "harness"
        name = "ConditionalArrayArgumentsHarness"
        sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
        frozen_sdk, frozen_cli = sha(args.android_jar), sha(args.neverd)
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (n + ".java") for n in (name, "ConditionalArrayArgumentsCaller")],
                             harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").write_text(json.dumps(frozen, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (4, 12):
            raise RuntimeError("Owned conditional array argument inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        classes = all_classes - ({"Lfixture/Probe;"} if args.dependency_provider else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            jar.writestr("fixture/Probe.class", (originals / "fixture/Probe.class").read_bytes())
        frozen_dependency = sha(dependency)

        def execute(runtime, label):
            results = []
            for route in range(4):
                for branch in range(2):
                    for mode in range(16):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), name,
                                             str(route), str(branch), str(mode)], f"{label}-{route}-{branch}-{mode}")
                        if output != "attempts=2,conditional-array-arguments=matched\n":
                            raise RuntimeError("Frozen conditional array argument observer changed")
                        results.append(output)
            return results

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").write_text(json.dumps(baseline, indent=2) + "\n")
        texts = {p.stem: p.read_text() for p in fixture.rglob("*.java")}
        selected = "flag ? Trace.probe.select(left) : Trace.probe.select(right)"
        mutations = [
            ("early-selection", "Core", "return new Result(\n            " + selected + ",",
             "Object selected = " + selected + ";\n        return new Result(selected,", 0),
            ("wrong-array-class", "Core", "new String[size]", "new Object[size]", 0),
            ("fixed-length", "Core", "new String[size]", "new String[3]", 1),
            ("wrong-first-index", "Core", "values[0] =", "values[1] =", 0),
            ("wrong-second-index", "Core", "values[1] =", "values[0] =", 0),
            ("lost-first-store", "Core", "values[0] = ", "", 0),
            ("lost-second-store", "Core", "values[1] = ", "", 0),
            ("early-bounds-check", "Core", "element(0, left)", "element(values[0] == null ? 0 : 0, left)", 1),
            ("stronger-value-cast", "Core", "values[0] = Trace.probe.element(0, left)",
             "values[0] = (String)Trace.probe.element(0, left)", 4),
            ("late-finish-receiver", "Core", "Trace.probe.finish(", "finish(", 12),
            ("wrapped-checked", "Core", "Trace.probe.select(left)", "select(left)", 7),
            ("lost-array-callback", "Core", "Trace.capture(new String[size])", "new String[size]", 0),
            ("lost-constructor-callback", "Result", "Trace.probe.constructed(this);", ";", 0),
            ("lost-selected-field", "Result", "this.selected = selected;", "this.selected = null;", 0),
            ("lost-array-identity", "Result", "this.values = values;", "this.values = new Object[0];", 15),
            ("lost-class-initialization", "Result", "static { Trace.probe.initialize(); }", "", 0),
        ]
        for label, owner, before, after, mode in mutations:
            text = texts[owner]
            if text.count(before) != 1:
                raise RuntimeError("Owned conditional array argument mutation anchor changed: " + label)
            text = text.replace(before, after)
            if label == "late-finish-receiver":
                text = text.rsplit("}", 1)[0] + """    private static Object[] finish(Object[] values, Object first, Object second) throws IOException {
        return Trace.probe.finish(values, first, second);
    }
}
"""
            if label == "wrapped-checked":
                text = text.rsplit("}", 1)[0] + """    private static Object select(Object value) throws IOException {
        try { return Trace.probe.select(value); }
        catch (IOException failure) { throw new IOException(failure); }
    }
}
"""
            source = work / label / (owner + ".java")
            source.parent.mkdir()
            source.write_text(text)
            compiled = work / (label + "-classes")
            verify.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     name, "1", "1", str(mode)], text=True, capture_output=True,
                                    timeout=1200, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned conditional array arguments mismatch" not in result.stderr:
                raise RuntimeError("Frozen observer missed wrong conditional array argument program: " + label)

        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]
        dexdir = work / "dex"
        dexdir.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--release", "--min-api", "24", *classpath, "--output", dexdir, *paths], "d8-original")
        dex = dexdir / "classes.dex"
        frozen_dex = sha(dex)
        if list(dexdir.glob("classes*.dex")) != [dex]:
            raise RuntimeError("Owned conditional array argument DEX count changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original conditional array argument definition")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", dex], "sdk-instructions")
        witnessed = []
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            method = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not method or method[1] != "make":
                continue
            ops = [(int(pc, 16), op, tail) for pc, op, tail in re.findall(r"\|([0-9a-fA-F]+):\s+(\S+)([^\n]*)", block[2])]
            roots = [pc for pc, op, tail in ops if op == "new-instance" and "Lfixture/Result;" in tail]
            inits = [pc for pc, op, tail in ops if op.startswith("invoke-direct") and "Lfixture/Result;.<init>:" in tail]
            if len(roots) != 1 or len(inits) != 1:
                raise RuntimeError("Original result allocation inventory changed")
            body = [(pc, op) for pc, op, _ in ops if roots[0] < pc < inits[0]]
            if (sum(op == "new-array" for _, op in body) != 1 or sum(op == "aput-object" for _, op in body) != 2
                    or sum(op.startswith("if-") for _, op in body) != 1
                    or any(op.startswith("filled-new-array") for _, op, _ in ops)):
                raise RuntimeError("D8 moved or replaced original conditional array operations")
            witnessed.append({"allocation_pc": roots[0], "initializer_pc": inits[0], "argument_operations": body})
        if len(witnessed) != 1:
            raise RuntimeError("Missing conditional array argument DEX witness")
        (work / "input-method-witness.json").write_text(json.dumps({"status": "verified", "dex_sha256": frozen_dex,
                                                                  "methods": witnessed}, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        expected_status = {m: "recovered" if facts[c]["methods"][m]["code"] else "declaration-only"
                           for c in classes for m in facts[c]["methods"]}
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage.get("generic_signature_projections")
                or coverage.get("class_source_bindings") or coverage.get("minimum_java_release", 8) != 14
                or {r["identity"]: r["status"] for r in coverage["methods"]} != expected_status):
            raise RuntimeError("Conditional array argument coverage or original signatures changed")
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        allowed = {o + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt = work / "rebuilt"
        dependencies = [dependency] if args.dependency_provider else []
        sources = sorted((output / "sources").rglob("*.java"))
        if len(sources) != len(classes):
            raise RuntimeError("Conditional array argument source inventory changed")
        verify.compile_local(sources, rebuilt, classpath=dependency if dependencies else None, release=14)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed:
            raise RuntimeError("Conditional array argument source invented or omitted a definition")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(facts[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {m: f for m, f in actual["methods"].items() if m not in helpers}
            if wanted.pop("major") != 52 or actual.pop("major") != 58 or wanted != actual:
                raise RuntimeError("Original conditional array argument metadata or Code roles changed: " + owner)
            if any(after_exceptions[m] != exceptions[m] for m in facts[owner]["methods"]):
                raise RuntimeError("Original conditional array argument Exceptions changed")
        if execute((rebuilt, harness, *dependencies), "rebuilt") != baseline:
            raise RuntimeError("Conditional array argument effects, identities or failures changed")
        if (frozen != {"originals": hashes(originals), "harness": hashes(harness)} or sha(args.neverd) != frozen_cli
                or sha(args.android_jar) != frozen_sdk or sha(dependency) != frozen_dependency or sha(dex) != frozen_dex):
            raise RuntimeError("Frozen conditional array argument inputs changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "all_java_units": len(sources), "minimum_java_release": 14, "fresh_jvm_cases": len(baseline),
                   "attempts_per_case": 2, "wrong_programs": len(mutations), "dependency_provider": args.dependency_provider,
                   "original_metadata_code_roles_and_exceptions": "unchanged", "ordered_array_operation_witness": "verified"}
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
