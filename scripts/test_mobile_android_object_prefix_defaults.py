#!/usr/bin/env python3
"""Verify allocation defaults reuse a proven linear Object-constructor prefix."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import zipfile
try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code

DEFAULTS = {"zeroFlag": ("Z", "false"), "zeroBits": ("J", "0L"), "zeroArray": ("[Ljava/lang/Object;", "null")}
OWNER = "Lfixture/Owner;"
CTOR = OWNER + "-><init>(Lfixture/Fn;)V"
SIGNATURE = "(Lfixture/Fn<-Lfixture/Input;Lfixture/Unit;>;)V"
compiler_facts = class_identity.compiler_classes


def encode_finals(path):
    before = path.read_bytes()
    records = class_records(before)
    strings = records["strings"]
    pos = records["pool_end"] + 6
    pos += 2 + 2 * int.from_bytes(before[pos:pos + 2], "big")
    count = int.from_bytes(before[pos:pos + 2], "big")
    pos += 2
    after, changed = bytearray(before), []
    for _ in range(count):
        flags, name, descriptor, attributes = struct.unpack_from(">HHHH", before, pos)
        field, typ = strings[name].decode(), strings[descriptor].decode()
        if field in DEFAULTS:
            if flags != 2 or typ != DEFAULTS[field][0]:
                raise RuntimeError("Owned private allocation-default field changed")
            struct.pack_into(">H", after, pos, 0x12)
            changed.append(pos + 1)
        pos += 8
        for _ in range(attributes):
            pos += 6 + int.from_bytes(before[pos + 2:pos + 6], "big")
    if (len(changed) != len(DEFAULTS) or len(before) != len(after)
            or {i for i, (a, b) in enumerate(zip(before, after)) if a != b} != set(changed)
            or method_code(before) != method_code(bytes(after))):
        raise RuntimeError("Field encoding changed original Code or another attribute")
    path.write_bytes(after)
    return {"fields": DEFAULTS, "access_before": 2, "access_after": 18,
            "all_Code_and_other_attributes": "unchanged", "changed_bytes": changed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-trace", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-object-prefix-defaults-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        version = verify.run([verify.java, "--version"], "jdk-version")
        major = re.search(r"(?m)^(?:openjdk|java) (\d+)", version)
        if not major or int(major[1]) < 25:
            raise RuntimeError("Original constructor-prefix fixture requires Java 25 or newer")
        java_options = ["--enable-final-field-mutation=ALL-UNNAMED"] if int(major[1]) >= 26 else []
        fixture = FIXTURES / "object_prefix_defaults/java/fixture"
        authored, originals, harness = (work / name for name in ("authored", "originals", "harness"))
        verify.compile_local(sorted(fixture.glob("*.java")), authored, release=25)
        compatible = work / "compatible-declarations"
        verify.compile_local([p for p in sorted(fixture.glob("*.java")) if p.name != "Owner.java"],
                             compatible, classpath=authored, release=8)
        for p in compatible.rglob("*.class"):
            shutil.copy2(p, authored / p.relative_to(compatible))
        verify.compile_local([FIXTURES / "harness" / ("ObjectPrefixDefaults" + name + ".java") for name in ("Caller", "Harness")],
                             harness, classpath=authored, release=8)
        authored_hashes, harness_hashes = hashes(authored), hashes(harness)
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        sdk_hash, cli_hash = sha(args.android_jar), sha(args.neverd)

        def execute(runtime, label, finals):
            result = []
            for route in range(5):
                for mode in range(12):
                    output = verify.run([verify.java, *java_options, "-cp", os.pathsep.join(map(str, runtime)),
                                         "probe.ObjectPrefixDefaultsHarness", str(route), str(mode), str(finals).lower()],
                                        f"{label}-{route}-{mode}")
                    if output != "attempts=2,object-prefix-defaults=matched\n":
                        raise RuntimeError("Frozen constructor-prefix observer changed")
                    result.append(output)
            return result

        baseline = execute((authored, harness), "authored", False)
        shutil.copytree(authored, originals)
        witness = encode_finals(originals / "fixture/Owner.class")
        before, records = compiler_facts(authored), compiler_facts(originals)
        for owner, record in before.items():
            wanted = deepcopy(record)
            if owner == OWNER:
                for name, (typ, _) in DEFAULTS.items():
                    wanted["fields"][OWNER + "->" + name + ":" + typ]["access"] |= 0x10
            if any(wanted[key] != records[owner].get(key) for key in wanted if key != "sha256"):
                raise RuntimeError("Default-field encoding changed another declaration")
            relative = owner[1:-1] + ".class"
            if method_code((authored / relative).read_bytes()) != method_code((originals / relative).read_bytes()):
                raise RuntimeError("Default-field encoding changed original compiler Code")
        if records[OWNER]["methods"][CTOR]["signature"] != SIGNATURE:
            raise RuntimeError("Original closed callback constructor Signature changed")
        if execute((originals, harness), "originals", True) != baseline:
            raise RuntimeError("Final field bits changed original behavior")
        original_hashes, exceptions = hashes(originals), exception_inventory(originals)
        (work / "encoding-witness.json").open("x").write(json.dumps(witness, indent=2) + "\n")
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"authored": authored_hashes, "originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        (work / "baseline.json").open("x").write(json.dumps(baseline) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 7 or len(all_methods) != 18:
            raise RuntimeError("Original object-prefix inventory changed")
        classes = all_classes - ({"Lfixture/Trace;"} if args.dependency_trace else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        dependency = work / "owned-trace.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            jar.writestr("fixture/Trace.class", (originals / "fixture/Trace.class").read_bytes())
        dependency_hash = sha(dependency)
        mutations = [
            ("Owner", 'Trace.guard(callback, "owned");', ";", 0),
            ("Owner", 'Trace.guard(callback, "owned");', 'Trace.guard(null, "owned");', 0),
            ("Trace", "Trace.argument = argument;", "Trace.argument = null;", 0),
            ("Trace", "Trace.label = label;", 'Trace.label = "";', 0),
            ("Trace", "guardAction.run();", ";", 2),
            ("Trace", "guardAction.run();", "try { guardAction.run(); } catch (Throwable failure) { throw new IllegalStateException(failure); }", 2),
            ("Trace", "partial = owner;", "partial = null;", 0),
            ("Owner", "return callback;", "return null;", 0),
            ("Owner", "return zeroFlag;", "return false;", 8),
            ("Owner", "return zeroBits;", "return (int) zeroBits;", 8),
            ("Owner", "return zeroArray;", "return null;", 8),
            ("Trace", "publishAction.run();", ";", 5),
            ("Trace", "publishAction.run();", "try { publishAction.run(); } catch (Throwable failure) { throw new IllegalStateException(failure); }", 7),
            ("Core", "Trace.hit(0);", ";", 0),
            ("Owner", "Trace.publish(this);", "Trace.publish(this); Trace.publish(this);", 8),
            ("Owner", "this.callback = callback;", "this.callback = null;", 0),
            ("Trace", 'if (argument == null) throw new NullPointerException("owned argument");', ";", 1),
            ("Owner", "Trace.publish(this);", "Trace.publish(this); zeroFlag = false; zeroBits = 0; zeroArray = null;", 8),
        ]
        for index, (name, anchor, replacement, mode) in enumerate(mutations):
            text = (fixture / (name + ".java")).read_text()
            if text.count(anchor) != 1:
                raise RuntimeError("Independent object-prefix counterexample anchor changed")
            source = work / f"wrong-{index}-source" / (name + ".java")
            source.parent.mkdir()
            source.open("x").write(text.replace(anchor, replacement))
            variant = work / f"wrong-{index}-classes"
            verify.compile_local([source], variant, classpath=authored, release=25 if name == "Owner" else 8)
            if name == "Owner":
                encode_finals(variant / "fixture/Owner.class")
            result = subprocess.run([verify.java, *java_options, "-cp", os.pathsep.join(map(str, (variant, originals, harness))),
                                     "probe.ObjectPrefixDefaultsHarness", "0", str(mode), "true"],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / f"wrong-{index}-counterexample.log").open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned object prefix defaults mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program " + str(index))

        # The prefix must not acquire access to another class's private entry.
        inaccessible = work / "inaccessible-guard-sources"
        shutil.copytree(fixture, inaccessible)
        guard_source = inaccessible / "Trace.java"
        declaration = "public static void guard(Object argument, String label)"
        guard_text = guard_source.read_text()
        if guard_text.count(declaration) != 1:
            raise RuntimeError("Owned guard access-control declaration changed")
        guard_source.write_text(guard_text.replace(declaration, declaration.replace("public", "private", 1)))
        control = subprocess.run([verify.javac, "-proc:none", "--release", "25", "-d", work / "inaccessible-guard-classes",
                                  *sorted(inaccessible.glob("*.java"))],
                                 text=True, capture_output=True, timeout=600, env=verify.environment)
        (work / "inaccessible-guard-compiler-control.log").open("x").write(control.stdout + control.stderr)
        if control.returncode == 0 or "guard" not in control.stderr or "private access" not in control.stderr:
            raise RuntimeError("Independent javac accepted an inaccessible constructor-prefix call")

        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_trace:
            classpath += ["--classpath", dependency]
        # The pinned D8 predates Java 25. Keep originals frozen and change only
        # the two-byte version header on a separate compiler-input copy.
        d8_inputs = work / "d8-inputs"
        shutil.copytree(originals, d8_inputs)
        owner_class = d8_inputs / "fixture/Owner.class"
        original_bytes = owner_class.read_bytes()
        if original_bytes[:8] != b"\xca\xfe\xba\xbe\x00\x00\x00E":
            raise RuntimeError("Owned Java 25 class version changed")
        lowered = original_bytes[:6] + (52).to_bytes(2, "big") + original_bytes[8:]
        owner_class.write_bytes(lowered)
        if (lowered[:6] != original_bytes[:6] or lowered[8:] != original_bytes[8:]
                or method_code(lowered) != method_code(original_bytes)):
            raise RuntimeError("D8 compatibility copy changed original Code or attributes")
        compatible_records = compiler_facts(d8_inputs)
        for c in records:
            for key in records[c]:
                expected = 52 if key == "major" else records[c][key]
                if key != "sha256" and compatible_records[c][key] != expected:
                    raise RuntimeError("D8 compatibility copy changed original metadata")
        if exception_inventory(d8_inputs) != exceptions or execute((d8_inputs, harness), "d8-inputs", True) != baseline:
            raise RuntimeError("D8 compatibility copy changed original behavior or Exceptions")
        d8_hashes = hashes(d8_inputs)
        (work / "d8-compatibility-witness.json").open("x").write(json.dumps({
            "owner_major_before": 69, "owner_major_after": 52,
            "all_Code_and_other_bytes": "unchanged", "frozen_hashes": d8_hashes}) + "\n")
        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(d8_inputs.rglob("*.class")) if "L" + p.relative_to(d8_inputs).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original object-prefix DEX count changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original object-prefix definitions")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-prefix")
        for name in DEFAULTS:
            if re.search(r"iput\S*[^\n]*Lfixture/Owner;\." + name + ":", disassembly):
                raise RuntimeError("Original default field acquired a physical store")
        constructors = [block[2] for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly)
                        if block[1] == OWNER and re.search(r"(?m)^\s+name\s+: '<init>'", block[2])]
        if len(constructors) != 1:
            raise RuntimeError("Original constructor entry changed")
        body = constructors[0]
        guard, parent, stored = body.find("Lfixture/Trace;.guard:"), body.find("Ljava/lang/Object;.<init>:"), body.find("Lfixture/Owner;.callback:")
        if not (0 <= guard < parent < stored):
            raise RuntimeError("Original guard, Object preparation and callback store order changed")
        (work / "input-method-witness.json").open("x").write(json.dumps({"guard_before_Object_before_original_store": True, "unwritten_fields": sorted(DEFAULTS), "dex_sha256": sha(source_dex)}) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "recovered" or coverage.get("projected_method_count", 0)
                or coverage.get("minimum_java_release", 8) != 25 or coverage.get("class_source_bindings")
                or coverage.get("field_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Object-prefix recovery changed original entries or contracts")
        for identity, row in rows.items():
            expected = "recovered" if records[row["class"]]["methods"][identity]["code"] else "declaration-only"
            if row["status"] != expected:
                raise RuntimeError("Original object-prefix Code/declaration role changed")
        initializers = coverage.get("default_instance_final_initializers", [])
        if len(initializers) != len(DEFAULTS) or {row["field"] for row in initializers} != set(DEFAULTS):
            raise RuntimeError("Missing exact allocation-default receipts")
        for row in initializers:
            typ, value = DEFAULTS[row["field"]]
            if (row["class"] != OWNER or row["type"] != typ or row["original_write_count"] != 0 or row["ConstantValue"] != "absent"
                    or row["source_initializer"] != "(null == null ? " + value + " : " + value + ")"
                    or row["initialization_point"] != "after-Object-constructor-before-original-body"):
                raise RuntimeError("Allocation-default receipt changed scope, value or timing")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_trace else None, release=25)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes
                            if any(row["code"] for row in records[c]["methods"].values())}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Object-prefix recovery invented or omitted an original entry/helper")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for c in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if after[c].get(key) != records[c].get(key):
                    raise RuntimeError("Object-prefix class metadata or ConstantValue changed: " + c + ":" + key)
            for identity, metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity) != metadata or after_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Original object-prefix method metadata or Exceptions changed: " + identity)
        for identity in expected_helpers:
            metadata = after[identity.split("->", 1)[0]]["methods"][identity]
            if (metadata["access"] != 0x000A or not metadata["code"]
                    or metadata["signature"] != "<E:Ljava/lang/Throwable;>(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;^TE;"):
                raise RuntimeError("Generated exception helper changed its private contract")
        runtime = (rebuilt, harness, dependency) if args.dependency_trace else (rebuilt, harness)
        if execute(runtime, "rebuilt", True) != baseline:
            raise RuntimeError("Object-prefix defaults changed references, effects, field reads or exact failures")
        if (hashes(authored) != authored_hashes or hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashes(d8_inputs) != d8_hashes or sha(args.android_jar) != sdk_hash or sha(args.neverd) != cli_hash or sha(dependency) != dependency_hash):
            raise RuntimeError("Original compiler inputs or frozen observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "stages": 4,
                    "independent_wrong_programs": len(mutations), "inaccessible_guard_compiler_controls": 1, "original_Code_and_encoding_attributes": "unchanged except the exact final field bits",
                    "d8_compatibility_copy": "version header only; Code and attributes unchanged", "all_generated_Java": "compiled", "original_metadata_and_Exceptions": "matched", "ConstantValue": "absent",
                    "prefix_order_and_opaque_callback_identity": "matched", "reflective_field_reads_and_partial_effects": "matched",
                    "dependency_trace": args.dependency_trace, "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
