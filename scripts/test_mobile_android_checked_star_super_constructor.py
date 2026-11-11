#!/usr/bin/env python3
"""Verify checked reifiable superclass arguments and reused incoming capture slots."""
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
    if error.name != "scripts": raise
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
    args = parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-checked-star-super-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "checked_star_super_constructor"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=25)
        verify.compile_local(sorted((fixture / "harness").glob("*.java")), harness, classpath=originals, release=25)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 7 or len(all_methods) != 12:
            raise RuntimeError("Original checked star constructor inventory changed")
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        target = "Lfixture/Base;-><init>(Lfixture/Context$Key;)V"
        if records["Lfixture/Base;"]["methods"][target]["signature"] != "(Lfixture/Context$Key<*>;)V":
            raise RuntimeError("Original reifiable constructor contract changed")
        if records["Lfixture/Context$Key;"]["signature"] != "<E::Lfixture/Context$Element;>Ljava/lang/Object;":
            raise RuntimeError("Original star carrier interface bound changed")
        # The dependency declaration reader accepts class versions through 17.
        # This fixture uses Java 25 syntax for bytecode operations supported by
        # older VMs. Follow the existing constructor fixture's version-only
        # encoding, then independently execute both inputs with the same oracle.
        encoded = work / "encoded"
        for original in sorted(originals.rglob("*.class")):
            data = original.read_bytes()
            if data[:4] != b"\xca\xfe\xba\xbe" or int.from_bytes(data[6:8], "big") != 69:
                raise RuntimeError("Original Java 25 class header changed")
            lowered = data[:6] + (52).to_bytes(2, "big") + data[8:]
            destination = encoded / original.relative_to(originals)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.open("xb").write(lowered)
            if lowered[:6] != data[:6] or lowered[8:] != data[8:]:
                raise RuntimeError("Compatibility encoding changed more than the version")
        encoded_hashes = hashes(encoded)
        encoded_records = class_identity.compiler_classes(encoded)
        if (set(encoded_records) != set(records) or exception_inventory(encoded) != exceptions or
                any({k: v for k, v in record.items() if k not in {"major", "sha256"}} !=
                    {k: v for k, v in encoded_records[owner].items() if k not in {"major", "sha256"}}
                    for owner, record in records.items())):
            raise RuntimeError("Version-only encoding changed original metadata")
        (work / "version-encoding-hashes.json").open("x").write(json.dumps(encoded_hashes, indent=2) + "\n")
        classes = {"Lfixture/Child;"}
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x") as archive:
            for path in sorted(encoded.rglob("*.class")):
                if path != encoded / "fixture/Child.class":
                    archive.writestr(path.relative_to(encoded).as_posix(), path.read_bytes())
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        dependency_hash, sdk_hash = sha(dependency), sha(args.android_jar)

        def execute(runtime, label):
            outputs = []
            cases = [(variant, 0, 0) for variant in range(8)]
            cases += [(variant, fail, kind) for variant in (0, 2) for fail in range(1, 6) for kind in (0, 1)]
            for route in range(4):
                for variant, fail, kind in cases:
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "CheckedStarHarness", str(route), str(variant), str(fail), str(kind)],
                        f"{label}-{route}-{variant}-{fail}-{kind}")
                    pattern = f"route={route},variant={variant},fail={fail},kind={kind},attempt=[01],events=[0-9]+,mask=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, v) for v in output.splitlines()):
                        raise RuntimeError("Frozen checked star constructor observer changed")
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        if execute((encoded, harness), "encoded") != baseline:
            raise RuntimeError("Version-only encoding changed constructor runtime behavior")
        dex = work / "dex"
        dex.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                    "--output", dex, *sorted(encoded.rglob("*.class"))], "d8-reference")
        source_dex = dex / "classes.dex"
        dex_hash = sha(source_dex)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != all_classes or
                {r["identity"] for r in inventory["methods"]} != set(all_methods)):
            raise RuntimeError("D8 changed an original checked star constructor definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        source_input = fixture / "smali/Child.smali"
        source_hash = sha(source_input)
        (work / "input-method-witness.json").open("x").write(json.dumps({
            "input_method_bytecode": "authored smali with checked star super argument and incoming capture reuse",
            "sha256": source_hash, "reference_dex_sha256": dex_hash}, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_input, "--classpath", args.android_jar.resolve(),
                    "--classpath", dependency, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({m["identity"] for m in coverage["methods"]} != methods or coverage["class_count"] != 1 or
                coverage["method_count"] != 4 or coverage["unrecovered_method_count"] or
                coverage.get("projected_method_count", 0) or coverage["status"] != "recovered" or
                coverage.get("class_source_bindings") or coverage.get("generic_signature_projections") or
                coverage.get("minimum_java_release", 8) != 25):
            raise RuntimeError("Checked star constructor changed original definitions or source contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency, release=25)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        expected_helpers = {"Lfixture/Child;->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"}
        helpers = {m["class"] + "->" + m["name"] + m["prototype"] for m in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Checked star constructor invented or omitted a runtime entry")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != after[owner].get(key):
                    raise RuntimeError("Original checked star class metadata changed: " + key)
            for method, metadata in records[owner]["methods"].items():
                if after[owner]["methods"].get(method) != metadata or after_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original checked star method metadata or Exceptions changed: " + method)
        if execute((rebuilt, harness, dependency), "rebuilt") != baseline:
            raise RuntimeError("Checked star constructor changed captures, cast timing, overload, references or failures")
        source = (fixture / "java/fixture/Child.java").read_text()
        mutations = (
            ("late-first", "        this.first = first;\n", "", "        super((Context.Key<?>) key);", "        super((Context.Key<?>) key);\n        this.first = first;"),
            ("late-second", "        this.second = second;\n", "", "        super((Context.Key<?>) key);", "        super((Context.Key<?>) key);\n        this.second = second;"),
            ("null-key", "super((Context.Key<?>) key);", "super((Context.Key<?>) null);"),
            ("wrong-overload", "super((Context.Key<?>) key);", "super((Object) key);"),
            ("lost-second", "this.second = second;", "this.second = first;"),
            ("duplicate-tail", "Trace.step(5);", "Trace.step(5); Trace.step(5);"),
            ("null-first", "this.first = first;", "this.first = null;"),
            ("lost-final", "Trace.seenFinal = this.key;", "Trace.seenFinal = null;"),
        )
        for label, *changes in mutations:
            text = source
            for before, changed in zip(changes[::2], changes[1::2]):
                if text.count(before) != 1: raise RuntimeError("Owned checked star counterexample anchor changed")
                text = text.replace(before, changed, 1)
            wrong = work / label / "Child.java"
            wrong.parent.mkdir()
            wrong.open("x").write(text)
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=25)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                "CheckedStarHarness", "1", "0", "0", "0"], text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned checked star constructor contract" not in result.stderr:
                raise RuntimeError("Frozen observer missed checked star constructor change: " + label)
        if ({"originals": hashes(originals), "harness": hashes(harness)} != frozen or hashes(encoded) != encoded_hashes or
                sha(source_input) != source_hash or sha(source_dex) != dex_hash or
                sha(dependency) != dependency_hash or sha(args.android_jar) != sdk_hash):
            raise RuntimeError("Original checked star inputs, SDK or frozen observers changed")
        evidence = {"status": "success", "class_count": 1, "method_count": 4, "fresh_jvm_cases": len(baseline),
                    "attempts_per_case": 2, "independent_wrong_programs": len(mutations), "minimum_java_release": 25,
                    "original_definitions_metadata_and_exceptions": "unchanged",
                    "original_inputs_and_frozen_observers": "unchanged",
                    "captures_cast_timing_overload_reference_identity_and_exact_failures": "matched"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__": raise SystemExit(main())
