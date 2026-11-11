#!/usr/bin/env python3
"""Verify generic array transfer calls preserve actual array identities and partial stores."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import shutil
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


def element_seed_sources(work: Path, fixture: Path) -> tuple[Path, list[Path]]:
    """Author the variant before javac/D8; compiler outputs stay immutable."""
    def replace(text, before, after, count=1):
        if text.count(before) != count:
            raise RuntimeError("Owned array element source anchor changed")
        return text.replace(before, after)

    authored = work / "element-source"
    authored.mkdir()
    for original in sorted(fixture.rglob("*.java")):
        text = original.read_text()
        if original.name == "Provider.java":
            text = replace(text, "T[] destination, int offset", "T[] destination, T seed, int offset")
            text = replace(text, "Trace.destination = destination; Trace.hit(2);",
                           "Trace.destination = destination; Trace.seed = seed; Trace.hit(2);")
            text = replace(text, "        Trace.hit(3);", "        destination[offset] = seed;\n        Trace.hit(3);")
        elif original.name == "Core.java":
            text = replace(text, "Object[] destination, int offset", "Object[] destination, Object seed, int offset")
            text = replace(text, "source, destination, offset, begin, end", "source, destination, seed, offset, begin, end")
        elif original.name == "Trace.java":
            text = replace(text, "Object source, destination, result;", "Object source, destination, result, seed;")
        else:
            raise RuntimeError("Owned array element fixture inventory changed")
        target = authored / original.relative_to(fixture)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.open("x").write(text)
    observers = work / "element-observers"
    observers.mkdir()
    sources = []
    for name in ("GenericArrayTransfersHarness", "GenericArrayTransfersCaller"):
        text = (FIXTURES / "harness" / (name + ".java")).read_text()
        text = replace(text, "Object[] destination, int offset", "Object[] destination, Object seed, int offset")
        text = replace(text, "source, destination, offset, begin, end", "source, destination, seed, offset, begin, end",
                       5 if name.endswith("Harness") else 1)
        if name.endswith("Harness"):
            text = replace(text, "Object[].class, Object[].class, int.class",
                           "Object[].class, Object[].class, Object.class, int.class", 2)
            text = replace(text, "mode == 10 ? source", "(mode == 10 || mode == 15) ? source")
            text = replace(text, "mode == 11 ? new String[]", "(mode == 11 || mode == 13 || mode == 14) ? new String[]")
            text = replace(text, "mode == 12 ? 0 : 3", "mode == 12 ? 0 : mode == 15 ? 2 : 3")
            text = replace(text, "        for (int attempt", "        Object seed = mode == 13 ? marker : mode == 14 ? null : mode == 15 ? source : new String(\"seed\");\n        for (int attempt")
            text = replace(text, "Trace.result = marker;", "Trace.result = marker; Trace.seed = marker;")
            text = replace(text, "try { System.arraycopy(source, begin, expected, offset, end - begin); }",
                           "try { System.arraycopy(source, begin, expected, offset, end - begin); expected[offset] = seed; }")
            text = replace(text, "else if (mode == 3 || mode == 4) System.arraycopy(source, begin, expected, offset, end - begin);",
                           "else if (mode == 3 || mode == 4) { System.arraycopy(source, begin, expected, offset, end - begin); expected[offset] = seed; }")
            text = replace(text, "&& Trace.destination == (body ? destination : marker));",
                           "&& Trace.destination == (body ? destination : marker) && Trace.seed == (body ? seed : marker));")
        target = observers / (name + ".java")
        target.open("x").write(text)
        sources.append(target)
    return authored, sources


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--package-provider", action="store_true")
    parser.add_argument("--element-seed", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-generic-array-transfers-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "generic_array_transfers/java"
        if args.package_provider:
            authored = work / "package-source"
            shutil.copytree(fixture, authored)
            source = authored / "fixture/Provider.java"
            text = source.read_text()
            if text.count("public class Provider") != 1 or text.count("public static <T> T[] move") != 1:
                raise RuntimeError("Original package array transfer source anchors changed")
            source.write_text(text.replace("public class Provider", "class Provider").replace("public static <T> T[] move", "static <T> T[] move"))
            fixture = authored
        observers = [FIXTURES / "harness" / (name + ".java") for name in
                     ("GenericArrayTransfersHarness", "GenericArrayTransfersCaller")]
        if args.element_seed:
            fixture, observers = element_seed_sources(work, fixture)
        provider_name = "Trace" if args.package_provider else "Provider"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local(observers, harness,
                             classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != 7:
            raise RuntimeError("Owned generic array transfer inventory changed")
        classes = all_classes - ({"Lfixture/" + provider_name + ";"} if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x") as archive:
            archive.writestr("fixture/" + provider_name + ".class", (originals / ("fixture/" + provider_name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            outputs = []
            for route in range(4):
                for mode in range(16 if args.element_seed else 13):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "GenericArrayTransfersHarness", str(route), str(mode)], f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Original generic array transfer matrix changed: " + output)
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        dex_dir = work / "dex"
        dex_dir.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex_dir, *paths], "d8-original")
        source_dex = dex_dir / "classes.dex"
        if list(dex_dir.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned generic array transfer fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original generic array transfer runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-transfer-disassembly")
        prototype = "([Ljava/lang/Object;[Ljava/lang/Object;" + ("Ljava/lang/Object;" if args.element_seed else "") + "III)[Ljava/lang/Object;"
        if len(re.findall(r"invoke-static[^\n]*Lfixture/Provider;.move:" + re.escape(prototype), disassembly)) != 1:
            raise RuntimeError("D8 changed the original array transfer call")
        identity = "Lfixture/Provider;->move" + prototype
        signature = "<T:Ljava/lang/Object;>([TT;[TT;" + ("TT;" if args.element_seed else "") + "III)[TT;"
        if records["Lfixture/Provider;"]["methods"][identity]["signature"] != signature:
            raise RuntimeError("Compiler changed the complete array formal relation")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count", 0)
                or coverage["status"] != "recovered" or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Generic array transfer recovery changed original inventory or contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=8, classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Generic array transfer recovery invented or omitted a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original payload class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after["methods"].get(identity) != metadata or actual_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Original payload method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Generic array transfer changed values, ordered effects, aliases or failure identity")
        call = "Provider.move(source, destination, " + ("seed, " if args.element_seed else "") + "offset, begin, end)"
        copied = "Provider.move(source, destination.clone(), " + ("seed, " if args.element_seed else "") + "offset, begin, end)"
        wrong_offset = "Provider.move(source, destination, " + ("seed, " if args.element_seed else "") + "0, begin, end)"
        mutations = (
            ("wrong-result-array", "Provider", "return destination;", "return source;"),
            ("skipped-copy", "Provider", "System.arraycopy(source, begin, destination, offset, end - begin);", ""),
            ("copied-destination", "Core", call, copied),
            ("wrong-offset", "Core", call, wrong_offset),
        )
        if args.element_seed:
            mutations += (
                ("lost-element-store", "Provider", "destination[offset] = seed;", ""),
                ("changed-element-store", "Provider", "destination[offset] = seed;", "destination[offset] = null;"),
                ("changed-element-input", "Core", call, call.replace("seed, ", "new Object(), ")),
            )
        for label, owner, old, new in mutations:
            text = (fixture / ("fixture/" + owner + ".java")).read_text()
            if text.count(old) != 1:
                raise RuntimeError("Independent transfer counterexample anchor changed")
            wrong = work / label / ("fixture/" + owner + ".java")
            wrong.parent.mkdir(parents=True)
            wrong.write_text(text.replace(old, new))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                    "GenericArrayTransfersHarness", "1", "0"], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned generic array transfer contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_provider": args.dependency_provider, "package_provider": args.package_provider,
                    "element_seed": args.element_seed, "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "original_inventory_metadata_and_exceptions": "unchanged",
                    "source_destination_and_returned_array_identity": "matched",
                    "overlap_covariance_partial_stores_and_failure_identity": "matched",
                    "independent_compiler_counterexamples": len(mutations), "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
