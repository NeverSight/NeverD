#!/usr/bin/env python3
"""Compare original array unboxing and superclass delegates with an immutable JVM oracle."""
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
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory

SOURCE = b"Lfixture/Parent<Ljava/lang/Object;Ljava/lang/Object;>;"
INPUT = b"Lfixture/Parent<Lfixture/Carrier;Lfixture/Result;>;"
SLOTS = {
    "write": ("(Lfixture/Token;Ljava/lang/Object;IJ)V", "(Lfixture/Token;Lfixture/Carrier;IJ)V", "(Lfixture/Token;[BIJ)V"),
    "count": ("(Ljava/lang/Object;)I", "(Lfixture/Carrier;)I", "([B)I"),
    "read": ("(Ljava/lang/Object;)Ljava/lang/Object;", "(Lfixture/Carrier;)Lfixture/Result;", "([B)Lfixture/Result;")}

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parent", action="store_true")
    args = parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-array-carrier-superclass-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted((FIXTURES / "array_carrier_superclass/java").rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/ArrayCarrierSuperclassHarness.java", FIXTURES / "harness/ArrayCarrierSuperclassCaller.java"], harness, classpath=originals)
        receipts = [change_signature(originals / "fixture/Core.class", SOURCE, INPUT)]
        for name, (original, _, _) in SLOTS.items():
            receipts.append(bridge_flag(originals / "fixture/Core.class", name, original, 1, 0x1041))
        (work / "original-encoding.json").write_text(json.dumps(receipts, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - ({"Lfixture/Parent;"} if args.dependency_parent else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-parent.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("fixture/Parent.class", (originals / "fixture/Parent.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_parent: classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))), "ArrayCarrierSuperclassHarness"], "baseline")
        expected = ["values=310,identities=408,states=252,failures=198",
                    "original-array-unbox-and-delegates=retained,complete-parent-arguments=retained,typed-entries-and-forwarding-frames=reported"]
        if baseline.splitlines() != expected: raise RuntimeError("Independent case matrix changed: " + baseline)
        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [p for p in sorted(root.rglob("*.class")) if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", destination, *paths], "d8-" + destination.name)
            path = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [path]: raise RuntimeError("Original DEX partition changed")
            return path
        source = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source], "sdk-inventory")
        inventory = parse_dexdump(dump, source.name, dex_header(source.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original class or method inventory")
        disassembly = verify.run([verify.dexdump, "-d", source], "original-unbox-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            proto = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or not proto or name[1] not in SLOTS or proto[1] != SLOTS[name[1]][0]: continue
            calls = re.findall(r"\|[0-9a-fA-F]+:\s+invoke-\S+[^\n]*", block[2])
            if (len(calls) != 2 or "Lfixture/Carrier;.unbox:()[B" not in calls[0]
                    or "Lfixture/Core;." + name[1] + "Value:" + SLOTS[name[1]][2] not in calls[1]
                    or "move-result-object" not in block[2] or "check-cast" not in block[2]):
                raise RuntimeError("D8 changed the original unbox/delegate sequence")
            witnessed.add(name[1])
        if witnessed != set(SLOTS): raise RuntimeError("Incomplete original unboxing witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source, *classpath, "-o", output], "recover")
        report = json.loads((output / "report.json").read_text())
        coverage = report["android_method_recovery"]
        if json.loads((output / "metadata/android-methods.json").read_text()) != coverage: raise RuntimeError("Coverage copies disagree")
        rows = {r["identity"]: r for r in coverage["methods"]}
        bridges = {"Lfixture/Core;->" + name + proto[0] for name, proto in SLOTS.items()}
        typed = {"Lfixture/Core;->" + name + proto[1] for name, proto in SLOTS.items()}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count") != 3
                or coverage["status"] != "partial" or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")
                or {k for k, v in rows.items() if v["status"] == "source-projected"} != bridges):
            raise RuntimeError("Array carrier recovery changed scope or original inventory")
        for name, (original, narrow, target) in SLOTS.items():
            identity = "Lfixture/Core;->" + name + original
            row = rows[identity]
            if (row.get("projection_kind") != "array-carrier-superclass-bridge"
                    or row.get("source_identity") != "Lfixture/Core;->" + name + narrow
                    or row.get("regenerated_bridge") != identity
                    or row.get("bridge_target") != "Lfixture/Core;->" + name + "Value" + target
                    or row.get("unbox_method") != "Lfixture/Carrier;->unbox()[B"
                    or "Lfixture/Parent;->" + name + original not in row.get("checked_inherited_contracts", [])
                    or not row.get("metadata_differences")):
                raise RuntimeError("Array carrier lost its original proof: " + identity)
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_parent else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow" + class_identity.THROW_HELPER_PROTOTYPE for owner in classes} | typed
        if helpers != expected_helpers or actual_classes != classes or set(actual_methods) != methods | expected_helpers:
            raise RuntimeError("Independent rebuilt helper or original inventory differs")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key): raise RuntimeError("Original class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original method metadata changed: " + method)
        for name, (original, narrow, _) in SLOTS.items():
            identity = "Lfixture/Core;->" + name + original
            added = "Lfixture/Core;->" + name + narrow
            wanted = dict(records["Lfixture/Core;"]["methods"][identity]); wanted.update(prototype=narrow, access=1)
            if actual_records["Lfixture/Core;"]["methods"].get(added) != wanted or actual_exceptions.get(added) != exceptions[identity]:
                raise RuntimeError("Unreported typed entry metadata: " + added)
        runtime = (rebuilt, harness, dependency) if args.dependency_parent else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), "ArrayCarrierSuperclassHarness"], "rebuilt-execution")
        if actual != baseline: raise RuntimeError("Array identity, unbox/delegate effects or failure order changed")
        for label in ("wrong-parent-argument", "wrong-parent-result", "missing-synthetic", "private-target"):
            negative = work / (label + "-originals"); shutil.copytree(originals, negative)
            if label == "wrong-parent-argument":
                receipt = change_signature(negative / "fixture/Core.class", INPUT, b"Lfixture/Parent<Ljava/lang/String;Lfixture/Result;>;")
            elif label == "wrong-parent-result":
                receipt = change_signature(negative / "fixture/Core.class", INPUT, b"Lfixture/Parent<Lfixture/Carrier;Lfixture/Token;>;")
            elif label == "missing-synthetic":
                receipt = change_method_flags(negative / "fixture/Core.class", SLOTS["write"][0].encode(), 0x1041, 0x41, name=b"write")
            else:
                receipt = change_method_flags(negative / "fixture/Core.class", SLOTS["write"][2].encode(), 4, 2, name=b"writeValue")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            ndex = dex(negative, work / (label + "-dex")); rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(ndex), *map(str, classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists(): raise RuntimeError("Admitted original contract/body drift: " + label)
        wrong = work / "wrong-java/fixture/Wrong.java"
        wrong.parent.mkdir(parents=True)
        wrong.write_text("package fixture; public final class Wrong extends Parent<Carrier,Result> {\n"
                         " public void write(Token token,String value,int index,long wide) {}\n"
                         " public int count(Carrier value) { return 0; }\n"
                         " public Result read(Carrier value) { return null; } }\n")
        rejected = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-classpath", str(originals),
                                   "-d", str(work / "wrong-compiled"), str(wrong)], text=True, capture_output=True,
                                  timeout=300, env=verify.environment)
        (work / "wrong-javac.log").write_text(rejected.stdout + rejected.stderr)
        if rejected.returncode == 0: raise RuntimeError("Independent javac accepted a different complete parameter contract")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash:
            raise RuntimeError("Frozen input, harness, dependency or SDK changed")
        evidence = {"status": "success", "profile": "dependency-parent" if args.dependency_parent else "source-parent",
                    "class_count": len(classes), "method_count": len(methods), "projected_methods": 3,
                    "value_cases": 310, "identity_cases": 408, "state_cases": 252, "failure_cases": 198,
                    "original_metadata_and_parent_arguments": "unchanged", "new_typed_entries": sorted(typed),
                    "original_unbox_and_delegate_effects": "unchanged", "original_input_harness_dependency_and_sdk_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n"); print(json.dumps(evidence))
    return 0
if __name__ == "__main__": raise SystemExit(main())
