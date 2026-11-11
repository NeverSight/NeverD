#!/usr/bin/env python3
"""Verify closed checked throws on private context/callback generic results."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from scripts.test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def omit_caller_exceptions(path):
    """Retain all original Code and constant-pool bytes while omitting caller Exceptions."""
    before = path.read_bytes()
    records = class_records(before)
    strings = records["strings"]
    offset = records["pool_end"] + 6
    offset += 2 + 2 * int.from_bytes(before[offset:offset + 2], "big")
    patches, selected = [], []
    for fields in (True, False):
        count = int.from_bytes(before[offset:offset + 2], "big")
        offset += 2
        for _ in range(count):
            start = offset
            flags, name, descriptor, attributes = struct.unpack_from(">HHHH", before, offset)
            offset += 8
            for _ in range(attributes):
                key, length = struct.unpack_from(">HI", before, offset)
                end = offset + 6 + length
                if not fields and strings[name] in (b"call", b"callStatic") and strings[key] == b"Exceptions":
                    if flags != (1 if strings[name] == b"call" else 9):
                        raise RuntimeError("Owned public caller flags changed")
                    if length != 4 or int.from_bytes(before[offset + 6:offset + 8], "big") != 1:
                        raise RuntimeError("Owned caller exception list changed")
                    entry = int.from_bytes(before[offset + 8:end], "big")
                    if strings[records["classes"][entry]] != b"java/io/IOException":
                        raise RuntimeError("Owned caller exception type changed")
                    patches.extend(((start + 6, start + 8, (attributes - 1).to_bytes(2, "big")), (offset, end, b"")))
                    selected.append(strings[name].decode())
                offset = end
    if sorted(selected) != ["call", "callStatic"]:
        raise RuntimeError("Missing both exact caller Exceptions attributes")
    after = before
    for start, end, replacement in sorted(patches, reverse=True):
        after = after[:start] + replacement + after[end:]
    if method_code(before) != method_code(after) or before[:records["pool_end"]] != after[:records["pool_end"]]:
        raise RuntimeError("Caller Exceptions mutation changed Code or constant pool")
    class_records(after)
    path.write_bytes(after)
    return {"methods": selected, "original_declared_throws": ["Ljava/io/IOException;"],
            "encoded_declared_throws": [], "Code_and_constant_pool": "unchanged"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "java-home", "compiler-java-home"):
        parser.add_argument("--" + name, required=True, type=Path)
    for name in ("d8", "work-dir", "baseline-neverd", "fixture-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--omit-caller-throws", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-private-context-throws-")
    with context as directory:
        work = Path(directory).resolve()
        v = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = args.fixture_dir or FIXTURES / "private_context_callback_throws"
        harness_root = fixture / "harness" if args.fixture_dir else FIXTURES / "harness"
        originals, harness = work / "originals", work / "harness"
        name = "PrivateContextThrowsHarness"
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        compiler.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=8)
        compiler.compile_local([harness_root / (n + ".java") for n in (name, "PrivateContextThrowsCaller")], harness, classpath=originals, release=8)
        if args.omit_caller_throws:
            receipt = omit_caller_exceptions(originals / "fixture/Core.class")
            (work / "caller-throws-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        frozen = {"originals": hashes(originals), "harness": hashes(harness), "sdk": sha(sdk), "cli": sha(args.neverd)}
        (work / "frozen-input-hashes.json").write_text(json.dumps(frozen, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(compiler, originals)
        if len(all_classes) != 4:
            raise RuntimeError("Owned private context class inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        providers = {"Lfixture/Carrier;", "Lfixture/Context;", "Lfixture/Trace;"}
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
                    for mode in range(14):
                        output = v.run([compiler.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), str(operation), str(mode)], f"{label}-{route}-{operation}-{mode}")
                        if len(output.splitlines()) != 2 or any(not line.startswith(f"route={route},operation={operation},mode={mode},attempt={i},trace=") for i, line in enumerate(output.splitlines())):
                            raise RuntimeError("Frozen private context observer output changed")
                        results.append(output)
            return results

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").write_text(json.dumps(baseline, indent=2) + "\n")
        wrong = [
            ("skip-callback", "T value = callback.read(context.input);", "T value = null;", 0),
            ("repeat-callback", "T value = callback.read(context.input);", "callback.read(context.input); T value = callback.read(context.input);", 0),
            ("wrong-input", "callback.read(context.input)", "callback.read(null)", 0),
            ("skip-cleanup", "Trace.finish();", ";", 0),
            ("repeat-cleanup", "Trace.finish();", "Trace.finish(); Trace.finish();", 0),
            ("wrapped-failure", "Trace.finish();", "try { Trace.finish(); } catch(java.io.IOException e) { throw new java.io.IOException(e); }", 9),
            ("early-cast", "Trace.hit(4);", "String ignored = (String)value; Trace.hit(4);", 2),
            ("reread-result", "Item value = read(context, callback);", "Item value = read(context, callback); value = (Item)Trace.result;", 12),
            ("wrong-receiver", "Trace.receiver = this;", "Trace.receiver = null;", 0),
        ]
        for label, before, after, mode in wrong:
            original_text = (fixture / "java/fixture/Core.java").read_text()
            if before not in original_text:
                raise RuntimeError("Owned wrong-program anchor changed")
            source = work / label / "Core.java"
            source.parent.mkdir()
            source.write_text(original_text.replace(before, after))
            compiled = work / (label + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            p = subprocess.run([compiler.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), name, "1", "0", str(mode)], capture_output=True, text=True, timeout=1200, env=v.environment)
            (work / (label + "-counterexample.log")).write_text(p.stdout + p.stderr)
            if p.returncode == 0 or "owned private context throws mismatch" not in p.stderr:
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
            raise RuntimeError("D8 changed original private context definitions")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        if args.baseline_neverd:
            red = subprocess.run([args.baseline_neverd.resolve(), "mobile", dex, *cp, "--timeout", "600", "-o", work / "before"], text=True, capture_output=True, env=v.environment, timeout=1200)
            (work / "before.log").write_text(red.stdout + red.stderr)
            if red.returncode == 0 or "generic invocation requires a proven erased member binding" not in red.stdout + red.stderr:
                raise RuntimeError("Expected the prior CLI's private checked-throws rejection")
        output = work / "recovered"
        v.run([args.neverd.resolve(), "mobile", dex, *cp, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        release = coverage.get("minimum_java_release", 8)
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("field_source_bindings")
                or coverage.get("requires_java_rebuild_postprocessing") or release != 8):
            raise RuntimeError("Private context recovery changed original definitions or generic metadata")
        declarations = {m for owner in classes for m, row in facts[owner]["methods"].items() if row["access"] & 0x500}
        if ({m["identity"] for m in coverage["methods"] if m["status"] == "declaration-only"} != declarations
                or coverage["declaration_only_method_count"] != len(declarations)
                or any(m["status"] not in ("recovered", "declaration-only") for m in coverage["methods"])
                or any(m["instruction_count"] for m in coverage["methods"] if m["status"] == "declaration-only")):
            raise RuntimeError("Private context Code/declaration roles changed")
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        allowed = {o + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt = work / "rebuilt"
        deps = [provider] if args.dependency_provider else []
        compiler.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=release, classpath=os.pathsep.join(map(str, deps)) if deps else None)
        actual_classes, actual_methods = compiler_inventory(compiler, rebuilt)
        if actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed or any(actual_methods[m] != all_methods[m] for m in methods):
            raise RuntimeError("Private context recovery invented or omitted definitions/roles")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(facts[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if actual != wanted or any(after_exceptions[m] != exceptions[m] for m in facts[owner]["methods"]):
                raise RuntimeError("Private context original metadata/Exceptions changed: " + owner)
        if execute((rebuilt, harness, *deps), "rebuilt") != baseline:
            raise RuntimeError("Private context result, effects or exception identity changed")
        signature = "<T:Ljava/lang/Object;>(Lfixture/Context;Lfixture/Carrier<TT;>;)TT;"
        controls = [
            ("unbound-formal", signature.replace("Carrier<TT;>", "Carrier<TUnknown;>")),
            ("extra-formal", signature.replace("<T:", "<Unused:Ljava/lang/Object;T:")),
            ("wrong-arity", signature.replace("Carrier<TT;>", "Carrier<TT;TT;>")),
            ("covariant-carrier", signature.replace("Carrier<TT;>", "Carrier<+TT;>")),
            ("erasure-mismatch", signature.replace("Lfixture/Context;", "[Lfixture/Context;")),
            ("generic-throws", signature + "^TT;"),
        ]
        for label, changed in controls:
            root = work / (label + "-originals")
            shutil.copytree(originals, root)
            target = root / "fixture/Core.class"
            code_before = method_code(target.read_bytes())
            receipt = change_signature(target, signature.encode(), changed.encode())
            if method_code(target.read_bytes()) != code_before:
                raise RuntimeError("Signature control changed original Code")
            receipt["target_methods"] = ["read", "readStatic"]
            receipt["Code"] = "unchanged"
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            counterdex = work / (label + "-dex")
            counterdex.mkdir()
            counterpaths = [root / p.relative_to(originals) for p in paths]
            v.run([*v.d8, "--release", "--min-api", "24", *cp, "--output", counterdex, *counterpaths], label + "-d8")
            rejected = subprocess.run([args.neverd.resolve(), "mobile", counterdex / "classes.dex", *cp, "--timeout", "600", "-o", work / (label + "-recovered")], text=True, capture_output=True, timeout=1200, env=v.environment)
            (work / (label + "-reject.log")).write_text(rejected.stdout + rejected.stderr)
            if rejected.returncode == 0 or "error" not in rejected.stdout + rejected.stderr:
                raise RuntimeError("Accepted malformed/unproved private context Signature: " + label)
        if frozen != {"originals": hashes(originals), "harness": hashes(harness), "sdk": sha(sdk), "cli": sha(args.neverd)} or sha(dex) != dex_hash or sha(provider) != provider_hash:
            raise RuntimeError("Frozen private context input changed")
        summary = {"status": "verified", "classes": len(classes), "methods": len(methods),
                   "all_java_units": len(list((output / "sources").rglob("*.java"))),
                   "fresh_jvm_cases": 112, "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "signature_controls": len(controls), "omit_caller_throws": args.omit_caller_throws,
                   "dependency_provider": args.dependency_provider}
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
