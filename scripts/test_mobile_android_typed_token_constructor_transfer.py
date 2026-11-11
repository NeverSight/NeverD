#!/usr/bin/env python3
"""Verify typed token transfers preserve constructor scope, prefix effects and opaque mapper results."""
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
    parser.add_argument("--allocated-transfer", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-typed-token-constructor-transfer-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        fixture = FIXTURES / "typed_token_constructor_transfer/java"
        originals, harness = work / "originals", work / "harness"
        sources = sorted(fixture.rglob("*.java"))
        if args.allocated_transfer:
            sources.remove(fixture / "fixture/Core.java")
            sources += sorted((fixture.parent / "allocated_transfer").rglob("*.java"))
        verify.compile_local(sources, originals, release=25)
        compiled = work / "compiled-java25"
        shutil.copytree(originals, compiled)
        compatibility = {}
        for path in sorted(originals.rglob("*.class")):
            before = path.read_bytes()
            if before[:4] != b"\xca\xfe\xba\xbe" or int.from_bytes(before[6:8], "big") != 69:
                raise RuntimeError("Authored Java 25 class header changed")
            after = before[:6] + (52).to_bytes(2, "big") + before[8:]
            if after[:6] + after[8:] != before[:6] + before[8:]:
                raise RuntimeError("D8 compatibility header changed original Code or metadata")
            path.write_bytes(after)
            compatibility[path.relative_to(originals).as_posix()] = {
                "compiled_major": 69, "d8_input_major": 52, "header_only": True,
                "compiled_sha256": hashlib.sha256(before).hexdigest(),
                "input_sha256": hashlib.sha256(after).hexdigest()}
        (work / "d8-class-header-compatibility.json").open("x").write(json.dumps(compatibility, indent=2) + "\n")
        harness_name = "TypedTokenConstructorTransferHarness"
        verify.compile_local([FIXTURES / "harness" / (harness_name + ".java")], harness,
                             classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        (work / "baseline-cli-sha256.txt").open("x").write(sha(args.neverd.resolve()) + "\n")
        records = class_identity.compiler_classes(originals)
        exceptions = exception_inventory(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != ((8, 14) if args.allocated_transfer else (7, 12)):
            raise RuntimeError("Owned typed token constructor transfer inventory changed")
        excluded = {"Lfixture/Bound;", "Lfixture/Input;", "Lfixture/Output;", "Lfixture/Context;", "Lfixture/Callback;", "Lfixture/Mapper;"} if args.dependency_inputs else set()
        if excluded and args.allocated_transfer:
            excluded.add("Lfixture/Transfer;")
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
                    stage = 1 if mode in (1, 3) else 2 if mode in (2, 4) else 3 if mode == 5 else 4 if mode in (6, 7, 8) else 5
                    trace = "".join(str(n) for n in range(1, stage + 1))
                    expected = [f"route={route},mode={mode},attempt={attempt},trace={trace}" for attempt in range(2)]
                    if result.splitlines() != expected:
                        raise RuntimeError("Independent seed/result/effect oracle changed: " + result)
                    outputs.append(result)
            return outputs

        baseline = execute((originals, harness), "baseline")
        if execute((compiled, harness), "compiled-baseline") != baseline:
            raise RuntimeError("Header-only D8 compatibility changed the compiled Java 25 baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        paths = [p.relative_to(originals) for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]

        def dex(root, name, declarations=None):
            destination = work / name
            destination.mkdir()
            declared = classpath if declarations is None else declarations
            d8_log = verify.run([*verify.d8, "--debug", "--min-api", "24", *declared, "--output", destination,
                        *[root / p for p in paths]], name)
            if "Signature is ignored" in d8_log:
                raise RuntimeError("D8 dropped a constructor Signature: " + name)
            return destination / "classes.dex"

        source_dex = dex(originals, "dex")
        source_dex_hash = sha(source_dex)
        (work / "source-dex-sha256.txt").open("x").write(source_dex_hash + "\n")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original typed token constructor transfer entry")
        (work / "inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "recovered"
                or coverage.get("projected_method_count", 0) or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Closed constructor lost original entries or generic source contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=sdk if excluded else None, release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes - {"Lfixture/Callback;"}}
        if actual_classes != classes or set(actual_methods) != methods | helpers or helpers != expected_helpers:
            raise RuntimeError("Original typed token transfer entry or generated helper role changed")
        if any(r.get("kind") != "throw-helper" or r.get("static") is not True
               for r in coverage.get("generated_source_helpers", [])):
            raise RuntimeError("Unexpected generated helper role")
        after = class_identity.compiler_classes(rebuilt)
        after_exceptions = exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != after[owner].get(key):
                    raise RuntimeError("Original typed token transfer class metadata changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                if after[owner]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original factory member metadata or Exceptions changed: " + identity)
        versions = {owner: {"original_major": records[owner]["major"],
                            "recovered_major": after[owner]["major"],
                            "original_minor": records[owner]["minor"],
                            "recovered_minor": after[owner]["minor"]} for owner in classes}
        release = coverage.get("minimum_java_release", 8)
        if any(row["original_major"] != 52 or row["recovered_major"] != release + 44
               or row["original_minor"] or row["recovered_minor"] for row in versions.values()):
            raise RuntimeError("Constructor compiler version disclosure changed")
        (work / "classfile-version-disclosure.json").open("x").write(json.dumps(versions, indent=2) + "\n")
        if execute((rebuilt, harness, *([sdk] if excluded else [])), "recovered") != baseline:
            raise RuntimeError("Closed constructor seed, cache, actual result, effects or failure changed")
        signature = b"<X:Ljava/lang/Object;>(Lfixture/Input<TX;>;)Lfixture/Output<TX;>;"
        controls = [
            ("star-result", signature.replace(b"Output<TX;>", b"Output<*>")),
            ("variant-input", signature.replace(b"Input<TX;>", b"Input<+TX;>")),
            ("variant-result", signature.replace(b"Output<TX;>", b"Output<+TX;>")),
            ("other-formal", signature.replace(b"<X:Ljava/lang/Object;>", b"<X:Ljava/lang/Object;Y:Ljava/lang/Object;>").replace(b"Output<TX;>", b"Output<TY;>")),
            ("bounded-mapper", signature.replace(b"<X:Ljava/lang/Object;>", b"<X:Lfixture/Bound;>")),
            ("raw-result", signature.replace(b"Output<TX;>", b"Output")),
            ("declared-throw", signature + b"^Ljava/lang/Exception;"),
            ("unknown-formal", signature.replace(b"Input<TX;>", b"Input<TMissing;>")),
        ]
        controls = [(name, "Mapper", signature, changed) for name, changed in controls]
        if args.allocated_transfer:
            member = b"()Lfixture/Output<TX;>;"
            initializer = b"(Lfixture/Input<TX;>;)V"
            header = b"<X:Ljava/lang/Object;>Ljava/lang/Object;"
            controls = [
                ("star-result", "Transfer", member, member.replace(b"TX;", b"*")),
                ("variant-result", "Transfer", member, member.replace(b"TX;", b"+TX;")),
                ("raw-result", "Transfer", member, member.replace(b"<TX;>", b"")),
                ("declared-throw", "Transfer", member, member + b"^Ljava/lang/Exception;"),
                ("unknown-result-formal", "Transfer", member, member.replace(b"TX;", b"TMissing;")),
                ("variant-input", "Transfer", initializer, initializer.replace(b"TX;", b"+TX;")),
                ("unknown-input-formal", "Transfer", initializer, initializer.replace(b"TX;", b"TMissing;")),
                ("bounded-owner", "Transfer", header, header.replace(b"Ljava/lang/Object;>", b"Lfixture/Bound;>")),
            ]
        for name, provider, signature, changed in controls:
            negative = work / (name + "-classes")
            shutil.copytree(originals, negative)
            path = negative / ("fixture/" + provider + ".class")
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
            annotations = verify.run([verify.dexdump, "-a", source], name + "-annotations")
            if annotations.count("VISIBILITY_SYSTEM Ldalvik/annotation/Signature;") != (5 if excluded else 14 if args.allocated_transfer else 10):
                raise RuntimeError("D8 changed the constructor Signature annotation inventory: " + name)
            rejected = work / (name + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(source), *declared, "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (name + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Unproved complete typed token transfer Signature accepted: " + name)
        wrongs = [("result", "return replacement;", "return null;", 0),
                  ("saved", "saved = input;", "saved = null;", 0),
                  ("duplicate-inspect", "marker = input.inspect();", "input.inspect(); marker = input.inspect();", 0),
                  ("map-label", 'observer.check(input, "map");', 'observer.check(input, "callback");', 0),
                  ("lost-check", "observer.check(value, name);", 'if (name.equals("callback")) observer.check(value, name);', 0),
                  ("duplicate-check", "observer.check(value, name);", "observer.check(value, name); observer.check(value, name);", 0),
                  ("lost-null-check", "if (value == null) throw new NullPointerException(name);", ";", 1),
                  ("early-mapping-failure", "marker = input.inspect();", "if (failure != null) throw failure; marker = input.inspect();", 7)]
        for name, before, changed, mode in wrongs:
            altered = work / (name + "-wrong-source")
            shutil.copytree(fixture, altered)
            path = altered / "fixture/Mapper.java"
            text = path.read_text()
            assert text.count(before) == 1
            path.write_text(text.replace(before, changed))
            wrong = work / (name + "-wrong-classes")
            verify.compile_local([altered / "fixture/Mapper.java"], wrong, classpath=originals, release=25)
            result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (wrong, originals, harness))),
                                     harness_name, "0", str(mode)], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (name + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned typed token constructor transfer" not in result.stderr:
                raise RuntimeError("Frozen caller missed independently compiled wrong factory: " + name)
        if ({"originals": hashes(originals), "harness": hashes(harness)} != frozen
                or sha(source_dex) != source_dex_hash
                or any(sha(Path(p)) != value for p, value in input_hashes.items())):
            raise RuntimeError("Frozen owned factory inputs changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_inputs": args.dependency_inputs, "allocated_transfer": args.allocated_transfer,
                    "fresh_jvm_cases": 64, "attempts_per_case": 2,
                    "same_code_counterexamples": [entry[0] for entry in controls],
                    "independent_compiler_counterexamples": len(wrongs),
                    "minimum_java_release": coverage.get("minimum_java_release", 8)}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
