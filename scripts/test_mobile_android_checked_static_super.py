#!/usr/bin/env python3
"""Verify parameter checks before private static superclass callbacks."""
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
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_static_defaults import method_code


def owned_super_opcode(path: Path) -> dict:
    """Build the JVM-valid static invokespecial form javac cannot express."""
    original = path.read_bytes()
    reader = class_identity.ClassFile(original)
    metadata = reader.facts()
    methods = method_code(original)
    identity = b"forward(Lfixture/Host;)V"
    attribute = methods[identity]
    length = int.from_bytes(attribute[10:14], "big")
    code = attribute[14:14 + length]
    if len(code) != 11 or [code[i] for i in (0, 1, 3, 6, 7, 10)] != [0x2a, 0x12, 0xb8, 0x2a, 0xb6, 0xb1]:
        raise RuntimeError("Owned javac callback changed its exact instruction witness")
    owner, name_type = reader.constant(int.from_bytes(code[8:10], "big"), 10)
    name, prototype = reader.constant(name_type, 12)
    if (reader.owner(owner), reader.text(name), reader.text(prototype)) != ("Lfixture/Root;", "ping", "()V"):
        raise RuntimeError("Owned callback no longer references the exact immediate superclass slot")
    if original.count(attribute) != 1:
        raise RuntimeError("Owned callback Code is not unique")
    offset = original.index(attribute) + 14 + 7
    changed = original[:offset] + b"\xb7" + original[offset + 1:]
    expected = dict(methods)
    expected[identity] = attribute[:21] + b"\xb7" + attribute[22:]
    if (method_code(changed) != expected or class_identity.ClassFile(changed).facts() != metadata
            or sum(a != b for a, b in zip(original, changed)) != 1):
        raise RuntimeError("Owned invokespecial construction changed other code or metadata")
    path.write_bytes(changed)
    return {"method": identity.decode(), "single_opcode": "invokevirtual to invokespecial", "other_code_and_metadata": "unchanged"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parent", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-checked-static-super-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted((FIXTURES / "checked_static_super/java").rglob("*.java")), originals, release=8)
        shutil.copytree(originals, work / "virtual-originals")
        receipt = owned_super_opcode(originals / "fixture/Host.class")
        (work / "owned-opcode-witness.json").write_text(json.dumps(receipt, indent=2) + "\n")
        verify.compile_local([FIXTURES / "harness" / (n + ".java") for n in ("CheckedStaticSuperCaller", "CheckedStaticSuperHarness")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (5, 12):
            raise RuntimeError("Owned static-super inventory changed: " + repr((len(all_classes), len(all_methods))))
        providers = {"Lfixture/Base;", "Lfixture/Root;"}
        classes = all_classes - (providers if args.dependency_parent else set())
        methods = {i for i in all_methods if i.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-parent.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_parent:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            result = []
            for variant in range(3):
                for route in range(4):
                    for mode in (0, 1, 2, 4, 6):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), "CheckedStaticSuperHarness",
                            str(variant), str(route), str(mode)], f"{label}-{variant}-{route}-{mode}")
                        if output != f"variant={variant},route={route},mode={mode}:matched\n":
                            raise RuntimeError("Original checked superclass matrix changed: " + output)
                        result.append(output)
            return result

        baseline = execute((originals, harness), "baseline")
        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned checked-super DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed the original static callback inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-super-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] == "Lfixture/Host;" and name and name[1] == "forward":
                if re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2]) != ["const-string", "invoke-static", "invoke-super", "return-void"]:
                    raise RuntimeError("D8 changed the original checked static-super witness")
                witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original checked static-super witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        identity = "Lfixture/Host;->forward(Lfixture/Host;)V"
        rows = {r["identity"]: r for r in coverage["methods"]}
        projected = {i for i,r in rows.items() if r["status"] == "source-projected"}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or projected != {identity} or coverage.get("projected_method_count") != 1
                or coverage["status"] != "partial" or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != 8 or rows[identity].get("projection_kind") != "static-super-accessor"
                or rows[identity].get("source_identity") != identity or len(rows[identity].get("metadata_differences", [])) != 1):
            raise RuntimeError("Checked superclass recovery changed inventory or original contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=8,
                             classpath=dependency if args.dependency_parent else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage["generated_source_helpers"]}
        helper = rows[identity].get("super_dispatch_helper")
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes} | {helper}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Checked superclass recovery invented a runtime entry or omitted helper accounting")
        actual, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        if actual["Lfixture/Host;"]["methods"][helper]["access"] != 2:
            raise RuntimeError("Superclass helper is not private and nonvirtual")
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if records[owner].get(key) != actual[owner].get(key):
                    raise RuntimeError("Original superclass metadata changed: " + owner + ":" + key)
            for method, metadata in records[owner]["methods"].items():
                if actual[owner]["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original callback flags, metadata or exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_parent else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Original parameter-check, null or superclass effect order changed")
        wrong = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (work / "virtual-originals", harness))),
            "CheckedStaticSuperHarness", "1", "1", "0"], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "virtual-dispatch-counterexample.log").write_text(wrong.stdout + wrong.stderr)
        if wrong.returncode == 0 or "AssertionError" not in wrong.stderr:
            raise RuntimeError("Independent observer did not detect virtual dispatch")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs or immutable callers changed")
        evidence = {"status":"success", "class_count":len(classes), "method_count":len(methods), "fresh_jvm_cases":len(baseline),
                    "dependency_parent":args.dependency_parent, "original_metadata_and_exceptions":"unchanged", "coverage":"partial",
                    "parameter_check_null_and_superclass_order":"matched", "original_exception_identity":"matched",
                    "owned_single_opcode_witness":"verified on JVM and D8", "independent_virtual_dispatch_counterexample":"detected",
                    "input_and_oracle_bytes":"unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
