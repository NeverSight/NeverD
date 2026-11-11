#!/usr/bin/env python3
"""Verify one protected final assignment retains initialization and abrupt effects."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
import hashlib
import json
import os
import shutil
import struct
from pathlib import Path
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from scripts.test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def private_constructor(path):
    before = path.read_bytes()
    records = class_records(before)
    offset = records["pool_end"] + 6
    offset += 2 + 2 * int.from_bytes(before[offset:offset + 2], "big")
    target = None
    for fields in (True, False):
        count = int.from_bytes(before[offset:offset + 2], "big")
        offset += 2
        for _ in range(count):
            flags, name, descriptor, attributes = struct.unpack_from(">HHHH", before, offset)
            if not fields and records["strings"][name] == b"<init>" and records["strings"][descriptor] == b"(Ljava/lang/Object;J)V":
                if flags != 1 or target is not None:
                    raise RuntimeError("Owned constructor access changed")
                target = offset
            offset += 8
            for _ in range(attributes):
                _, length = struct.unpack_from(">HI", before, offset)
                offset += 6 + length
    if target is None:
        raise RuntimeError("Missing exact owned constructor")
    after = before[:target] + (2).to_bytes(2, "big") + before[target + 2:]
    if method_code(before) != method_code(after):
        raise RuntimeError("Private access control changed Code")
    class_records(after)
    path.write_bytes(after)
    return {"identity": "Lfixture/Value;-><init>(Ljava/lang/Object;J)V", "before_access": 1,
            "after_access": 2, "all_other_classfile_bytes": "unchanged",
            "before_sha256": hashlib.sha256(before).hexdigest(),
            "after_sha256": hashlib.sha256(after).hexdigest()}


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-protected-single-final-")
    with context as directory:
        work = Path(directory).resolve()
        v = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = args.fixture_dir or FIXTURES / "protected_single_final_store"
        harness_root = fixture / "harness" if args.fixture_dir else FIXTURES / "harness"
        originals, harness = work / "originals", work / "harness"
        name = "ProtectedSingleFinalHarness"
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        compiler.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=8)
        compiler.compile_local([harness_root / (n + ".java") for n in (name, "ProtectedSingleFinalCaller")], harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness), "sdk": sha(sdk), "cli": sha(args.neverd)}
        (work / "frozen-input-hashes.json").write_text(json.dumps(frozen, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(compiler, originals)
        if (len(all_classes), len(all_methods)) != (5, 6):
            raise RuntimeError("Owned protected single final class inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        providers = {"Lfixture/Value;", "Lfixture/Wrapped;", "Lfixture/Trace;", "Lfixture/Action;"}
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
                for operation in range(2):
                    for mode in range(16):
                        output = v.run([compiler.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), str(operation), str(mode)], f"{label}-{route}-{operation}-{mode}")
                        if len(output.splitlines()) != 2 or any(not line.startswith(f"route={route},operation={operation},mode={mode},attempt={i},trace=") for i, line in enumerate(output.splitlines())):
                            raise RuntimeError("Frozen protected single final observer output changed")
                        results.append(output)
            return results

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").write_text(json.dumps(baseline, indent=2) + "\n")
        wrong = [
            ("lost-value", "Core.java", "this.value = new Value(input, tag);", "this.value = null;", 0, 0),
            ("wrong-input", "Core.java", "new Value(input, tag)", "new Value(null, tag)", 0, 0),
            ("widened-catch", "Core.java", "catch (java.io.IOException failure)", "catch (RuntimeException other) { throw new Wrapped(other); } catch (java.io.IOException failure)", 0, 3),
            ("changed-cause", "Core.java", "throw new Wrapped(failure);", "throw new Wrapped(new java.io.IOException(failure));", 0, 2),
            ("lost-after", "Core.java", "Trace.afterAction.run();", ";", 1, 0),
            ("unconditional-after", "Core.java", "if (after) Trace.afterAction.run();", "Trace.afterAction.run();", 0, 0),
            ("changed-input-identity", "Value.java", "this.input = input;", "this.input = new Object();", 0, 0),
            ("lost-allocation-identity", "Value.java", "Trace.allocated = this;", "Trace.allocated = null;", 0, 0),
            ("lost-original-cause", "Wrapped.java", "super(cause);", "super((Throwable)null);", 0, 2),
            ("repeated-after", "Core.java", "if (after) Trace.afterAction.run();", "if (after) { Trace.afterAction.run(); Trace.afterAction.run(); }", 1, 0),
        ]
        for label, filename, before, after, operation, mode in wrong:
            original_text = (fixture / "java/fixture" / filename).read_text()
            if original_text.count(before) != 1:
                raise RuntimeError("Owned wrong-program anchor changed")
            source = work / label / filename
            source.parent.mkdir()
            source.write_text(original_text.replace(before, after))
            compiled = work / (label + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            p = subprocess.run([compiler.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), name, "1", str(operation), str(mode)], capture_output=True, text=True, timeout=1200, env=v.environment)
            (work / (label + "-counterexample.log")).write_text(p.stdout + p.stderr)
            if p.returncode == 0 or "owned protected single final mismatch" not in p.stderr:
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
            raise RuntimeError("D8 changed original protected single final definitions")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        if args.baseline_neverd:
            red = subprocess.run([args.baseline_neverd.resolve(), "mobile", dex, *cp, "--timeout", "600", "-o", work / "before"], text=True, capture_output=True, env=v.environment, timeout=1200)
            (work / "before.log").write_text(red.stdout + red.stderr)
            if red.returncode == 0 or "needs one verified initialization on every return" not in red.stdout + red.stderr:
                raise RuntimeError("Expected the prior CLI's protected final-store rejection")
        output = work / "recovered"
        v.run([args.neverd.resolve(), "mobile", dex, *cp, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        release = coverage.get("minimum_java_release", 8)
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("field_source_bindings")
                or coverage.get("requires_java_rebuild_postprocessing") or release != 8):
            raise RuntimeError("Protected single final recovery changed original definitions or generic metadata")
        declarations = {m for owner in classes for m, row in facts[owner]["methods"].items() if row["access"] & 0x500}
        if ({m["identity"] for m in coverage["methods"] if m["status"] == "declaration-only"} != declarations
                or coverage["declaration_only_method_count"] != len(declarations)
                or any(m["status"] not in ("recovered", "declaration-only") for m in coverage["methods"])
                or any(m["instruction_count"] for m in coverage["methods"] if m["status"] == "declaration-only")):
            raise RuntimeError("Protected single final Code/declaration roles changed")
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        allowed = {o + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt = work / "rebuilt"
        deps = [provider] if args.dependency_provider else []
        compiler.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=release, classpath=os.pathsep.join(map(str, deps)) if deps else None)
        actual_classes, actual_methods = compiler_inventory(compiler, rebuilt)
        if actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed or any(actual_methods[m] != all_methods[m] for m in methods):
            raise RuntimeError("Protected single final recovery invented or omitted definitions/roles")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(facts[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if wanted.pop("major") != 52 or actual.pop("major") != 44 + release:
                raise RuntimeError("Owned compiler release changed")
            if actual != wanted or any(after_exceptions[m] != exceptions[m] for m in facts[owner]["methods"]):
                raise RuntimeError("Protected single final original metadata/Exceptions changed: " + owner)
        if execute((rebuilt, harness, *deps), "rebuilt") != baseline:
            raise RuntimeError("Protected single final result, effects or exception identity changed")
        changed = work / "private-constructor-originals"
        shutil.copytree(originals, changed)
        receipt = private_constructor(changed / "fixture/Value.class")
        (work / "private-constructor-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        privatecp = ["--classpath", sdk]
        if args.dependency_provider:
            privateprovider = work / "private-constructor-provider.jar"
            with zipfile.ZipFile(privateprovider, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                for owner in sorted(providers):
                    entry = owner[1:-1] + ".class"
                    archive.writestr(entry, (changed / entry).read_bytes())
            privatecp += ["--classpath", privateprovider]
        privatedex = work / "private-constructor-dex"
        privatedex.mkdir()
        privatepaths = [changed / p.relative_to(originals) for p in paths]
        v.run([*v.d8, "--release", "--min-api", "24", *privatecp, "--output", privatedex, *privatepaths], "private-constructor-d8")
        rejection = subprocess.run([args.neverd.resolve(), "mobile", privatedex / "classes.dex", *privatecp, "--timeout", "600", "-o", work / "private-constructor-recovered"], text=True, capture_output=True, timeout=1200, env=v.environment)
        (work / "private-constructor-rejection.log").write_text(rejection.stdout + rejection.stderr)
        if rejection.returncode == 0 or "private constructor belongs to a different top-level class" not in rejection.stdout + rejection.stderr:
            raise RuntimeError("Foreign private constructor scope was not rejected")
        if frozen != {"originals": hashes(originals), "harness": hashes(harness), "sdk": sha(sdk), "cli": sha(args.neverd)} or sha(dex) != dex_hash or sha(provider) != provider_hash:
            raise RuntimeError("Frozen protected single final input changed")
        summary = {"status": "verified", "classes": len(classes), "methods": len(methods),
                   "all_java_units": len(list((output / "sources").rglob("*.java"))),
                   "fresh_jvm_cases": 128, "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "minimum_java_release": release, "same_Code_access_controls": 1,
                   "dependency_provider": args.dependency_provider}
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
