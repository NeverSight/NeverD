#!/usr/bin/env python3
"""Verify closed local carrier targets with disclosed Java method spellings."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
import copy
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
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_static_defaults import method_code
    from scripts.test_mobile_android_shared_upper_bounds import class_records
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_static_defaults import method_code
    from test_mobile_android_shared_upper_bounds import class_records

ORIGINAL_NAME = "apply-owned-wide"
PROTOTYPE = "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"
INPUT_SIGNATURE = "Ljava/lang/Object;Lfixture/Function<Lfixture/CarrierA;Lfixture/CarrierB;Lfixture/CarrierB;>;"


def encode_separate_name(path: Path) -> dict:
    before = path.read_bytes()
    old = b"\x01\x00\x05apply"
    new = b"\x01" + len(ORIGINAL_NAME).to_bytes(2, "big") + ORIGINAL_NAME.encode()
    if before.count(old) != 1:
        raise RuntimeError("Missing exact independent interface/call UTF8 name")
    after = before.replace(old, new, 1)
    expected = {name.replace(b"apply(JJ)V", ORIGINAL_NAME.encode() + b"(JJ)V"): code
                for name, code in method_code(before).items()}
    if (method_code(after) != expected or
            before[class_records(before)["pool_end"]:] != after[class_records(after)["pool_end"]:]):
        raise RuntimeError("Independent name encoding changed Code or declaration bytes")
    path.write_bytes(after)
    return {"class_file": path.name, "original_name": "apply", "input_name": ORIGINAL_NAME,
            "all_code_and_descriptors": "unchanged"}


def encode(root: Path, enclosing: bool = False) -> dict:
    final_edits = []
    for relative in ("fixture/Owner$1.class", "fixture/Owner.class"):
        item = root / relative
        before = item.read_bytes()
        shape = class_records(before)
        data = bytearray(before)
        if relative.endswith("Owner$1.class"):
            at = shape["pool_end"]
            if int.from_bytes(before[at:at + 2], "big") != 0x20:
                raise RuntimeError("Owned anonymous class flags changed")
            data[at:at + 2] = (0x30).to_bytes(2, "big")
        found = 0
        for name, start, stop in shape["rows"]:
            if name != b"InnerClasses": continue
            count = int.from_bytes(before[start + 6:start + 8], "big")
            if stop != start + 8 + count * 8: raise RuntimeError("Malformed owned inner metadata")
            for i in range(count):
                at = start + 8 + i * 8
                inner = int.from_bytes(before[at:at + 2], "big")
                if shape["strings"][shape["classes"][inner]] != b"fixture/Owner$1": continue
                if int.from_bytes(before[at + 6:at + 8], "big") != 0: raise RuntimeError("Owned anonymous inner flags changed")
                data[at + 6:at + 8] = (0x10).to_bytes(2, "big")
                found += 1
        if found != 1 or method_code(before) != method_code(bytes(data)):
            raise RuntimeError("Finality encoding changed Code or missed the owner")
        item.write_bytes(data)
        final_edits.append(relative)
    path = root / "fixture/Owner$1.class"
    before = path.read_bytes()
    shape = class_records(before)
    if any(name == b"Signature" for name, _, _ in shape["rows"]):
        raise RuntimeError("Owned raw anonymous class unexpectedly has a Signature")
    count, pool_end, attributes = shape["count"], shape["pool_end"], shape["class_attributes"]
    text = INPUT_SIGNATURE.encode()
    pool = b"\x01\x00\x09Signature" + b"\x01" + len(text).to_bytes(2, "big") + text
    attribute = count.to_bytes(2, "big") + (2).to_bytes(4, "big") + (count + 1).to_bytes(2, "big")
    after = (before[:8] + (count + 2).to_bytes(2, "big") + before[10:pool_end] + pool
             + before[pool_end:attributes] + (shape["attribute_count"] + 1).to_bytes(2, "big")
             + before[attributes + 2:] + attribute)
    if method_code(before) != method_code(after): raise RuntimeError("Class Signature insertion changed Code")
    path.write_bytes(after)
    receipt = {"signature": {"original": None, "input": INPUT_SIGNATURE, "all_code_bytes": "unchanged"},
               "flags": bridge_flag(path, "invoke", PROTOTYPE, 0x1, 0x1041)}
    before = path.read_bytes()
    old = b"\x01\x00\x05apply"
    new = b"\x01" + len(ORIGINAL_NAME).to_bytes(2, "big") + ORIGINAL_NAME.encode()
    if before.count(old) != 1:
        raise RuntimeError("Missing unique owned JVM target name")
    after = before.replace(old, new, 1)
    wanted = {name.replace(b"apply(JJ)J", ORIGINAL_NAME.encode() + b"(JJ)J"): code
              for name, code in method_code(before).items()}
    if (method_code(after) != wanted or
            before[class_records(before)["pool_end"]:] != after[class_records(after)["pool_end"]:]):
        raise RuntimeError("Owned name encoding changed Code or declaration bytes")
    path.write_bytes(after)
    if enclosing:
        nested = root / "fixture/Owner$1$1.class"
        before_nested = nested.read_bytes()
        if before_nested.count(old) != 1:
            raise RuntimeError("Missing exact original nested EnclosingMethod name")
        after_nested = before_nested.replace(old, new, 1)
        if (method_code(before_nested) != method_code(after_nested) or
                before_nested[class_records(before_nested)["pool_end"]:] != after_nested[class_records(after_nested)["pool_end"]:]):
            raise RuntimeError("Owned nested name encoding changed Code or declaration bytes")
        nested.write_bytes(after_nested)
        receipt["nested_enclosure_name"] = {"original": "apply", "input": ORIGINAL_NAME, "all_code_bytes": "unchanged"}
    receipt["finality"] = {"classes": final_edits, "all_code_bytes": "unchanged"}
    receipt["name"] = {"source": "apply", "jvm": ORIGINAL_NAME, "all_code_bytes": "unchanged"}
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-carriers", action="store_true")
    parser.add_argument("--enclosing", action="store_true")
    parser.add_argument("--separate-family", action="store_true")
    args = parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-carrier-target-names-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "carrier_target_names/java"
        carriers = FIXTURES / "value_carrier_bridges/java/fixture"
        if args.enclosing:
            authored = work / "authored-java"
            shutil.copytree(fixture, authored)
            source = authored / "fixture/Owner.java"
            text = source.read_text()
            marker = "                return second;"
            if text.count(marker) != 1: raise RuntimeError("Missing exact nested enclosure source marker")
            source.write_text(text.replace(marker, "                Trace.marker = new Object() {};\n" + marker))
            fixture = authored
            trace = authored / "fixture/Trace.java"
            trace.write_text((carriers / "Trace.java").read_text().replace("public final class Trace {", "public final class Trace {\n    public static Object marker;"))
        inputs = [*sorted(fixture.rglob("*.java")), *[carriers / (n + ".java") for n in ("CarrierA", "CarrierB", "Function", "Trace") if not (args.enclosing and n == "Trace")]]
        family_fixture = FIXTURES / "carrier_target_names_family/java"
        if args.separate_family:
            inputs += sorted(family_fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals, release=8)
        (work / "owned-encoding.json").write_text(json.dumps(encode(originals, args.enclosing), indent=2) + "\n")
        harness_sources = [FIXTURES / "harness" / (n + ".java") for n in
                           ("CarrierTargetNamesCaller", "CarrierTargetNamesHarness")]
        if args.enclosing:
            observer = work / "authored-harness/CarrierTargetNamesHarness.java"
            observer.parent.mkdir()
            text = harness_sources[1].read_text()
            marker = "            Trace.targetFailure = problem;"
            if text.count(marker) != 1: raise RuntimeError("Nested observer anchor changed")
            observer.write_text(text.replace(marker, "            require(Trace.marker != null && Trace.marker.getClass().getEnclosingMethod().equals(primitive));\n"
                                            "            Field captured = Trace.marker.getClass().getDeclaredField(\"this$0\");\n"
                                            "            captured.setAccessible(true); require(captured.get(Trace.marker) == target);\n" + marker))
            harness_sources[1] = observer
        if args.separate_family:
            harness_sources.append(FIXTURES / "harness/SeparatedCarrierFamilyHarness.java")
        verify.compile_local(harness_sources, harness, classpath=originals, release=8)
        if args.separate_family:
            receipt = [encode_separate_name(originals / ("fixture/" + name + ".class")) for name in ("WideAction", "WideOwner")]
            (work / "separate-family-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes) != (7 if args.enclosing else 6) + (2 if args.separate_family else 0)
                or len(all_methods) != (13 if args.enclosing else 12) + (4 if args.separate_family else 0)):
            raise RuntimeError("Owned local carrier inventory changed: " + repr((len(all_classes), len(all_methods))))
        excluded = {"Lfixture/CarrierA;", "Lfixture/CarrierB;"} if args.dependency_carriers else set()
        classes = all_classes - excluded
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        owner = "Lfixture/Owner$1;"
        original_target = owner + "->" + ORIGINAL_NAME + "(JJ)J"
        bridge = owner + "->invoke" + PROTOTYPE
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-carriers.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for name in ("CarrierA", "CarrierB"):
                jar.writestr("fixture/" + name + ".class", (originals / "fixture" / (name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_carriers: classpath += ["--classpath", dependency]

        original_root = "Lfixture/WideAction;->" + ORIGINAL_NAME + "(JJ)V"
        original_implementation = "Lfixture/WideOwner;->" + ORIGINAL_NAME + "(JJ)V"
        def execute(runtime, label, target, root_name=None):
            outputs = []
            for route in range(4):
                for mode in range(9):
                    observer = "SeparatedCarrierFamilyHarness" if args.separate_family else "CarrierTargetNamesHarness"
                    out = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), observer,
                                      str(route), str(mode), target, *([root_name] if args.separate_family else [])], f"{label}-{route}-{mode}")
                    if not re.fullmatch(f"route={route},mode={mode},attempts=2,trace=[0-9]+\n", out):
                        raise RuntimeError("Owned carrier target observer changed: " + out)
                    outputs.append(out)
            return outputs
        baseline = execute((originals, harness), "baseline", ORIGINAL_NAME, ORIGINAL_NAME)
        dex = work / "dex"; dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed original method/class inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "bridge-disassembly")
        if ORIGINAL_NAME + ":(JJ)J" not in disassembly or disassembly.count("move-result-wide") < 3:
            raise RuntimeError("Missing independent original wide bridge witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage["projected_method_count"] != (5 if args.enclosing else 4) + (2 if args.separate_family else 0) or coverage["status"] != "partial"):
            raise RuntimeError("Carrier name projection changed original inventory")
        row = next(r for r in coverage["methods"] if r["identity"] == original_target)
        if (row.get("projection_kind") != "virtual-method-identifier" or row.get("source_name_declaration") != original_target
                or row.get("source_name_family") != [original_target] or not row.get("metadata_differences")):
            raise RuntimeError("Missing exact target identifier projection")
        source_name = row["source_name"]
        family_name = None
        if args.separate_family:
            family_rows = {r["identity"]: r for r in coverage["methods"]}
            family_name = family_rows[original_root].get("source_name")
            for identity in (original_root, original_implementation):
                member = family_rows[identity]
                if (member.get("projection_kind") != "virtual-method-identifier"
                        or member.get("source_name") != family_name or family_name == source_name
                        or member.get("source_name_declaration") != original_root
                        or set(member.get("source_name_family", [])) != {original_root, original_implementation}):
                    raise RuntimeError("An independent carrier target displaced the complete interface family")
        carrier_row = next(r for r in coverage["methods"] if r["identity"] == bridge)
        if (carrier_row.get("projection_kind") != "value-carrier-interface-bridge"
                or carrier_row.get("bridge_target") != original_target):
            raise RuntimeError("Identifier spelling displaced original carrier proof")
        projections = coverage.get("generic_signature_projections", [])
        if (len(projections) != 1 or len(projections[0]["changes"]) != 1
                or projections[0]["changes"][0]["kind"] != "checked-value-carrier-interface"
                or projections[0]["changes"][0]["scope_origin"] != bridge):
            raise RuntimeError("Missing exact raw interface receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_carriers else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        bindings = coverage.get("class_source_bindings", [])
        if len(bindings) != (2 if args.enclosing else 1) or any(b["binding_kind"] != "anonymous-to-named-local" for b in bindings):
            raise RuntimeError("Missing exact local owner scope")
        by_owner, class_names = {b["class"]: b for b in bindings}, {}
        for b in bindings:
            matches = [c for c in actual_classes if c.endswith(b["source_name"] + ";")]
            if len(matches) != 1: raise RuntimeError("Ambiguous recompiled local class mapping")
            class_names[b["class"]] = matches[0]
        mapped_owner = class_names[owner]
        if args.enclosing:
            nested_receipt = by_owner["Lfixture/Owner$1$1;"].get("source_enclosing_method")
            if nested_receipt != {"identity": owner + "->" + source_name + "(JJ)J", "owner": owner,
                                   "name": source_name, "prototype": "(JJ)J", "parameters": ["J", "J"], "returns": "J"}:
                raise RuntimeError("Missing exact renamed enclosing-method receipt")
        nested_owner = "Lfixture/Owner$1$1;"
        nested_constructor = nested_owner + "-><init>(" + owner + ")V"
        if args.enclosing and not any("constructor parameters may differ" in text for text in by_owner[nested_owner]["metadata_differences"]):
            raise RuntimeError("Missing original enclosing-instance constructor disclosure")
        def mapped(identity):
            if args.enclosing and identity == nested_constructor:
                identity = nested_owner + "-><init>(" + owner + owner + ")V"
            result = identity.replace(original_target, owner + "->" + source_name + "(JJ)J")
            if args.separate_family:
                result = result.replace(original_root, "Lfixture/WideAction;->" + family_name + "(JJ)V")
                result = result.replace(original_implementation, "Lfixture/WideOwner;->" + family_name + "(JJ)V")
            for old_owner, new_owner in class_names.items(): result = result.replace(old_owner, new_owner)
            return result
        helpers = {mapped(r["class"] + "->" + r["name"] + r["prototype"]) for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != {mapped(c) for c in classes} or set(actual_methods) != {mapped(m) for m in methods} | helpers:
            raise RuntimeError("Recompiled local names lost or invented an entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[mapped(identity)]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                expected = None if identity == owner and key == "signature" else before.get(key)
                if identity == nested_owner and key == "fields":
                    expected = {mapped(k): copy.deepcopy(v) for k, v in expected.items()}
                    if len(expected) != 1: raise RuntimeError("Unexpected nested capture field")
                    field = expected[mapped(nested_owner + "->this$0:" + owner)]
                    if field["access"] != 0x1010: raise RuntimeError("Original capture flags changed")
                    field["access"] &= ~0x1000
                    field["descriptor"] = mapped(owner)
                if identity in by_owner and key == "inner_class":
                    expected = copy.deepcopy(expected)
                    expected["name"] = by_owner[identity]["source_name"]
                    expected["inner"] = class_names[identity]
                if identity in by_owner and key == "enclosing_method":
                    expected = copy.deepcopy(expected)
                    expected["owner"] = mapped(expected["owner"])
                    if identity != owner: expected["name"] = source_name
                if after.get(key) != expected: raise RuntimeError("Undisclosed carrier class metadata difference: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                expected = copy.deepcopy(metadata)
                if method == original_target: expected["name"] = source_name
                if args.separate_family and method in (original_root, original_implementation): expected["name"] = family_name
                if method == bridge: expected["access"] = 0x1
                if args.enclosing and method == nested_constructor:
                    expected["prototype"] = "(" + mapped_owner + mapped_owner + ")V"
                if (after["methods"].get(mapped(method)) != expected or actual_exceptions.get(mapped(method)) != exceptions[method]):
                    raise RuntimeError("Undisclosed target or bridge metadata difference: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_carriers else (rebuilt, harness)
        if execute(runtime, "rebuilt", source_name, family_name) != baseline:
            raise RuntimeError("Carrier rename changed original effects or identities")
        mutations = [("wrong-result", "return second;", "return first;"),
                     ("lost-effect", "Trace.events = Trace.events * 10 + 3;", "Trace.events = Trace.events;"),
                     ("swapped-inputs", "apply(a, b)", "apply(b, a)")]
        source = (fixture / "fixture/Owner.java").read_text()
        for name, before, after in mutations:
            if source.count(before) != 1: raise RuntimeError("Counterexample source marker changed")
            edited = work / name / "fixture/Owner.java"; edited.parent.mkdir(parents=True)
            edited.write_text(source.replace(before, after))
            variant = work / (name + "-classes")
            verify.compile_local([edited], variant, classpath=originals, release=8)
            encode(variant, args.enclosing)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (variant, originals, harness))),
                                     "CarrierTargetNamesHarness", "0", "0", ORIGINAL_NAME], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (name + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "AssertionError" not in result.stderr:
                raise RuntimeError("Immutable observer missed " + name)
        separate_counterexamples = 0
        if args.separate_family:
            source_path = family_fixture / "fixture/WideOwner.java"
            source_text = source_path.read_text()
            old = "        if (failure != null) throw failure;\n        second = secondValue;"
            if source_text.count(old) != 1:
                raise RuntimeError("Independent interface partial-write witness changed")
            wrong = work / "early-interface-write/fixture/WideOwner.java"
            wrong.parent.mkdir(parents=True)
            wrong.write_text(source_text.replace(old, "        second = secondValue;\n        if (failure != null) throw failure;"))
            variant = work / "early-interface-write-classes"
            verify.compile_local([family_fixture / "fixture/WideAction.java", wrong], variant, release=8)
            for name in ("WideAction", "WideOwner"):
                encode_separate_name(variant / ("fixture/" + name + ".class"))
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (variant, originals, harness))),
                "SeparatedCarrierFamilyHarness", "1", "1", ORIGINAL_NAME, ORIGINAL_NAME],
                text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "early-interface-write-counterexample.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned separate carrier family" not in result.stderr:
                raise RuntimeError("Immutable interface observer missed early partial writes")
            separate_counterexamples = 1
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs or immutable observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "dependency_carriers": args.dependency_carriers, "renamed_enclosing_method": args.enclosing, "independent_compiled_counterexamples": len(mutations),
                    "separate_interface_family": args.separate_family, "interface_counterexamples": separate_counterexamples,
                    "original_code_bytes": "unchanged during input metadata encoding",
                    "method_and_class_inventory": "exact after reported name mapping and helper accounting",
                    "metadata": ("unchanged except disclosed local/method names, raw interface, bridge flags, enclosing method and capture constructor/field differences"
                                 if args.enclosing else "unchanged except disclosed local name, method name, raw interface and bridge flags"),
                    "input_and_observer_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0

if __name__ == "__main__": raise SystemExit(main())
