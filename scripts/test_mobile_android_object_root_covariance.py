#!/usr/bin/env python3
"""Verify original Object-root covariant bridges and checked clone contracts."""
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
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory, omit_original_exceptions
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory, omit_original_exceptions

BRIDGES = {"Lfixture/" + name + ";->clone()Ljava/lang/Object;" for name in ("Core", "Broken")}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--omit-original-exceptions", action="store_true")
    parser.add_argument("--narrowed-child", action="store_true")
    parser.add_argument("--sdk-parent", action="store_true")
    args = parser.parse_args()
    if args.sdk_parent and not args.narrowed_child:
        parser.error("--sdk-parent requires --narrowed-child")
    if args.omit_original_exceptions and args.narrowed_child:
        parser.error("the exception encoding and narrowed child profiles are independent")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-object-root-covariance-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "object_root_covariance/java"
        if args.narrowed_child:
            staged = work / "fixture"
            shutil.copytree(fixture, staged)
            fixture = staged
            (fixture / "fixture/Child.java").write_text("""package fixture;
public class Child<T> extends Core<T> {
    public int tag, entries;
    public Object childSeen;
    public CloneNotSupportedException checked;
    public boolean after, nullable;
    public Child<T> clone() throws CloneNotSupportedException {
        entries++;
        childSeen = this;
        if (!after && checked != null) throw checked;
        Child<T> copy = (Child<T>)super.clone();
        if (after && checked != null) throw checked;
        copy.tag = tag + 1;
        if (nullable) return null;
        return copy;
    }
}
""")
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("ObjectRootCovarianceHarness", "ObjectRootCovarianceCaller")], harness, classpath=originals)
        if args.omit_original_exceptions:
            for owner in ("Core", "Broken"):
                receipt = omit_original_exceptions(originals / ("fixture/" + owner + ".class"),
                    method_name=b"clone", typed_descriptor=("()Lfixture/" + owner + ";").encode(),
                    bridge_descriptor=b"()Ljava/lang/Object;",
                    expected_exceptions=["Ljava/lang/CloneNotSupportedException;"])
                (work / (owner + "-original-exceptions-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        classes, methods = compiler_inventory(verify, originals)
        if len(classes) != 4 or len(methods) != (12 if args.narrowed_child else 9):
            raise RuntimeError("Owned original Object covariance inventory changed")
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        bridges = set(BRIDGES)
        if args.narrowed_child:
            bridges.update({"Lfixture/Child;->clone()Lfixture/Core;", "Lfixture/Child;->clone()Ljava/lang/Object;"})
        for identity in bridges:
            if (records[identity.split("->", 1)[0]]["methods"][identity]["access"] != 0x1041
                    or exceptions[identity] != ([] if args.omit_original_exceptions else ["Ljava/lang/CloneNotSupportedException;"])):
                raise RuntimeError("Original compiler did not generate the exact Object bridge: " + identity)
        classpath = ["--classpath", args.android_jar.resolve()]
        sdk = None
        if args.sdk_parent:
            sdk = work / "provider.jar"
            with zipfile.ZipFile(sdk, "w") as archive:
                archive.write(originals / "fixture/Core.class", "fixture/Core.class")
            classpath += ["--classpath", sdk]
            classes = classes - {"Lfixture/Core;"}
            methods = {identity: value for identity, value in methods.items() if identity.split("->")[0] in classes}
            bridges -= {"Lfixture/Core;->clone()Ljava/lang/Object;"}
        provider_hash = hashlib.sha256(sdk.read_bytes()).hexdigest() if sdk else None
        (work / "frozen-input-hashes.json").write_text(json.dumps({"originals": original_hashes,
            "harness": harness_hashes, "android_sdk": sdk_hash, "provider": provider_hash}, indent=2) + "\n")
        runtime_args = ["narrowed-child"] if args.narrowed_child else (["empty-original"] if args.omit_original_exceptions else [])
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                              "ObjectRootCovarianceHarness", *runtime_args], "baseline")
        counts = (52, 181, 111, 48)
        expected = ["values=52,identities=181,states=111,failures=48",
                "object-root-and-generic-result-metadata=retained,original-checked-exceptions-and-shallow-clone-aliases=matched,external-subclass-dispatch=matched"]
        if args.narrowed_child:
            expected.append("narrowed-child-slots-and-checked-exception-identity=matched,cases=80")
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned Object covariance matrix changed: " + baseline)
        wrong_programs = 0
        if args.narrowed_child:
            child = (fixture / "fixture/Child.java").read_text()
            for label, old, new in (
                ("lost-child-call", "entries++;", ";"),
                ("repeat-child-call", "entries++;", "entries += 2;"),
                ("wrong-backref", "childSeen = this;", "childSeen = null;"),
                ("lost-parent-call", "Child<T> copy = (Child<T>)super.clone();", "Child<T> copy = this;"),
                ("lost-early-checked", "if (!after && checked != null) throw checked;", ";"),
                ("lost-late-checked", "if (after && checked != null) throw checked;", ";"),
                ("changed-checked-object", "if (after && checked != null) throw checked;", "if (after && checked != null) throw new CloneNotSupportedException();"),
                ("lost-post-copy-write", "copy.tag = tag + 1;", ";"),
                ("wrong-post-copy-value", "copy.tag = tag + 1;", "copy.tag = tag;"),
                ("lost-null-result", "if (nullable) return null;", ";")):
                if child.count(old) != 1:
                    raise RuntimeError("Wrong-program source anchor changed: " + label)
                source = work / ("wrong-" + label) / "fixture/Child.java"
                source.parent.mkdir(parents=True)
                source.write_text(child.replace(old, new))
                compiled = source.parent.parent / "classes"
                verify.compile_local([source], compiled, classpath=originals)
                result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                    "ObjectRootCovarianceHarness", "narrowed-child"], text=True, capture_output=True, env=verify.environment, timeout=300)
                (source.parent.parent / "execution.log").write_text(result.stdout + result.stderr)
                if result.returncode == 0 or "AssertionError" not in result.stderr:
                    raise RuntimeError("Frozen observer missed wrong program: " + label)
                wrong_programs += 1

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned Object covariance fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original Object covariance runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-bridge-disassembly")
        witnesses = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            prototype = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            identity = block[1] + "->" + name[1] + prototype[1] if name and prototype else ""
            if identity not in bridges:
                continue
            flags = re.search(r"(?m)^\s+access\s+: 0x([0-9a-fA-F]+)", block[2])
            ops = [op.removesuffix("/range") for op in re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])]
            if not flags or int(flags[1], 16) != 0x1041 or ops != ["invoke-virtual", "move-result-object", "return-object"]:
                raise RuntimeError("D8 changed the original Object covariance bridge: " + identity)
            witnesses.add(identity)
        if witnesses != bridges:
            raise RuntimeError("Missing original Object covariance bridge witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count") != len(bridges)
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != bridges
                or coverage["status"] != "partial" or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Object covariance recovery changed runtime entries or original contracts")
        for identity in bridges:
            owner = identity.split("->", 1)[0]
            dispatch = ("Lfixture/Core;->clone()" + identity.split("clone()", 1)[1]
                        if owner == "Lfixture/Child;" else "Ljava/lang/Object;->clone()Ljava/lang/Object;")
            if (rows[identity].get("projection_kind") != "generic-superclass-bridge"
                    or rows[identity].get("bridge_target") != owner + "->clone()" + owner
                    or rows[identity].get("bridge_dispatch") != dispatch):
                raise RuntimeError("Object covariance bridge lost its original declaring contract")
            if args.omit_original_exceptions and (
                    rows[identity].get("source_bridge_declared_throws") != ["Ljava/lang/CloneNotSupportedException;"]
                    or rows[identity].get("bridge_exception_source") != "Ljava/lang/Object;->clone()Ljava/lang/Object;"
                    or not rows[identity].get("metadata_differences")):
                raise RuntimeError("Regenerated Object bridge exception difference was not reported")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8), classpath=sdk)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {"Lfixture/" + name + ";->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for name in ("Core", "Child", "Broken", "Caller") if "Lfixture/" + name + ";" in classes}
        if actual_classes != classes or set(actual_methods) != set(methods) | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Object covariance recovery invented a runtime entry or unexpected helper")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original Object covariance class metadata changed: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted_exceptions = (["Ljava/lang/CloneNotSupportedException;"]
                    if args.omit_original_exceptions and method in bridges else exceptions[method])
                if after["methods"][method] != metadata or actual_exceptions[method] != wanted_exceptions:
                    raise RuntimeError("Original Object covariance method metadata or Exceptions changed: " + method)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, *([sdk] if sdk else [])))),
                             "ObjectRootCovarianceHarness", *(["empty-rebuilt"] if args.omit_original_exceptions else runtime_args)], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Object covariance changed clone identity, checked failures or external overrides")
        if args.narrowed_child:
            for label, cp in (("original-repeat", (originals, harness)),
                              ("rebuilt-repeat", (rebuilt, harness, *([sdk] if sdk else [])))):
                if verify.run([verify.java, "-cp", os.pathsep.join(map(str, cp)), "ObjectRootCovarianceHarness", *runtime_args], label) != baseline:
                    raise RuntimeError("Repeated fresh JVM changed the original bridge behavior")
        negative = work / "missing-synthetic-originals"
        shutil.copytree(originals, negative)
        receipt = change_method_flags(negative / ("fixture/Child.class" if args.narrowed_child else "fixture/Core.class"), b"()Ljava/lang/Object;", 0x1041, 0x41, name=b"clone")
        (work / "missing-synthetic-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = dex(negative, work / "missing-synthetic-dex")
        rejected = work / "missing-synthetic-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath),
                                 "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "missing-synthetic-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "bridge" not in result.stderr:
            raise RuntimeError("Object covariance accepted an unproved original bridge")
        invalid = work / "javac-negative/fixture/Core.java"
        invalid.parent.mkdir(parents=True)
        text = (fixture / "fixture/Core.java").read_text()
        assert text.count("throws CloneNotSupportedException") == 1
        invalid.write_text(text.replace("throws CloneNotSupportedException", "throws Exception"))
        result = subprocess.run([str(verify.javac), "--release", "8", "-proc:none", "-d",
                                 str(work / "javac-negative-classes"), str(invalid)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "javac-negative.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "does not throw" not in result.stderr:
            raise RuntimeError("Independent javac accepted a widened Object override exception contract")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or (sdk and hashlib.sha256(sdk.read_bytes()).hexdigest() != provider_hash)):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "values": counts[0], "identities": counts[1], "states": counts[2], "failures": counts[3],
                    "omit_original_exceptions": args.omit_original_exceptions,
                    "narrowed_child": args.narrowed_child, "sdk_parent": args.sdk_parent,
                    "narrowed_child_cases": 80 if args.narrowed_child else 0,
                    "wrong_programs": wrong_programs,
                    "original_class_and_method_metadata_and_exceptions": (
                        "only reported regenerated bridge Exceptions differ" if args.omit_original_exceptions else "unchanged"),
                    "shallow_clone_and_external_overrides": "matched",
                    "same_code_counterexample": "rejected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        (work / "summary.json").write_text(json.dumps(dict(evidence, status="verified"), indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
