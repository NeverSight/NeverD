#!/usr/bin/env python3
"""Verify scalar interface constructor arguments retain allocation and initialization effects."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from scripts.test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
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
    from test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-interface-constructor-arguments-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "interface_constructor_arguments"
        originals, harness = work / "originals", work / "harness"
        name = "InterfaceConstructorArgumentsHarness"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        sdk_hash, cli_hash = sha(sdk), sha(args.neverd)
        compiler.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=8)
        compiler.compile_local([FIXTURES / "harness" / (name + ".java")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (3, 6):
            raise RuntimeError("Owned interface constructor arguments inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        identity = "Lfixture/Tile;-><init>(ZIJ)V"
        if facts["Lfixture/Tile;"]["methods"][identity]["signature"] is not None:
            raise RuntimeError("Original scalar constructor unexpectedly gained a Signature")
        providers = all_classes - {"Lfixture/Api;"}
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
                for mode in range(16):
                    text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), str(mode)],
                                      f"{method}-{route}-{mode}")
                    if text != "attempts=2,interface-constructor-arguments=matched\n":
                        raise RuntimeError("Frozen interface constructor arguments observer changed")
                    outputs.append(text)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        texts = {p.name: p.read_text() for p in (fixture / "java").rglob("*.java")}
        wrong = [('lost-first', 'Api.java', 'Z_FIRST = new Tile(true, -17, 0x0123456789ABCDEFL);', 'Z_FIRST = null;', 0, 0), ('lost-last', 'Api.java', 'B_FLOAT = new Tile(-0.0f, Double.NaN);', 'B_FLOAT = null;', 0, 0), ('changed-flag', 'Api.java', 'new Tile(true, -17,', 'new Tile(false, -17,', 0, 0), ('changed-number', 'Api.java', 'new Tile(true, -17,', 'new Tile(true, 17,', 0, 0), ('changed-wide', 'Api.java', '0x0123456789ABCDEFL', '0L', 0, 0), ('changed-zero-sign', 'Api.java', 'new Tile(-0.0f,', 'new Tile(0.0f,', 0, 0), ('changed-nan', 'Api.java', 'Double.NaN', 'Double.POSITIVE_INFINITY', 0, 0), ('aliased-second', 'Api.java', 'A_SECOND = new Tile(false, Integer.MIN_VALUE, Long.MIN_VALUE);', 'A_SECOND = Z_FIRST;', 0, 0), ('lost-escape', 'Tile.java', 'Trace.escaped = this;', 'Trace.escaped = null;', 0, 0), ('lost-constructor-count', 'Tile.java', 'Trace.constructors++;', ';', 0, 0), ('lost-class-event', 'Tile.java', 'Trace.events = Trace.events * 10 + 1;', ';', 0, 0), ('lost-callback', 'Tile.java', 'Trace.action.run();', ';', 0, 0), ('repeat-callback', 'Tile.java', 'Trace.action.run();', 'Trace.action.run(); Trace.action.run();', 0, 0), ('early-completion', 'Tile.java', 'Trace.stage = 10 + Trace.constructors;', 'Trace.completed++; Trace.stage = 10 + Trace.constructors;', 0, 0), ('early-instance-write', 'Tile.java', 'Trace.flag = flag;', 'this.flag = flag; Trace.flag = flag;', 0, 0), ('wrong-number-store', 'Tile.java', 'this.number = number;', 'this.number = 0;', 0, 0), ('wrong-wide-store', 'Tile.java', 'this.ticks = ticks;', 'this.ticks = 0;', 0, 0), ('lost-runtime-failure', 'Tile.java', 'Trace.action.run();', 'try { Trace.action.run(); } catch (RuntimeException ignored) {}', 0, 3), ('replaced-error', 'Tile.java', 'Trace.action.run();', 'try { Trace.action.run(); } catch (Error caught) { throw new AssertionError(caught); }', 0, 4), ('overwritten-callback-mark', 'Tile.java', 'this.ticks = ticks;', 'this.ticks = ticks; this.mark = null;', 0, 11)]
        for method, filename, anchor, replacement, route, mode in wrong:
            text = texts[filename]
            if anchor not in text:
                raise RuntimeError("Interface constructor arguments counterexample anchor changed: " + method)
            source = work / method / filename
            source.parent.mkdir()
            if method == "early-instance-write":
                text = text.replace("        this.flag = flag;\n", "")
            source.open("x").write(text.replace(anchor, replacement))
            compiled = work / (method + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), name, str(route), str(mode)],
                                    text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (method + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned interface constructor arguments mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program: " + method)
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
            raise RuntimeError("D8 changed original interface constructor arguments definitions")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Interface constructor arguments recovery changed definitions or generic metadata")
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        allowed = {o + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt = work / "rebuilt"
        dependencies = [dependency] if excluded else []
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=8,
                             classpath=os.pathsep.join(map(str, dependencies)) if dependencies else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if (actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed
                or any(actual_methods[m] != all_methods[m] for m in methods)):
            raise RuntimeError("Interface constructor arguments recovery lost or invented runtime definitions")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(facts[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if actual != wanted or any(after_exceptions[m] != exceptions[m] for m in facts[owner]["methods"]):
                raise RuntimeError("Interface constructor arguments metadata/roles/Exceptions changed: " + owner)
        if execute((rebuilt, harness, *dependencies), "rebuilt") != baseline:
            raise RuntimeError("Interface constructor arguments identity, original casts, arrays or effects changed")
        controls = [
            ("private-constructor", "flags", 0x2),
            ("synthetic-constructor", "flags", 0x1001),
            ("variable-arity-constructor", "flags", 0x81),
            ("abstract-allocation-type", "class", 0x421),
            ("object-formal-constructor", "signature", "<T:Ljava/lang/Object;>(ZIJ)V"),
            ("bounded-formal-constructor", "signature", "<T:Ljava/lang/Number;>(ZIJ)V"),
            ("throwing-formal-constructor", "signature", "<E:Ljava/lang/Exception;>(ZIJ)V^TE;"),
            ("two-formal-constructor", "signature", "<T:Ljava/lang/Object;U:Ljava/lang/Object;>(ZIJ)V"),
        ]
        for method, kind, changed in controls:
            negative = work / (method + "-classes")
            shutil.copytree(originals, negative)
            target = negative / "fixture/Tile.class"
            original_code = method_code(target.read_bytes())
            if kind == "flags":
                receipt = change_method_flags(target, b"(ZIJ)V", 1, changed, name=b"<init>")
            elif kind == "signature":
                receipt = add_parameter_signature(target, changed, method_name="<init>", prototype="(ZIJ)V")
            else:
                data = target.read_bytes()
                offset = class_records(data)["pool_end"]
                if int.from_bytes(data[offset:offset + 2], "big") != 0x21:
                    raise RuntimeError("Original allocation class flags changed")
                target.write_bytes(data[:offset] + changed.to_bytes(2, "big") + data[offset + 2:])
                receipt = {"original_flags": 0x21, "changed_flags": changed, "code_and_descriptors": "unchanged"}
            if method_code(target.read_bytes()) != original_code:
                raise RuntimeError("Signature-only control changed Code or runtime descriptors")
            (work / (method + "-metadata.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            declared = classpath
            if excluded:
                changed_dependency = work / (method + "-provider.jar")
                write_provider(changed_dependency, negative)
                declared = ["--classpath", sdk, "--classpath", changed_dependency]
            encoded = work / (method + "-dex")
            encoded.mkdir()
            verify.run([*verify.d8, "--release", "--min-api", "24", *declared, "--output", encoded,
                        *[negative / p.relative_to(originals) for p in paths]], "d8-" + method)
            rejected = work / (method + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(encoded / "classes.dex"),
                                     *map(str, declared), "-o", str(rejected)], text=True, capture_output=True,
                                    timeout=1200, env=verify.environment)
            (work / (method + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode != 1 or rejected.exists() or not result.stderr.startswith("error:"):
                raise RuntimeError("Interface constructor arguments proof admitted unsupported metadata: " + method)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(sdk) != sdk_hash
                or sha(dependency) != dependency_hash or sha(dex) != dex_hash or sha(args.neverd) != cli_hash):
            raise RuntimeError("Frozen original program, observer, SDK or CLI changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": 64, "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "same_code_counterexamples": [r[0] for r in controls], "dependency_provider": args.dependency_provider,
                   "metadata_roles_exceptions_and_runtime_definitions": "unchanged", "minimum_java_release": 8}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
