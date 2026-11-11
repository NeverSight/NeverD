#!/usr/bin/env python3
"""Verify protected blank-final stores with exact abrupt constructor cleanup."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--narrow-second", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-protected-self-final-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "protected_self_final/java"
        inputs = []
        for source in sorted(fixture.rglob("*.java")):
            if args.narrow_second and source.name in ("Core.java", "Provider.java"):
                text = source.read_text()
                changes = (("public final long word;", "public final int word;"),) if source.name == "Core.java" else (
                    ("public long second", "public int second"), ("return Trace.word;", "return (int)Trace.word;"))
                for before, after in changes:
                    if text.count(before) != 1:
                        raise RuntimeError("Original final-store width anchor changed")
                    text = text.replace(before, after, 1)
                source = work / "narrow-java/fixture" / source.name
                source.parent.mkdir(parents=True, exist_ok=True)
                source.open("x").write(text)
            inputs.append(source)
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals, release=8)
        verify.compile_local([FIXTURES / "harness" / ("ProtectedSelfFinal" + suffix + ".java")
                              for suffix in ("Caller", "Harness")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != 9:
            raise RuntimeError("Original protected self final inventory changed")
        declarations = {"Lfixture/Provider;"}
        classes = all_classes - (declarations if args.dependency_provider else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        dependency = work / "owned-api.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for descriptor in sorted(declarations):
                path = descriptor[1:-1] + ".class"
                jar.writestr(path, (originals / path).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        (work / "frozen-input-hashes.json").write_text(json.dumps(
            {"originals": original_hashes, "harness": harness_hashes, "sdk": sdk_hash,
             "dependency": dependency_hash}, indent=2) + "\n")

        def execute(runtime: tuple, label: str) -> list[str]:
            outputs = []
            for route in range(4):
                for mode in range(16):
                    result = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "ProtectedSelfFinalHarness", str(route), str(mode), str(args.narrow_second).lower()],
                        f"{label}-{route}-{mode}")
                    pattern = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(result.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in result.splitlines()):
                        raise RuntimeError("Original protected final matrix changed: " + result)
                    outputs.append(result)
            return outputs

        baseline = execute((originals, harness), "baseline")
        mutations = (
            ("skipped-cleanup", "Core", "provider.restore();", "", 0),
            ("early-cleanup", "Core", "provider.restore();", "", 0),
            ("late-first-store", "Core", "this.first=provider.first(input,word);", "Object saved=provider.first(input,word);", 0),
            ("early-word-store", "Core", "this.word=provider.second(input,word);", "provider.second(input,word);", 0),
            ("wrong-prefix", "Core", "this.prefix=input;", "this.prefix=null;", 0),
            ("wrong-word", "Core", "this.word=provider.second(input,word);", "this.word=(short)provider.second(input,word);", 0),
            ("changed-cleanup-exception", "Provider", "Trace.hit(5);", "try{Trace.hit(5);}catch(RuntimeException failure){throw new IllegalArgumentException(failure);}", 5),
            ("duplicate-first-getter", "Core", "this.first=provider.first(input,word);", "provider.first(input,word);this.first=provider.first(input,word);", 0))
        for label, name, before, after, mode in mutations:
            text = next(p for p in inputs if p.name == name + ".java").read_text()
            if text.count(before) != 1:
                raise RuntimeError("Original final-store counterexample anchor changed: " + label)
            text = text.replace(before, after, 1)
            if label == "early-cleanup":
                text = text.replace("        try {", "        provider.restore();\n        try {", 1)
            elif label == "late-first-store":
                text = text.replace("this.word=provider.second(input,word);", "this.word=provider.second(input,word);this.first=saved;", 1)
            elif label == "early-word-store":
                text = text.replace("        try {", "        this.word=" + ("(int)" if args.narrow_second else "") + "word;\n        try {", 1)
            changed = work / label / (name + ".java")
            changed.parent.mkdir()
            changed.open("x").write(text)
            wrong = work / (label + "-classes")
            verify.compile_local([changed], wrong, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness))),
                "ProtectedSelfFinalHarness", "1", str(mode), str(args.narrow_second).lower()],
                text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned protected self final" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect protected finals: " + label)
        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original protected self final DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes
                or {r["identity"] for r in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original protected self final entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "sdk-final-store-disassembly")
        match = re.search(r"Class descriptor\s+: 'Lfixture/Core;'([\s\S]*?)(?=\nClass #|\Z)", disassembly)
        if not match or "name          : '<init>'" not in match[1]:
            raise RuntimeError("Missing original protected constructor body")
        body = match[1].split("name          : '<init>'", 1)[1]
        ranges = [(int(a, 16), int(b, 16)) for a, b in re.findall(
            r"0x([0-9a-f]+) - 0x([0-9a-f]+)\s+<any> ->", body)]
        stores = [(int(pc, 16), name) for pc, name in re.findall(
            r"\|([0-9a-f]+): iput(?:-object|-wide)? [^\n]*Lfixture/Core;\.(first|word):", body)]
        returns = [int(pc, 16) for pc in re.findall(r"\|([0-9a-f]+): return-void", body)]
        handlers = [int(pc, 16) for pc in re.findall(r"\|([0-9a-f]+): move-exception", body)]
        if (len(stores) != 2 or {n for _, n in stores} != {"first", "word"}
                or not all(any(lo <= pc < hi for lo, hi in ranges) for pc, _ in stores)
                or len(returns) != 1 or len(handlers) != 1 or handlers[0] <= returns[0]):
            raise RuntimeError("D8 changed the protected self-store and abrupt-tail witness")
        (work / "protected-final-witness.json").open("x").write(json.dumps({
            "original_code": "unchanged javac/D8 output", "catch_all_ranges": ranges,
            "protected_final_stores": stores, "normal_return": returns[0], "abrupt_handler": handlers[0]}, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Protected self final recovery changed original entries or full generic contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
            classpath=dependency if args.dependency_provider else None, release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for c in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Protected self final invented or omitted a runtime entry")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != after[owner].get(key):
                    raise RuntimeError("Original protected self final class metadata changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                if after[owner]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original protected self final method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Protected self final changed identities, callbacks, cast timing or exact failures")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, SDK or frozen observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
            "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
            "narrow_second": args.narrow_second, "independent_wrong_programs": len(mutations),
            "protected_final_stores_and_abrupt_cleanup": "matched",
            "partial_objects_raw_words_changing_getters_and_failure_identity": "matched",
            "all_original_entries_metadata_and_exceptions": "unchanged", "original_inputs_and_frozen_callers": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
