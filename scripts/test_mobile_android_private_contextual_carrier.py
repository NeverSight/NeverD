#!/usr/bin/env python3
"""Verify bounded invariant token bindings retain independent nominal results and effects."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-private-contextual-carrier-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        fixture = FIXTURES / "private_contextual_carrier/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        harness_name = "PrivateContextualCarrierHarness"
        verify.compile_local([FIXTURES / "harness" / (harness_name + ".java")], harness,
                             classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        (work / "baseline-cli-sha256.txt").open("x").write(sha(args.neverd.resolve()) + "\n")
        records = class_identity.compiler_classes(originals)
        exceptions = exception_inventory(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (4, 10):
            raise RuntimeError("Owned private contextual carrier inventory changed")
        excluded = {"Lfixture/Context;", "Lfixture/Carrier;", "Lfixture/Callback;"} if args.dependency_inputs else set()
        classes = all_classes - excluded
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        sdk = work / "sdk"
        sdk.mkdir()
        dependencies = []
        if excluded:
            jar = work / "owned-inputs.jar"
            with zipfile.ZipFile(jar, "x") as archive:
                for owner in sorted(excluded):
                    name = owner[1:-1] + ".class"
                    path = sdk / name
                    path.parent.mkdir(exist_ok=True)
                    shutil.copyfile(originals / name, path)
                    archive.writestr(name, path.read_bytes())
            dependencies.append(jar)
        input_hashes = {str(p): sha(p) for p in [args.android_jar.resolve(), *dependencies]}
        classpath = [part for p in [args.android_jar.resolve(), *dependencies]
                     for part in ("--classpath", str(p))]

        def execute(runtime, label):
            outputs = []
            for route in range(4):
                for mode in range(16):
                    result = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                         harness_name, str(route), str(mode)], f"{label}-{route}-{mode}")
                    success = mode not in (1, 2, 3, 4, 5, 7)
                    events = (0 if route == 3 else 3) if mode == 1 else (7 if success else 1) if route == 3 else (414 if success else 16)
                    expected = [f"route={route},mode={mode},attempt={attempt},events={events}" for attempt in range(2)]
                    if result.splitlines() != expected:
                        raise RuntimeError("Independent seed/result/effect oracle changed: " + result)
                    outputs.append(result)
            return outputs

        baseline = execute((originals, harness), "baseline")
        paths = [p.relative_to(originals) for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]

        def dex(root, name, declarations=None):
            destination = work / name
            destination.mkdir()
            declared = classpath if declarations is None else declarations
            verify.run([*verify.d8, "--debug", "--min-api", "24", *declared, "--output", destination,
                        *[root / p for p in paths]], name)
            return destination / "classes.dex"

        source_dex = dex(originals, "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original private contextual carrier entry")
        (work / "inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "recovered"
                or coverage.get("projected_method_count", 0) or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Bounded token nominal factory lost original entries or generic source contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=sdk if excluded else None, release=8)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes - {"Lfixture/Callback;"}}
        if actual_classes != classes or set(actual_methods) != methods | helpers or helpers != expected_helpers:
            raise RuntimeError("Original private contextual carrier factory entry or generated helper role changed")
        if any(r.get("kind") != "throw-helper" or r.get("static") is not True
               for r in coverage.get("generated_source_helpers", [])):
            raise RuntimeError("Unexpected generated helper role")
        after = class_identity.compiler_classes(rebuilt)
        after_exceptions = exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != after[owner].get(key):
                    raise RuntimeError("Original private contextual carrier factory class metadata changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                if after[owner]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original factory member metadata or Exceptions changed: " + identity)
        if execute((rebuilt, harness, *([sdk] if excluded else [])), "recovered") != baseline:
            raise RuntimeError("Bounded token nominal factory seed, cache, actual result, effects or failure changed")
        signature = b"<T:Ljava/lang/Object;>(ILfixture/Context;Lfixture/Callback<TT;>;Lfixture/Carrier<TT;>;)Lfixture/Carrier<TT;>;"
        controls = [
            ("star-callback", signature.replace(b"Callback<TT;>", b"Callback<*>")),
            ("producer-callback", signature.replace(b"Callback<TT;>", b"Callback<+TT;>")),
            ("consumer-callback", signature.replace(b"Callback<TT;>", b"Callback<-TT;>")),
            ("extra-formal", signature.replace(b"<T:Ljava/lang/Object;>", b"<T:Ljava/lang/Object;U:Ljava/lang/Object;>")),
            ("unknown-formal", signature.replace(b"Callback<TT;>", b"Callback<TMissing;>")),
            ("wrong-arity", signature.replace(b"Callback<TT;>", b"Callback<TT;TT;>")),
            ("unknown-bound", signature.replace(b"<T:Ljava/lang/Object;>", b"<T:Lfixture/Missing;>")),
            ("result-erasure", signature.replace(b")Lfixture/Carrier<TT;>;", b")Ljava/lang/Object;")),
        ]
        for name, changed in controls:
            negative = work / (name + "-classes")
            shutil.copytree(originals, negative)
            path = negative / "fixture/Core.class"
            original_code = method_code(path.read_bytes())
            receipt = change_signature(path, signature, changed)
            if method_code(path.read_bytes()) != original_code:
                raise RuntimeError("Signature-only control changed original Code")
            (work / (name + "-metadata.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            declared = classpath
            if excluded:
                changed_jar = work / (name + "-declarations.jar")
                with zipfile.ZipFile(changed_jar, "x") as archive:
                    for owner in sorted(excluded):
                        name_in_jar = owner[1:-1] + ".class"
                        archive.writestr(name_in_jar, (negative / name_in_jar).read_bytes())
                declared = ["--classpath", str(args.android_jar.resolve()), "--classpath", str(changed_jar)]
            source = dex(negative, name + "-dex", declared)
            rejected = work / (name + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(source), *declared, "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (name + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Unproved complete private contextual carrier factory Signature accepted: " + name)
        wrongs = [("result-identity", "        return result;\n    }\n    public static Carrier call", "        return new Carrier<T>(result.value);\n    }\n    public static Carrier call", 0),
                  ("saved-seed", "savedSeed = seed;", "savedSeed = null;", 0),
                  ("lost-context", "marker = context.touch();", "marker = null;", 0),
                  ("duplicate-callback", "Carrier<T> result = callback.create(context, index, seed);", "callback.create(context, index, seed); Carrier<T> result = callback.create(context, index, seed);", 0),
                  ("saved-before-failure", "if (failure != null) throw failure;", "savedResult = result; if (failure != null) throw failure;", 5),
                  ("early-failure", "marker = context.touch();", "if (failure != null) throw failure; marker = context.touch();", 5),
                  ("selected-index", "selected = index;", "selected = 0;", 0),
                  ("wrong-seed", "callback.create(context, index, seed)", "callback.create(context, index, null)", 0)]
        for name, before, changed, mode in wrongs:
            altered = work / (name + "-wrong-source")
            shutil.copytree(fixture, altered)
            path = altered / "fixture/Core.java"
            text = path.read_text()
            assert text.count(before) == 1
            path.write_text(text.replace(before, changed))
            wrong = work / (name + "-wrong-classes")
            verify.compile_local(sorted(altered.rglob("*.java")), wrong, release=8)
            result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (wrong, harness))),
                                     harness_name, "0", str(mode)], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (name + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned private contextual carrier" not in result.stderr:
                raise RuntimeError("Frozen caller missed independently compiled wrong factory: " + name)
        if ({"originals": hashes(originals), "harness": hashes(harness)} != frozen
                or any(sha(Path(p)) != value for p, value in input_hashes.items())):
            raise RuntimeError("Frozen owned factory inputs changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_inputs": args.dependency_inputs, "fresh_jvm_cases": 64, "attempts_per_case": 2,
                    "same_code_counterexamples": [name for name, _ in controls],
                    "independent_compiler_counterexamples": len(wrongs)}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
