#!/usr/bin/env python3
"""Verify strings after deferred reads retain allocation, initialization and effect order."""
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "java-home", "compiler-java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-deferred-field-string-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "deferred_field_string"
        originals, harness = work / "originals", work / "harness"
        name = "fixture.DeferredFieldStringHarness"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        sdk_hash, cli_hash = sha(sdk), sha(args.neverd)
        compiler.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=8)
        compiler.compile_local([FIXTURES / "harness" / (n + ".java") for n in ("DeferredFieldStringHarness", "DeferredFieldStringCaller")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (4, 9):
            raise RuntimeError("Owned deferred field string inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        providers = {"Lfixture/Target;"}
        excluded = providers if args.dependency_provider else set()
        classes = all_classes - excluded
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        dependency = work / "owned-provider.jar"

        def write_provider(destination, source):
            with zipfile.ZipFile(destination, "x", compression=zipfile.ZIP_DEFLATED) as jar:
                for owner in sorted(providers):
                    entry = owner[1:-1] + ".class"
                    jar.writestr(entry, (source / entry).read_bytes())

        write_provider(dependency, originals)
        dependency_hash = sha(dependency)

        def execute(runtime, method):
            outputs = []
            for route in range(4):
                for branch in range(2):
                    for mode in range(16):
                        text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), str(branch), str(mode)],
                                          f"{method}-{route}-{branch}-{mode}")
                        if text != "attempts=2,deferred-field-string=matched\n":
                            raise RuntimeError("Frozen deferred field string observer changed")
                        outputs.append(text)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        texts = {owner: (fixture / ("java/fixture/" + owner + ".java")).read_text() for owner in ("Core", "Owner", "Target")}
        static_call = 'new Target("owned allocation label", null, false, Owner.shared)'
        instance_call = 'new Target("owned allocation label", null, false, owner.value)'
        wrong = [
            ("early-static-read", "Core", "return " + static_call + ";", 'Object snapshot = Owner.shared; return new Target("owned allocation label", null, false, snapshot);', 1, 0),
            ("early-instance-read", "Core", "Target value = " + instance_call + ";", 'Object snapshot = owner.value; Target value = new Target("owned allocation label", null, false, snapshot);', 0, 0),
            ("lost-constructor", "Core", "return " + static_call + ";", "return null;", 1, 0),
            ("repeated-constructor", "Core", "return " + static_call + ";", static_call + "; return " + static_call + ";", 1, 0),
            ("changed-label-identity", "Core", static_call, static_call.replace('"owned allocation label"', 'new String("owned allocation label")'), 1, 0),
            ("changed-marker", "Core", static_call, static_call.replace(", null,", ", new Object(),"), 1, 0),
            ("changed-flag", "Core", static_call, static_call.replace(", false,", ", true,"), 1, 0),
            ("changed-payload", "Core", static_call, static_call.replace("Owner.shared", "new Object()"), 1, 0),
            ("late-field-reread", "Core", "return " + static_call + ";", "Target target = " + static_call + "; target.value = Owner.shared; return target;", 1, 0),
            ("wrapped-constructor", "Core", "return " + static_call + ";", "try { return " + static_call + "; } catch(Throwable failure) { throw new IllegalStateException(failure); }", 1, 5),
            ("lost-callback", "Target", "Trace.hit(2, this);", ";", 1, 0),
            ("lost-result", "Core", "return " + static_call + ";", static_call + "; return null;", 1, 0),
            ("lost-volatile", "Owner", "public static volatile Object shared;", "public static Object shared;", 1, 0),
            ("lost-owner-continuation", "Core", "Trace.lastOwner = owner;", ";", 0, 0),
        ]
        for label, owner, anchor, replacement, shape, mode in wrong:
            text = texts[owner]
            if text.count(anchor) != 1:
                raise RuntimeError("Deferred field string counterexample anchor changed: " + label)
            source = work / label / (owner + ".java")
            source.parent.mkdir()
            source.open("x").write(text.replace(anchor, replacement))
            compiled = work / (label + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), name, "0", str(shape), str(mode)],
                                    text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned deferred field string mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program: " + label)
        classpath = ["--classpath", sdk]
        if excluded:
            classpath += ["--classpath", dependency]
        dexdir = work / "dex"
        dexdir.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--release", "--min-api", "24", *classpath, "--output", dexdir, *paths], "d8-original")
        dex = dexdir / "classes.dex"
        dex_hash = sha(dex)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original deferred field string definitions")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        code_dump = verify.run([verify.dexdump, "-d", dex], "encoded-order")
        witnessed = {}
        for block in re.split(r"(?m)^Class #", code_dump)[1:]:
            if "Class descriptor  : 'Lfixture/Core;'" not in block:
                continue
            for method in re.split(r"(?m)^    #[0-9]+", block)[1:]:
                named = re.search(r"name\s+: '([^']+)'", method)
                if not named or named[1] not in {"fromInstance", "fromStatic"}:
                    continue
                ops = [(m[1],m[2]) for m in re.finditer(r"(?m)^.*\|[0-9a-f]+: ([a-z0-9/-]+)\s*(.*)$", method)]
                new = next(i for i,(op,ref) in enumerate(ops) if op == "new-instance" and "Lfixture/Target;" in ref)
                field = next(i for i,(op,ref) in enumerate(ops) if op in {"iget-object", "sget-object"} and "Lfixture/Owner;." in ref)
                string = next(i for i,(op,ref) in enumerate(ops) if op in {"const-string", "const-string/jumbo"})
                init = next(i for i,(op,ref) in enumerate(ops) if op.startswith("invoke-direct") and "Lfixture/Target;.<init>" in ref)
                if not new < field < string < init:
                    raise RuntimeError("D8 changed the independently authored field/string allocation order")
                field_register = re.match(r"v([0-9]+)", ops[field][1])[1]
                string_register = re.match(r"v([0-9]+)", ops[string][1])[1]
                arguments = re.findall(r"v([0-9]+)", ops[init][1].split("}",1)[0])
                if arguments[1] != string_register or arguments[-1] != field_register or arguments.count(field_register) != 1 or arguments.count(string_register) != 1:
                    raise RuntimeError("D8 changed the reversed argument consumption witness")
                witnessed[named[1]] = {"ordered_producers": [new,field,string,init], "field_register":field_register, "string_register":string_register, "constructor_arguments":arguments}
        if set(witnessed) != {"fromInstance", "fromStatic"}:
            raise RuntimeError("Missing independently encoded allocation order witness")
        (work / "encoded-order.json").open("x").write(json.dumps(witnessed, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        minimum_java_release = coverage.get("minimum_java_release", 8)
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("field_source_bindings") or coverage.get("requires_java_rebuild_postprocessing") or minimum_java_release != 8):
            raise RuntimeError("Deferred field string recovery changed definitions or generic metadata")
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        allowed = {o + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt = work / "rebuilt"
        dependencies = [dependency] if excluded else []
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=minimum_java_release,
                             classpath=os.pathsep.join(map(str, dependencies)) if dependencies else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if (actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed
                or any(actual_methods[m] != all_methods[m] for m in methods)):
            raise RuntimeError("Deferred field string recovery lost or invented runtime definitions")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(facts[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if wanted.pop("major") != 52 or actual.pop("major") != 44 + minimum_java_release:
                raise RuntimeError("Deferred field string compiler release changed")
            if actual != wanted or any(after_exceptions[m] != exceptions[m] for m in facts[owner]["methods"]):
                raise RuntimeError("Deferred field string metadata/roles/Exceptions changed: " + owner)
        if execute((rebuilt, harness, *dependencies), "rebuilt") != baseline:
            raise RuntimeError("Deferred field string allocation, initialization, field snapshot, continuation or exception order changed")
        controls = []
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(sdk) != sdk_hash
                or sha(dependency) != dependency_hash or sha(dex) != dex_hash or sha(args.neverd) != cli_hash):
            raise RuntimeError("Frozen original program, observer, SDK or CLI changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "same_code_counterexamples": [r[0] for r in controls], "dependency_provider": args.dependency_provider,
                   "metadata_roles_exceptions_and_runtime_definitions": "unchanged", "minimum_java_release": minimum_java_release}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
