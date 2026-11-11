#!/usr/bin/env python3
"""Verify fresh carrier allocation keeps scoped constructor binding and effects."""
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
    from scripts.test_mobile_android_nested_return import change_signature
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
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "java-home", "compiler-java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--superclass", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-allocated-nested-carrier-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "allocated_nested_carrier_delegation"
        sources = sorted((fixture / "java").rglob("*.java"))
        if args.superclass:
            sources = [p for p in sources if p.name != "Core.java"]
            sources += sorted((FIXTURES / "allocated_nested_carrier_superclass/java").rglob("*.java"))
        target_owner = "Parent" if args.superclass else "Core"
        originals, harness = work / "originals", work / "harness"
        name = "AllocatedNestedCarrierDelegationHarness"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        sdk_hash, cli_hash = sha(sdk), sha(args.neverd)
        compiler.compile_local(sources, originals, release=8)
        compiler.compile_local([FIXTURES / "harness" / (n + ".java") for n in
                                ("AllocatedNestedCarrierDelegationCaller", name)], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != ((7, 12) if args.superclass else (6, 11)):
            raise RuntimeError("Owned allocated nested carrier inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        # Core references this entry in both source and dependency profiles.
        signature = "(JLjava/util/Map<Lfixture/Wrap<TK;>;Lfixture/Pair<TK;TV;>;>;)V"
        identity = "Lfixture/" + target_owner + ";-><init>(JLjava/util/Map;)V"
        if facts["Lfixture/" + target_owner + ";"]["methods"][identity]["signature"] != signature:
            raise RuntimeError("Original allocated nested carrier Signature changed")
        for caller in ("Lfixture/Core;-><init>(J)V", "Lfixture/Core;-><init>(IJ)V"):
            if facts["Lfixture/Core;"]["methods"][caller]["signature"] is not None:
                raise RuntimeError("Original primitive caller acquired a generic Signature")
        providers = all_classes - {"Lfixture/Core;"}
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

        def execute(runtime, label):
            outputs = []
            for route in range(3):
                for mode in range(16):
                    for wide in range(2):
                        text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), name,
                                           str(route), str(mode), str(wide)], f"{label}-{route}-{mode}-{wide}")
                        if text != "attempts=2,allocated-nested-carrier=matched\n":
                            raise RuntimeError("Frozen allocated nested carrier observer changed")
                        outputs.append(text)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        texts = {p.name: p.read_text() for p in sources}
        wrong = [
            ("lost-allocation", "Core.java", "new Store<Wrap<K>, Pair<K, V>>()", "new HashMap<Wrap<K>, Pair<K, V>>()", 0, 0),
            ("lost-callback", "Store.java", "Trace.action.run();", ";", 0, 0),
            ("repeated-callback", "Store.java", "Trace.action.run();", "Trace.action.run(); Trace.action.run();", 0, 0),
            ("early-value", "Store.java", "Trace.action.run();", "Object saved = Trace.value; Trace.action.run(); Trace.value = saved;", 0, 6),
            ("early-key", "Store.java", "Trace.action.run();", "Object saved = Trace.key; Trace.action.run(); Trace.key = saved;", 0, 9),
            ("lost-content", "Store.java", "((Map) this).put(Trace.key, Trace.value);", ";", 0, 0),
            ("changed-identity", "Core.java", "this.map = map;", "this.map = new HashMap<Wrap<K>, Pair<K, V>>();", 0, 0),
            ("changed-wide", "Core.java", "this.tag = tag;", "this.tag = (int) tag;", 0, 0),
            ("late-tag-read", "Core.java", "this.tag = tag;", "this.tag = Trace.tag;", 0, 9),
            ("lost-target", "Core.java", "Trace.targets++;", ";", 0, 0),
            ("lost-partial", "Base.java", "Trace.partial = this;", "Trace.partial = null;", 0, 0),
            ("lost-stage", "Core.java", "Trace.stage = 4;", "Trace.stage = 2;", 0, 0),
            ("swallowed-exception", "Store.java", "Trace.action.run();", "try { Trace.action.run(); } catch (RuntimeException ignored) {}", 0, 4),
            ("replaced-exception", "Store.java", "Trace.action.run();", "try { Trace.action.run(); } catch (RuntimeException failure) { throw new IllegalArgumentException(); }", 0, 4),
            ("lost-initializer", "Store.java", "Trace.initializations++;", ";", 0, 0),
            ("lost-initializer-failure", "Store.java", "if (Trace.initFailure != null) throw Trace.initFailure;", ";", 0, 11),
            ("lost-element-cast", "Core.java", "return map.get(key);", "return null;", 0, 1),
            ("invented-element-cast", "Core.java", "return ((Map) map).get(key);", "return (Pair) ((Map) map).get(key);", 0, 1),
        ]
        for label, filename, anchor, replacement, route, mode in wrong:
            if args.superclass and filename == "Core.java" and label != "lost-allocation":
                filename = "Parent.java"
            text = texts[filename]
            if anchor not in text:
                raise RuntimeError("Allocated nested carrier counterexample anchor changed: " + label)
            # Repeated source statements are intentionally replaced together.
            source = work / label / filename
            source.parent.mkdir()
            source.open("x").write(text.replace(anchor, replacement))
            compiled = work / (label + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), name, str(route), str(mode), "1"],
                                    text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned allocated nested carrier mismatch" not in result.stderr:
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
            raise RuntimeError("D8 changed original allocated nested carrier definitions")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Allocated nested carrier recovery changed definitions or generic metadata")
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        allowed = {o + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt = work / "rebuilt"
        dependencies = [dependency] if excluded else []
        release = coverage.get("minimum_java_release", 8)
        if release != 25:
            raise RuntimeError("Original allocation prefix requires Java 25")
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=release,
                             classpath=os.pathsep.join(map(str, dependencies)) if dependencies else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if (actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed
                or any(actual_methods[m] != all_methods[m] for m in methods)):
            raise RuntimeError("Allocated nested carrier recovery lost or invented runtime definitions")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(facts[owner]), copy.deepcopy(after[owner])
            if wanted["major"] != 52 or actual["major"] != release + 44:
                raise RuntimeError("Original or rebuilt classfile version changed")
            wanted["major"] = actual["major"]
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if actual != wanted or any(after_exceptions[m] != exceptions[m] for m in facts[owner]["methods"]):
                raise RuntimeError("Allocated nested carrier metadata/roles/Exceptions changed: " + owner)
        if execute((rebuilt, harness, *dependencies), "rebuilt") != baseline:
            raise RuntimeError("Allocated nested carrier identity, original casts, wide arguments or effects changed")
        before = signature.encode()
        unsupported_variance = b"+" if args.superclass else b"-"
        controls = [
            ("method-formal", before, b"<X:Ljava/lang/Object;>" + before, target_owner),
            ("variant-outer", before, before.replace(b"Map<Lfixture/Wrap", b"Map<" + unsupported_variance + b"Lfixture/Wrap"), target_owner),
            ("variant-nested", before, before.replace(b"Wrap<TK;", b"Wrap<" + unsupported_variance + b"TK;"), target_owner),
            ("unknown-formal", before, before.replace(b"Wrap<TK;", b"Wrap<TMissing;"), target_owner),
            ("unknown-carrier", before, before.replace(b"Lfixture/Pair<", b"Lfixture/Missing<"), target_owner),
            ("wrong-nested-arity", before, before.replace(b"Pair<TK;TV;", b"Pair<TK;"), target_owner),
            ("owner-bound", ("<K:Ljava/lang/Object;V:Ljava/lang/Object;>Lfixture/" + ("Parent" if args.superclass else "Base") + "<TK;TV;>;").encode(),
             ("<K:Ljava/lang/Number;V:Ljava/lang/Object;>Lfixture/" + ("Parent" if args.superclass else "Base") + "<TK;TV;>;").encode(), "Core"),
            ("carrier-bound", b"<K:Ljava/lang/Object;>Ljava/lang/Object;",
             b"<K:Ljava/lang/Number;>Ljava/lang/Object;", "Wrap"),
        ]
        for label, before, changed, owner in controls:
            negative = work / (label + "-classes")
            shutil.copytree(originals, negative)
            target = negative / ("fixture/" + owner + ".class")
            original_code = method_code(target.read_bytes())
            receipt = change_signature(target, before, changed)
            if method_code(target.read_bytes()) != original_code:
                raise RuntimeError("Signature-only control changed Code or runtime descriptors")
            (work / (label + "-metadata.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            declared = classpath
            if excluded:
                changed_dependency = work / (label + "-provider.jar")
                write_provider(changed_dependency, negative)
                declared = ["--classpath", sdk, "--classpath", changed_dependency]
            encoded = work / (label + "-dex")
            encoded.mkdir()
            verify.run([*verify.d8, "--release", "--min-api", "24", *declared, "--output", encoded,
                        *[negative / p.relative_to(originals) for p in paths]], "d8-" + label)
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(encoded / "classes.dex"),
                                     *map(str, declared), "-o", str(rejected)], text=True, capture_output=True,
                                    timeout=1200, env=verify.environment)
            (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Allocated nested carrier proof admitted unsupported metadata: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(sdk) != sdk_hash
                or sha(dependency) != dependency_hash or sha(dex) != dex_hash or sha(args.neverd) != cli_hash):
            raise RuntimeError("Frozen original program, observer, SDK or CLI changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": 96, "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "same_code_counterexamples": [r[0] for r in controls], "dependency_provider": args.dependency_provider, "superclass": args.superclass,
                   "metadata_roles_exceptions_and_runtime_definitions": "unchanged", "minimum_java_release": release,
                   "original_classfile_major": 52, "rebuilt_classfile_major": release + 44}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
