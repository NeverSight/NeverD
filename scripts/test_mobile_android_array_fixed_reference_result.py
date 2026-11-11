#!/usr/bin/env python3
"""Verify formal array inputs retain independent fixed reference-array results."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile
try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature, method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature, method_code

MAIN = "ArrayFixedReferenceResultHarness"
SIGNATURE = b"<T:Ljava/lang/Object;>([TT;Z)[Ljava/lang/Object;"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-array-fixed-reference-result-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "array_fixed_reference_result/java"
        sources = sorted(fixture.rglob("*.java"))
        observers = [FIXTURES / "harness" / ("ArrayFixedReferenceResult" + name + ".java") for name in ("Caller", "Harness")]
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        source_hashes = {str(path): sha(path) for path in [*sources, *observers]}
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sources, originals, release=8)
        verify.compile_local(observers, harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness), "sources": source_hashes}
        (work / "frozen-input-hashes.json").write_text(json.dumps(frozen, indent=2) + "\n")
        sdk_hash, cli_hash = sha(args.android_jar), sha(args.neverd)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 4 or len(all_methods) != 11:
            raise RuntimeError("Original fixed reference array inventory changed")
        target = originals / "fixture/Provider.class"
        if target.read_bytes().count(SIGNATURE) != 1:
            raise RuntimeError("Original fixed reference array Signature changed")
        external = {"Lfixture/Provider;", "Lfixture/Facade;"} if args.dependency_provider else set()
        classes = all_classes - external
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for name in ("Provider", "Facade"):
                path = "fixture/" + name + ".class"
                jar.writestr(path, (originals / path).read_bytes())
        dependency_hash = sha(dependency)
        classpath = ["--classpath", args.android_jar.resolve()]
        if external:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            outputs = []
            for route in range(4):
                for mode in range(22):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), MAIN, str(route), str(mode)],
                                        f"{label}-{route}-{mode}")
                    if output != "attempts=2,operations=3,fixed-reference-array=matched\n":
                        raise RuntimeError("Frozen fixed reference array observer changed")
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").write_text(json.dumps(baseline, indent=2) + "\n")
        mutations = [
            ("Core", "Facade.adapt(values, reuse)", "Facade.adapt(values == null ? null : values.clone(), reuse)", 0),
            ("Core", "Facade.adapt(values, reuse)", "Facade.adapt(values, !reuse)", 0),
            ("Core", "Trace.hit(4);", "", 0),
            ("Core", "snapshot(); Trace.hit(5);", "Trace.hit(5);", 0),
            ("Core", "return (String[]) snapshot();", "snapshot(); return null;", 0),
            ("Provider", "Trace.initializations++;", "", 0),
            ("Provider", "Trace.calls++;", "Trace.calls += 2;", 0),
            ("Provider", "Trace.action.run();", "Trace.action.run(); Trace.action.run();", 0),
            ("Provider", "Trace.action.run();", "", 10),
            ("Provider", "Trace.action.run();", "try { Trace.action.run(); } catch (RuntimeException failure) { throw new IllegalStateException(failure); }", 10),
            ("Provider", "return chosen;", "return chosen == null ? null : chosen.clone();", 0),
            ("Provider", "java.util.Arrays.copyOf(values, values.length, Object[].class)", "java.util.Arrays.copyOf(values, values.length)", 2),
            ("Provider", "values.length, Object[].class", "values.length - 1, Object[].class", 1),
            ("Provider", "reuse && values.getClass() == Object[].class", "reuse", 2),
            ("Provider", "chosen = Trace.result;", "chosen = null;", 6),
            ("Provider", "Trace.action.run();", "boolean saved = Trace.override; Trace.action.run(); Trace.override = saved;", 17),
        ]
        for index, (owner, anchor, replacement, mode) in enumerate(mutations):
            source = (fixture / ("fixture/" + owner + ".java")).read_text()
            if source.count(anchor) != 1:
                raise RuntimeError("Independent fixed array wrong-program anchor changed")
            path = work / (f"wrong-{index}-source/" + owner + ".java")
            path.parent.mkdir()
            path.write_text(source.replace(anchor, replacement, 1))
            compiled = work / f"wrong-{index}-classes"
            verify.compile_local([path], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), MAIN, "1", str(mode)],
                                    capture_output=True, text=True, timeout=600, env=verify.environment)
            (work / f"wrong-{index}-counterexample.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned fixed reference array result" not in result.stderr:
                raise RuntimeError("Observer missed wrong fixed array program " + str(index))
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(originals.rglob("*.class")) if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8-original")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original fixed array DEX set changed")
        dex_hash = sha(source_dex)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {row["descriptor"] for row in inventory["classes"]} != classes or {row["identity"] for row in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original fixed array definition")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-body-disassembly")
        witnesses = {}
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            proto = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if not name or not proto or (block[1], name[1]) not in (("Lfixture/Core;", "snapshot"), ("Lfixture/Provider;", "adapt")):
                continue
            instructions = re.findall(r"\|[0-9a-fA-F]+:\s+([^\n]+)", block[2])
            if name[1] == "snapshot":
                if sum("Lfixture/Facade;.adapt:([Ljava/lang/Object;Z)[Ljava/lang/Object;" in op for op in instructions) != 1:
                    raise RuntimeError("Original inherited provider call witness changed")
            elif (sum("Ljava/util/Arrays;.copyOf:([Ljava/lang/Object;ILjava/lang/Class;)[Ljava/lang/Object;" in op for op in instructions) != 1
                  or not any("Ljava/lang/Object;.getClass:" in op for op in instructions)):
                raise RuntimeError("Original fixed array provider witness changed")
            witnesses[block[1] + "->" + name[1] + proto[1]] = instructions
        if len(witnesses) != (1 if external else 2):
            raise RuntimeError("Missing original fixed array body witness")
        (work / "input-method-witness.json").write_text(json.dumps({"dex_sha256": dex_hash, "opcodes": witnesses}, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"] or coverage["status"] != "recovered"
                or coverage.get("projected_method_count", 0) or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")):
            raise RuntimeError("Fixed array recovery changed definitions or generic metadata")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if external else None, release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes
                            if any(row["code"] for row in records[c]["methods"].values())}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Fixed array recovery invented or omitted original definitions/helpers")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for c in classes:
            for key, value in records[c].items():
                if key not in ("sha256", "path", "size", "methods") and after[c].get(key) != value:
                    raise RuntimeError("Fixed array class/field metadata changed: " + c + ":" + key)
            for identity, metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Fixed array method/Exceptions metadata changed: " + identity)
        for identity in helpers:
            row = after[identity.split("->", 1)[0]]["methods"][identity]
            if row["access"] != 0xA or not row["code"] or row["signature"] != "<E:Ljava/lang/Throwable;>(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;^TE;":
                raise RuntimeError("Fixed array exception helper changed")
        runtime = (rebuilt, harness, dependency) if external else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Fixed array recovery changed identity, component types, effects or exceptions")
        negatives = [
            SIGNATURE.replace(b"[TT;", b"[TMissing;"),
            SIGNATURE.replace(b"[TT;", b"[Ljava/lang/String;"),
            SIGNATURE.replace(b"<T:Ljava/lang/Object;>", b"<T:Ljava/lang/Number;>"),
            SIGNATURE.replace(b";Z)", b";I)"),
            SIGNATURE.replace(b")[Ljava/lang/Object;", b")[Ljava/lang/String;"),
        ]
        controls = []
        for index, signature in enumerate(negatives):
            negative = work / f"negative-{index}-originals"
            shutil.copytree(originals, negative)
            target = negative / "fixture/Provider.class"
            code = method_code(target.read_bytes())
            receipt = change_signature(target, SIGNATURE, signature)
            if method_code(target.read_bytes()) != code:
                raise RuntimeError("Fixed array Signature control changed original Code")
            cp = classpath
            if external:
                changed_dependency = work / f"negative-{index}-provider.jar"
                with zipfile.ZipFile(changed_dependency, "x") as jar:
                    for name in ("Provider", "Facade"):
                        path = "fixture/" + name + ".class"
                        jar.writestr(path, (negative / path).read_bytes())
                cp = ["--classpath", args.android_jar.resolve(), "--classpath", changed_dependency]
            encoded = work / f"negative-{index}-dex"
            encoded.mkdir()
            log = verify.run([*verify.d8, "--debug", "--min-api", "24", *cp, "--output", encoded,
                              *[negative / path.relative_to(originals) for path in paths]], f"negative-{index}-d8")
            if "Signature is ignored" in log:
                raise RuntimeError("D8 discarded a fixed array negative control")
            rejected = work / f"negative-{index}-rejected"
            result = subprocess.run([args.neverd.resolve(), "mobile", encoded / "classes.dex", *cp, "-o", rejected],
                                    capture_output=True, text=True, timeout=600, env=verify.environment)
            (work / f"negative-{index}-recovery.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Unproved fixed array metadata accepted")
            controls.append(receipt)
        (work / "signature-controls.json").write_text(json.dumps(controls, indent=2) + "\n")
        if (hashes(originals) != frozen["originals"] or hashes(harness) != frozen["harness"]
                or any(sha(Path(path)) != value for path, value in source_hashes.items())
                or sha(args.android_jar) != sdk_hash or sha(args.neverd) != cli_hash or sha(dependency) != dependency_hash or sha(source_dex) != dex_hash):
            raise RuntimeError("Frozen fixed array inputs or observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods), "fresh_jvm_cases": len(baseline),
                    "attempts_per_case": 2, "operations_per_case": 3, "independent_wrong_programs": len(mutations),
                    "same_Code_signature_controls": len(controls), "dependency_provider": args.dependency_provider,
                    "original_classes_methods_fields_Signatures_Exceptions": "unchanged", "array_identity_runtime_components_initialization_effects_and_exact_failures": "matched",
                    "original_inputs_and_frozen_observers": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
