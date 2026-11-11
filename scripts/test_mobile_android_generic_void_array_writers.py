#!/usr/bin/env python3
"""Verify generic void array writer calls preserve actual array identities and partial stores."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--scalar", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-generic-void-array-writers-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "generic_void_array_writers/java"
        harness_sources = [FIXTURES / "harness" / (name + ".java") for name in
                           ("GenericVoidArrayWritersHarness", "GenericVoidArrayWritersCaller")]
        if args.scalar:
            authored = work / "scalar-source"
            shutil.copytree(fixture, authored)
            provider = authored / "fixture/Provider.java"
            text = provider.read_text()
            if text.count("public static <T> void fill") != 1 or text.count("Trace.hit(3);") != 1:
                raise RuntimeError("Original scalar array producer anchors changed")
            provider.write_text(text.replace("public static <T> void fill", "public static <T> int fill")
                               .replace("Trace.hit(3);", "Trace.hit(3);\n        return values.length * 31 + begin * 7 + end;"))
            core = authored / "fixture/Core.java"
            text = core.read_text()
            if text.count("Provider.fill(values, value, begin, end);") != 1:
                raise RuntimeError("Original scalar array result anchor changed")
            core.write_text(text.replace("Provider.fill(values, value, begin, end);", "Trace.code = Provider.fill(values, value, begin, end);"))
            trace = authored / "fixture/Trace.java"
            trace.write_text(trace.read_text().replace("public static int fail;", "public static int fail, code;"))
            fixture = authored
            oracle = work / "scalar-harness"
            oracle.mkdir()
            for original in harness_sources:
                text = original.read_text()
                if original.name.endswith("Harness.java"):
                    text = text.replace("Trace.events = 0;", "Trace.events = 0; Trace.code = 45678;")
                    anchor = "require(Trace.result == (returned ? values : marker));"
                    if text.count(anchor) != 1:
                        raise RuntimeError("Original scalar array observer anchor changed")
                    text = text.replace(anchor, anchor + "\n            require(Trace.code == (returned ? values.length * 31 + begin * 7 + end : 45678));")
                (oracle / original.name).write_text(text)
            harness_sources = sorted(oracle.glob("*.java"))
        provider_name = "Provider"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local(harness_sources, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != 7:
            raise RuntimeError("Owned generic void array writer inventory changed")
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
                for mode in range(13):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "GenericVoidArrayWritersHarness", str(route), str(mode)], f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Original generic void array writer matrix changed: " + output)
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
            raise RuntimeError("Owned generic void array writer fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original generic void array writer runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-transfer-disassembly")
        result_type = "I" if args.scalar else "V"
        pattern = r"invoke-static[^\n]*Lfixture/Provider;.fill:\(\[Ljava/lang/Object;Ljava/lang/Object;II\)" + result_type
        if len(re.findall(pattern, disassembly)) != 1:
            raise RuntimeError("D8 changed the original generic array operation call")
        identity = "Lfixture/Provider;->fill([Ljava/lang/Object;Ljava/lang/Object;II)" + result_type
        if records["Lfixture/Provider;"]["methods"][identity]["signature"] != "<T:Ljava/lang/Object;>([TT;TT;II)" + result_type:
            raise RuntimeError("Compiler changed the complete array formal relation")
        if args.scalar and "move-result" not in disassembly:
            raise RuntimeError("D8 lost original scalar array result use")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count", 0)
                or coverage["status"] != "recovered" or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Generic void array writer recovery changed original inventory or contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=8, classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Generic void array writer recovery invented or omitted a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original writer class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after["methods"].get(identity) != metadata or actual_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Original writer method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Generic void array writer changed values, ordered effects, aliases or failure identity")
        mutations = (
            ("copied-array", "Provider", "java.util.Arrays.fill(values, begin, end, value);", "java.util.Arrays.fill(values.clone(), begin, end, value);", "0"),
            ("wrong-value", "Provider", "java.util.Arrays.fill(values, begin, end, value);", "java.util.Arrays.fill(values, begin, end, null);", "0"),
            ("wrong-range", "Provider", "java.util.Arrays.fill(values, begin, end, value);", "java.util.Arrays.fill(values, begin + 1, end, value);", "0"),
            ("early-value-cast", "Provider", "java.util.Arrays.fill(values, begin, end, value);", "java.util.Arrays.fill(values, begin, end, String.class.cast(value));", "11"),
            ("wrong-return-identity", "Core", "return values;", "return new Object[]{value};", "0"),
            ("lost-callback", "Provider", "Trace.hit(3);", "", "0"),
        )
        if args.scalar:
            mutations += (("wrong-scalar-result", "Provider", "return values.length * 31 + begin * 7 + end;", "return 0;", "0"),)
        for label, owner, old, new, mode in mutations:
            text = (fixture / ("fixture/" + owner + ".java")).read_text()
            if text.count(old) != 1:
                raise RuntimeError("Independent transfer counterexample anchor changed")
            wrong = work / label / ("fixture/" + owner + ".java")
            wrong.parent.mkdir(parents=True)
            wrong.write_text(text.replace(old, new))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                    "GenericVoidArrayWritersHarness", "1", mode], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned generic void array writer contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_provider": args.dependency_provider, "scalar": args.scalar, "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "original_inventory_metadata_and_exceptions": "unchanged",
                    "array_element_and_returned_array_identity": "matched",
                    "null_range_covariant_store_and_original_failure_identity": "matched",
                    "independent_compiler_counterexamples": len(mutations), "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
