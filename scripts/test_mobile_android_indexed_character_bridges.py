#!/usr/bin/env python3
"""Verify original final indexed character bridges through immutable JVM callers."""
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
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--synthetic", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-indexed-character-bridges-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "indexed_character_bridges/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/IndexedCharacterBridgesHarness.java"], harness, classpath=originals)
        flags = 0x51 | (0x1000 if args.synthetic else 0)
        receipt = bridge_flag(originals / "fixture/Text.class", "charAt", "(I)C", input_flags=flags)
        (work / "original-bridge-flags.json").write_text(json.dumps(receipt, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 2 or len(all_methods) != 9:
            raise RuntimeError("Owned character input inventory changed")
        classes = all_classes - ({"Lfixture/Indexed;"} if args.dependency_provider else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        if any(row["signature"] for row in records.values()):
            raise RuntimeError("Indexed character input acquired a class Signature")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        provider = work / "owned-indexed-provider.jar"
        with zipfile.ZipFile(provider, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("fixture/Indexed.class", (originals / "fixture/Indexed.class").read_bytes())
        provider_hash = hashlib.sha256(provider.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", provider]
        def execute(paths, label, mode):
            output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, paths)),
                "IndexedCharacterBridgesHarness", mode, "synthetic" if args.synthetic else "bridge"], label)
            if output.splitlines() != ["calls=260,failures=100,unsigned-char-and-original-virtual-forwarding=matched"]:
                raise RuntimeError("Original character forwarding matrix changed: " + output)
            return output
        def make_dex(source, label):
            output = work / label
            output.mkdir()
            paths = [p for p in sorted(source.rglob("*.class")) if "L" + p.relative_to(source).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", output, *paths], "d8-" + label)
            if list(output.glob("classes*.dex")) != [output / "classes.dex"]:
                raise RuntimeError("Indexed character fixture changed DEX inventory")
            return output / "classes.dex"
        baseline = execute((originals, harness), "baseline", "original")
        dex = make_dex(originals, "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed original character entries")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", dex], "original-disassembly")
        witnesses = 0
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            if block[1] != "Lfixture/Text;" or not re.search(r"(?m)^\s+name\s+: 'charAt'", block[2]):
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            if (ops not in [["invoke-virtual", "move-result", "return"], ["invoke-virtual/range", "move-result", "return"]]
                    or "Lfixture/Text;.get:(I)C" not in block[2]):
                raise RuntimeError("Original indexed forwarding Code changed")
            witnesses += 1
        if witnesses != 1:
            raise RuntimeError("Missing original character forwarding witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if (coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or {row["identity"] for row in coverage["methods"]} != methods
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial"
                or coverage["projected_method_count"] != 2 or coverage.get("generic_signature_projections")):
            raise RuntimeError("Indexed character recovery lost inventory or projection disclosure")
        bridge = "Lfixture/Text;->charAt(I)C"
        row = next(row for row in coverage["methods"] if row["identity"] == bridge)
        if (row.get("bridge_target") != "Lfixture/Text;->get(I)C"
                or row.get("projection_kind") != "plain-interface-forwarding-bridge" or not row.get("metadata_differences")):
            raise RuntimeError("Character forwarding lacks its exact original body receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
            release=coverage.get("minimum_java_release", 8), classpath=provider if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != methods | helpers:
            raise RuntimeError("Character source invented or lost runtime entries")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after[owner].get(key) != records[owner].get(key):
                    raise RuntimeError("Original character class metadata changed: " + key)
            for identity, metadata in records[owner]["methods"].items():
                expected = dict(metadata)
                if identity == bridge:
                    expected["access"] &= ~0x1040
                if after[owner]["methods"].get(identity) != expected or after_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Character method changed beyond reported bridge flags")
        paths = (rebuilt, harness, provider) if args.dependency_provider else (rebuilt, harness)
        if execute(paths, "rebuilt", "rebuilt") != baseline:
            raise RuntimeError("Unsigned character values or original dispatch/effects changed")
        negative = work / "method-formal-originals"
        shutil.copytree(originals, negative)
        receipt = add_parameter_signature(negative / "fixture/Text.class", "<T:Ljava/lang/Object;>(I)C", method_name="get", prototype="(I)C")
        (work / "method-formal-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        changed = make_dex(negative, "method-formal-dex")
        rejected = work / "method-formal-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(changed), *map(str, classpath), "-o", str(rejected)],
            text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "method-formal-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Character bridge admitted unproved target formals")
        wrong = work / "wrong-width-java/fixture/Text.java"
        wrong.parent.mkdir(parents=True)
        content = (fixture / "fixture/Text.java").read_text()
        if content.count("return value;") != 1:
            raise RuntimeError("Owned character width anchor changed")
        wrong.write_text(content.replace("return value;", "return (char)(byte)value;"))
        compiled = work / "wrong-width-classes"
        verify.compile_local([wrong], compiled, classpath=originals)
        bridge_flag(compiled / "fixture/Text.class", "charAt", "(I)C", input_flags=flags)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
            "IndexedCharacterBridgesHarness", "original", "synthetic" if args.synthetic else "bridge"],
            text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-width-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "indexed character bridge contract" not in result.stderr:
            raise RuntimeError("Independent observer missed changed character width")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(provider.read_bytes()).hexdigest() != provider_hash):
            raise RuntimeError("Original inputs, immutable observer or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
            "runtime_calls": 260, "expected_failures": 100, "dependency_provider": args.dependency_provider,
            "synthetic": args.synthetic, "coverage": "partial", "unsigned_character_values": "matched",
            "original_receiver_arguments_effects_and_failures": "matched", "virtual_target_override": "matched",
            "full_metadata_and_inventory": "unchanged except reported character bridge flags and helpers",
            "same_code_formal_counterexample": "rejected", "wrong_width_counterexample": "detected",
            "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
