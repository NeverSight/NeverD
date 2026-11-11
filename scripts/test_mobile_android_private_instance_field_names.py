#!/usr/bin/env python3
"""Verify private instance field names retain storage and initialization effects."""
from __future__ import annotations
import argparse
import copy
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
    from scripts.test_mobile_android_nested_return import method_code, class_records
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import method_code, class_records

compiler_facts = class_identity.compiler_classes
OWNER = "Lfixture/Provider;"
MAIN = "PrivateInstanceFieldNamesHarness"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-observer", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-private-instance-fields-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "private_instance_field_names/java"
        authored, originals, harness = work / "authored", work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), authored, release=8)
        verify.compile_local([FIXTURES / "harness" / ("PrivateInstanceFieldNames" + n + ".java") for n in ("Caller", "Harness")],
                             harness, classpath=authored, release=8)
        shutil.copytree(authored, originals)
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        target = originals / "fixture/Provider.class"
        before = target.read_bytes()
        encoded = before
        names = {"anchorValue": "final", "cachedValue": "lambda$-23", "storedBits": "bits-word"}
        for old, new in names.items():
            pattern = b"\x01" + len(old).to_bytes(2, "big") + old.encode()
            if encoded.count(pattern) != 1:
                raise RuntimeError("Original instance field name is not a unique constant-pool entry")
            encoded = encoded.replace(pattern, b"\x01" + len(new).to_bytes(2, "big") + new.encode(), 1)
        if (method_code(before) != method_code(encoded)
                or before[class_records(before)["pool_end"]:] != encoded[class_records(encoded)["pool_end"]:]):
            raise RuntimeError("Instance field spelling changed Code or declaration attributes")
        target.write_bytes(encoded)
        encoding = {"names": names, "before_sha256": hashlib.sha256(before).hexdigest(), "after_sha256": sha(target),
                    "Code_descriptors_flags_attributes": "unchanged"}
        (work / "encoding-witness.json").open("x").write(json.dumps(encoding, indent=2) + "\n")
        frozen = {"authored": hashes(authored), "originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        sdk_hash, cli_hash = sha(args.android_jar), sha(args.neverd)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 4 or len(all_methods) != 12:
            raise RuntimeError("Owned private instance compiler inventory changed")
        classes = all_classes - ({"Lfixture/Trace;"} if args.dependency_observer else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        dependency = work / "owned-observer.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            jar.writestr("fixture/Trace.class", (originals / "fixture/Trace.class").read_bytes())
        dependency_hash = sha(dependency)
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_observer:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            output = []
            for route in range(4):
                for mode in range(20):
                    value = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), MAIN, str(route), str(mode)],
                                       f"{label}-{route}-{mode}")
                    if value != "attempts=2,private-instance-fields=matched\n":
                        raise RuntimeError("Frozen private instance observer output changed")
                    output.append(value)
            return output

        baseline = execute((authored, harness), "authored")
        if execute((originals, harness), "encoded") != baseline:
            raise RuntimeError("Name-only encoding changed original storage behavior")
        (work / "baseline.json").open("x").write(json.dumps(baseline) + "\n")
        mutations = [
            ("anchorValue = anchor;", "anchorValue = null;", 0),
            ("anchorValue = anchor;", "anchorValue = initial;", 0),
            ("cachedValue = initial;", "cachedValue = null;", 0),
            ("storedBits = bits; Trace.hit(4);", "storedBits = 0; Trace.hit(4);", 0),
            ("private Callback<Object,Object> cachedValue;", "private static Callback<Object,Object> cachedValue;", 0),
            ("private volatile long storedBits;", "private static volatile long storedBits;", 0),
            ("return old;", "return next;", 0),
            ("cachedValue = next;", "cachedValue = cachedValue;", 0),
            ("storedBits = bits; Trace.hit(8);", "storedBits = 0; Trace.hit(8);", 0),
            ("storedBits = bits; Trace.hit(8);", "storedBits = (int) bits; Trace.hit(8);", 5),
            ("Trace.hit(7); storedBits = bits;", "storedBits = bits; Trace.hit(7);", 9),
            ("Trace.hit(5); Callback", "Callback", 8),
            ("storedBits = bits; Trace.hit(8);", "Trace.hit(8); storedBits = bits;", 10),
            ("cachedValue.apply(value)", "cachedValue.apply(null)", 0),
            ("return cachedValue.apply(value);", "return null;", 0),
            ("return cachedValue.apply(value);", "return (String) cachedValue.apply(value);", 0),
            ("return anchorValue;", "return new Object();", 0),
            ("return cachedValue;", "return null;", 0),
        ]
        source = (fixture / "fixture/Provider.java").read_text()
        for index, (anchor, replacement, mode) in enumerate(mutations):
            if source.count(anchor) != 1:
                raise RuntimeError("Private instance wrong-program anchor changed")
            changed = work / f"wrong-{index}-source/Provider.java"
            changed.parent.mkdir()
            changed.open("x").write(source.replace(anchor, replacement))
            wrong = work / f"wrong-{index}-classes"
            verify.compile_local([changed], wrong, classpath=authored, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, authored, harness))), MAIN, "1", str(mode)],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / f"wrong-{index}-counterexample.log").open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned private instance fields" not in result.stderr:
                raise RuntimeError("Observer missed independent incorrect instance storage " + str(index))
        dex = work / "dex"
        dex.mkdir()
        inputs = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *inputs], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Private instance DEX set changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {x["descriptor"] for x in inventory["classes"]} != classes or {x["identity"] for x in inventory["methods"]} != methods:
            raise RuntimeError("D8 invented or omitted an original instance storage entry")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        (work / "input-method-witness.json").open("x").write(json.dumps({"dex_sha256": sha(source_dex), "encoding": encoding}) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count", 0) or coverage["status"] != "partial"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Private instance naming changed original definitions or contracts")
        for identity, row in rows.items():
            if row["status"] != ("recovered" if records[row["class"]]["methods"][identity]["code"] else "declaration-only"):
                raise RuntimeError("Original instance storage Code role changed")
        bindings = coverage.get("field_source_bindings", [])
        expected_fields = {OWNER + "->" + n + ":" + t for n, t in (("final", "Ljava/lang/Object;"), ("lambda$-23", "Lfixture/Callback;"), ("bits-word", "J"))}
        if len(bindings) != 3 or {x["original_identity"] for x in bindings} != expected_fields:
            raise RuntimeError("Private instance spelling omitted or invented a field mapping")
        for row in bindings:
            if (row["class"] != OWNER or row["binding_kind"] != "private-instance-field-identifier"
                    or row.get("capture_constructors") or len(row.get("metadata_differences", [])) != 2
                    or row["source_name"] in ("__neverdInstanceField0", "__neverdInstanceField1")
                    or row["source_identity"] != OWNER + "->" + row["source_name"] + ":" + row["type"]):
                raise RuntimeError("Private instance field lacks its exact disclosed mapping")
        (work / "disclosed-metadata-differences.json").open("x").write(json.dumps(bindings, indent=2) + "\n")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_observer else None, release=8)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {x["class"] + "->" + x["name"] + x["prototype"] for x in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes
                            if any(m["code"] for m in records[c]["methods"].values())}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Private instance recovery changed entries/helpers")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        expected = copy.deepcopy(records)
        for row in bindings:
            field = expected[OWNER]["fields"].pop(row["original_identity"])
            field["name"] = row["source_name"]
            expected[OWNER]["fields"][row["source_identity"]] = field
        for c in classes:
            for key in expected[c]:
                if key not in ("sha256", "path", "size", "methods") and after[c].get(key) != expected[c][key]:
                    raise RuntimeError("Instance metadata changed beyond disclosed names: " + c + ":" + key)
            for identity, metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original instance method metadata/Exceptions changed: " + identity)
        for identity in expected_helpers:
            fact = after[identity.split("->", 1)[0]]["methods"][identity]
            if (fact["access"] != 0xA or not fact["code"]
                    or fact["signature"] != "<E:Ljava/lang/Throwable;>(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;^TE;"):
                raise RuntimeError("Private exception helper metadata changed")
        runtime = (rebuilt, harness, dependency) if args.dependency_observer else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Instance spelling changed storage, receivers or exact failures")
        if ({"authored": hashes(authored), "originals": hashes(originals), "harness": hashes(harness)} != frozen
                or sha(args.android_jar) != sdk_hash or sha(args.neverd) != cli_hash or sha(dependency) != dependency_hash
                or sha(source_dex) != json.loads((work / "input-method-witness.json").read_text())["dex_sha256"]):
            raise RuntimeError("Original instance inputs or frozen observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods), "fresh_jvm_cases": len(baseline),
                    "attempts_per_case": 2, "stages": 3, "independent_wrong_programs": len(mutations), "dependency_observer": args.dependency_observer,
                    "all_generated_Java": "compiled", "original_metadata_and_Exceptions": "matched except three disclosed field names",
                    "receivers_final_references_wide_values_partial_effects_exact_failures": "matched", "field_name_bindings": 3,
                    "original_inputs_and_observers": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
