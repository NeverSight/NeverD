#!/usr/bin/env python3
"""Verify static final assignments with resource cleanup and suppressed exceptions."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from scripts.test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "java-home", "compiler-java-home"):
        parser.add_argument("--" + name, required=True, type=Path)
    for name in ("d8", "work-dir", "baseline-neverd", "fixture-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-static-final-resource-")
    with context as directory:
        work = Path(directory).resolve()
        v = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = args.fixture_dir or FIXTURES / "static_final_resource"
        harness_root = fixture / "harness" if args.fixture_dir else FIXTURES / "harness"
        originals, harness = work / "originals", work / "harness"
        name = "StaticFinalResourceHarness"
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        compiler.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=8)
        compiler.compile_local([harness_root / (n + ".java") for n in (name, "StaticFinalResourceCaller")], harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness), "sdk": sha(sdk), "cli": sha(args.neverd)}
        (work / "frozen-input-hashes.json").write_text(json.dumps(frozen, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(compiler, originals)
        if (len(all_classes), len(all_methods)) != (3, 10):
            raise RuntimeError("Owned static final resource class inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        providers = {"Lfixture/Provider;"}
        classes = all_classes - providers if args.dependency_provider else all_classes
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        provider = work / "owned-provider.jar"
        with zipfile.ZipFile(provider, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for owner in sorted(providers):
                entry = owner[1:-1] + ".class"
                archive.writestr(entry, (originals / entry).read_bytes())
        provider_hash = sha(provider)

        def execute(runtime, label):
            results = []
            for route in range(4):
                for mode in range(18):
                    output = v.run([compiler.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), str(mode)], f"{label}-{route}-{mode}")
                    if len(output.splitlines()) != 2 or any(not re.fullmatch(
                            f"route={route},mode={mode},attempt={i},events=[0-9]+", line)
                            for i, line in enumerate(output.splitlines())):
                        raise RuntimeError("Frozen static final resource observer output changed")
                    results.append(output)
            return results

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").write_text(json.dumps(baseline, indent=2) + "\n")
        resource_block = "try (Provider resource = Trace.create()) {\n            VALUE = resource.get();\n        }"
        wrong = [
            ("skip-get", "Core.java", "VALUE = resource.get();", "VALUE = new Object();", 0),
            ("repeat-get", "Core.java", "VALUE = resource.get();", "resource.get(); VALUE = resource.get();", 0),
            ("copied-result", "Core.java", "VALUE = resource.get();", "resource.get(); VALUE = new Object();", 0),
            ("early-close", "Core.java", "VALUE = resource.get();", "Object saved = resource.get(); resource.close(); VALUE = saved;", 0),
            ("skip-close", "Core.java", resource_block, "Provider resource = Trace.create(); VALUE = resource.get();", 0),
            ("late-store", "Core.java", resource_block, "Object saved; try (Provider resource = Trace.create()) { saved = resource.get(); } VALUE = saved;", 0),
            ("lost-suppression", "Core.java", resource_block, "Provider resource = Trace.create(); try { VALUE = resource.get(); } finally { resource.close(); }", 12),
            ("wrapped-open", "Trace.java", "return hook.open();", "try { return hook.open(); } catch (Throwable failure) { throw new RuntimeException(failure); }", 3),
            ("repeat-open", "Trace.java", "return hook.open();", "hook.open(); return hook.open();", 0),
        ]
        for label, filename, before, after, mode in wrong:
            original_text = (fixture / "java/fixture" / filename).read_text()
            if original_text.count(before) != 1:
                raise RuntimeError("Owned wrong-program anchor changed")
            source = work / label / filename
            source.parent.mkdir()
            source.write_text(original_text.replace(before, after, 1))
            compiled = work / (label + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            p = subprocess.run([compiler.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), name, "1", str(mode)], capture_output=True, text=True, timeout=1200, env=v.environment)
            (work / (label + "-counterexample.log")).write_text(p.stdout + p.stderr)
            if p.returncode == 0 or "owned static final resource contract" not in p.stderr:
                raise RuntimeError("Frozen observer missed wrong program: " + label)
        cp = ["--classpath", sdk]
        if args.dependency_provider:
            cp += ["--classpath", provider]
        dexdir = work / "dex"
        dexdir.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        v.run([*v.d8, "--release", "--min-api", "24", *cp, "--output", dexdir, *paths], "d8-original")
        dex = dexdir / "classes.dex"
        dex_hash = sha(dex)
        dump = v.run([v.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original static final resource definitions")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = v.run([v.dexdump, "-d", dex], "original-static-cleanup")
        witnessed = []
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            if block[1] != "Lfixture/Core;" or "'<clinit>'" not in block[2]:
                continue
            ops = [(int(pc, 16), op, tail) for pc, op, tail in re.findall(r"\|([0-9a-fA-F]+):\s+(\S+)([^\n]*)", block[2])]
            stores = [pc for pc, op, tail in ops if op == "sput-object" and "Lfixture/Core;.VALUE:" in tail]
            returns = [pc for pc, op, _ in ops if op == "return-void"]
            regions = [(int(a, 16), int(b, 16)) for a, b in re.findall(r"0x([0-9a-f]+) - 0x([0-9a-f]+)", block[2])]
            if (len(stores) != 1 or len(returns) != 1 or len(regions) != 2
                    or not any(a <= stores[0] < b for a, b in regions)
                    or not any(pc > returns[0] and op.startswith("if-") for pc, op, _ in ops)
                    or not any(pc > returns[0] and "addSuppressed:" in tail for pc, _, tail in ops)):
                raise RuntimeError("D8 changed the original protected store and branching suppression witness")
            witnessed.append({"store_pc": stores[0], "return_pc": returns[0], "protected_ranges": regions,
                              "operations": [(pc, op) for pc, op, _ in ops]})
        if len(witnessed) != 1:
            raise RuntimeError("Missing original static resource initializer witness")
        (work / "input-method-witness.json").write_text(json.dumps({"dex_sha256": dex_hash, "methods": witnessed}, indent=2) + "\n")
        if args.baseline_neverd:
            red = subprocess.run([args.baseline_neverd.resolve(), "mobile", dex, *cp, "--timeout", "600", "-o", work / "before"], text=True, capture_output=True, env=v.environment, timeout=1200)
            (work / "before.log").write_text(red.stdout + red.stderr)
            if red.returncode == 0 or "needs one verified initialization on every return" not in red.stdout + red.stderr:
                raise RuntimeError("Expected the prior CLI's closed context invocation rejection")
        output = work / "recovered"
        v.run([args.neverd.resolve(), "mobile", dex, *cp, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        release = coverage.get("minimum_java_release", 8)
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("field_source_bindings")
                or coverage.get("requires_java_rebuild_postprocessing") or release != 8):
            raise RuntimeError("Static final resource recovery changed original definitions or generic metadata")
        declarations = {m for owner in classes for m, row in facts[owner]["methods"].items() if row["access"] & 0x500}
        if ({m["identity"] for m in coverage["methods"] if m["status"] == "declaration-only"} != declarations
                or coverage["declaration_only_method_count"] != len(declarations)
                or any(m["status"] not in ("recovered", "declaration-only") for m in coverage["methods"])
                or any(m["instruction_count"] for m in coverage["methods"] if m["status"] == "declaration-only")):
            raise RuntimeError("Static final resource Code/declaration roles changed")
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        allowed = {o + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt = work / "rebuilt"
        deps = [provider] if args.dependency_provider else []
        compiler.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=release, classpath=os.pathsep.join(map(str, deps)) if deps else None)
        actual_classes, actual_methods = compiler_inventory(compiler, rebuilt)
        if actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed or any(actual_methods[m] != all_methods[m] for m in methods):
            raise RuntimeError("Static final resource recovery invented or omitted definitions/roles")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(facts[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if wanted.pop("major") != 52 or actual.pop("major") != 44 + release:
                raise RuntimeError("Owned compiler release changed")
            if actual != wanted or any(after_exceptions[m] != exceptions[m] for m in facts[owner]["methods"]):
                raise RuntimeError("Static final resource original metadata/Exceptions changed: " + owner)
        (work / "compiled-metadata.json").write_text(json.dumps({"before": facts, "after": after, "exceptions_before": exceptions, "exceptions_after": after_exceptions}, indent=2) + "\n")
        if execute((rebuilt, harness, *deps), "rebuilt") != baseline:
            raise RuntimeError("Static final resource result, effects or exception identity changed")
        if frozen != {"originals": hashes(originals), "harness": hashes(harness), "sdk": sha(sdk), "cli": sha(args.neverd)} or sha(dex) != dex_hash or sha(provider) != provider_hash:
            raise RuntimeError("Frozen static final resource input changed")
        summary = {"status": "verified", "classes": len(classes), "methods": len(methods),
                   "all_java_units": len(list((output / "sources").rglob("*.java"))),
                   "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "minimum_java_release": release,
                   "dependency_provider": args.dependency_provider}
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
