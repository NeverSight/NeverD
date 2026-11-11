#!/usr/bin/env python3
"""Compare original checked wide-carrier calls with recovered JVM execution."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_inherited_erased_parameters import add_parameter_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_inherited_erased_parameters import add_parameter_signature


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--mixed", action="store_true")
    parser.add_argument("--generic-result", action="store_true")
    parser.add_argument("--boxed-integer", action="store_true")
    parser.add_argument("--result-only", action="store_true")
    parser.add_argument("--generic-input", action="store_true")
    parser.add_argument("--zero-arity", action="store_true")
    parser.add_argument("--standard-long", action="store_true")
    parser.add_argument("--standard-double", action="store_true")
    parser.add_argument("--standard-float", action="store_true")
    parser.add_argument("--named-primitive", action="store_true")
    parser.add_argument("--primitive-array-result", action="store_true")
    parser.add_argument("--reference-carrier", action="store_true")
    parser.add_argument("--array-unit", action="store_true")
    parser.add_argument("--scalar-unit", action="store_true")
    parser.add_argument("--parameterized-reference", action="store_true")
    parser.add_argument("--open-reference", action="store_true")
    args = parser.parse_args()
    if sum([args.mixed, args.generic_result, args.boxed_integer, args.result_only,
            args.generic_input, args.zero_arity, args.standard_long, args.standard_double, args.standard_float,
            args.named_primitive, args.primitive_array_result, args.reference_carrier, args.array_unit, args.scalar_unit]) > 1:
        parser.error("Select one value-carrier profile")
    if args.parameterized_reference and not (args.reference_carrier or args.array_unit):
        parser.error("Parameterized references require a reference or array Unit profile")
    if args.open_reference and not args.parameterized_reference:
        parser.error("Open references require --parameterized-reference")
    args.standard_long |= args.standard_double
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-value-carrier-bridges-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest() if args.open_reference else None
        originals, harness_dir = work / "originals", work / "harness"
        fixture = ("scalar_carrier_unit_bridges" if args.scalar_unit else
                   "array_carrier_unit_bridges" if args.array_unit else
                   "reference_carrier_bridges" if args.reference_carrier else
                   "primitive_array_result_bridges" if args.primitive_array_result else
                   "named_primitive_reference_bridges" if args.named_primitive else
                   "standard_float_reference_bridges" if args.standard_float else
                   "standard_double_reference_bridges" if args.standard_double else
                   "standard_long_reference_bridges" if args.standard_long else
                   "zero_arity_carrier_bridges" if args.zero_arity else
                   "generic_input_carrier_bridges" if args.generic_input else
                   "result_only_carrier_bridges" if args.result_only else
                   "boxed_integer_reference_bridges" if args.boxed_integer else
                   "generic_result_value_carrier_bridges" if args.generic_result else
                   "mixed_value_carrier_bridges" if args.mixed else "value_carrier_bridges")
        inputs = sorted((FIXTURES / (fixture + "/java")).rglob("*.java"))
        if args.mixed or args.generic_result:
            inputs += [FIXTURES / ("value_carrier_bridges/java/fixture/" + name + ".java")
                       for name in ("CarrierA", "CarrierB", "Trace")]
        if args.generic_result:
            inputs.append(FIXTURES / "value_carrier_bridges/java/fixture/Function.java")
        if args.boxed_integer or args.standard_long or args.standard_float or args.named_primitive or args.primitive_array_result or args.reference_carrier or args.array_unit or args.scalar_unit:
            inputs.append(FIXTURES / "value_carrier_bridges/java/fixture/Trace.java")
        if args.standard_float:
            inputs.append(FIXTURES / "generic_result_value_carrier_bridges/java/fixture/Box.java")
        if args.result_only or args.zero_arity:
            inputs += [FIXTURES / ("value_carrier_bridges/java/fixture/" + name + ".java")
                       for name in ("CarrierB", "Trace")]
        if args.generic_input:
            inputs += [FIXTURES / ("value_carrier_bridges/java/fixture/" + name + ".java")
                       for name in ("CarrierA", "Trace")]
            inputs.append(FIXTURES / "generic_result_value_carrier_bridges/java/fixture/Box.java")
        if args.parameterized_reference:
            carrier = next(p for p in inputs if p.name == "Carrier.java")
            content = carrier.read_text()
            declaration = "public class Carrier {"
            if content.count(declaration) != 1:
                raise RuntimeError("Missing exact owned carrier class declaration")
            owned_sources = work / "parameterized-reference-java/fixture"
            owned_sources.mkdir(parents=True)
            replacement = owned_sources / "Carrier.java"
            replacement.write_text(content.replace(declaration, "public class Carrier<Q> {"))
            inputs = [replacement if p == carrier else p for p in inputs]
            if not args.open_reference:
                tag = owned_sources / "Tag.java"
                tag.write_text("package fixture; public final class Tag {}\n")
                inputs.append(tag)
        verify.compile_local(inputs, originals)
        harness = ("ScalarCarrierUnitBridgeHarness" if args.scalar_unit else
                   "ArrayCarrierUnitBridgeHarness" if args.array_unit else
                   "ReferenceCarrierBridgeHarness" if args.reference_carrier else
                   "PrimitiveArrayResultBridgeHarness" if args.primitive_array_result else
                   "NamedPrimitiveReferenceBridgeHarness" if args.named_primitive else
                   "StandardFloatReferenceBridgeHarness" if args.standard_float else
                   "StandardDoubleReferenceBridgeHarness" if args.standard_double else
                   "StandardLongReferenceBridgeHarness" if args.standard_long else
                   "ZeroArityCarrierBridgeHarness" if args.zero_arity else
                   "GenericInputCarrierBridgeHarness" if args.generic_input else
                   "ResultOnlyCarrierBridgeHarness" if args.result_only else
                   "BoxedIntegerReferenceBridgeHarness" if args.boxed_integer else
                   "GenericResultValueCarrierBridgeHarness" if args.generic_result else
                   "MixedValueCarrierBridgeHarness" if args.mixed else "ValueCarrierBridgeHarness")
        harness_source = FIXTURES / ("harness/" + harness + ".java")
        if args.parameterized_reference:
            content = harness_source.read_text()
            condition = "arguments[0] != Carrier.class"
            if content.count(condition) != 1:
                raise RuntimeError("Missing exact original carrier reflection assertion")
            if args.open_reference:
                argument = "((ParameterizedType) arguments[0]).getActualTypeArguments()[0]"
                mismatch = ("((ParameterizedType) arguments[0]).getActualTypeArguments().length != 1 || "
                    + "!(" + argument + " instanceof WildcardType) || "
                    + "((WildcardType) " + argument + ").getLowerBounds().length != 0 || "
                    + "!java.util.Arrays.equals(((WildcardType) " + argument + ").getUpperBounds(), "
                    + "new Type[] {Receiver.class.getTypeParameters()[0]})")
            else:
                mismatch = ("!java.util.Arrays.equals(((ParameterizedType) arguments[0]).getActualTypeArguments(), "
                            "new Type[] {Tag.class})")
            content = content.replace(condition,
                "(!(arguments[0] instanceof ParameterizedType) || "
                "((ParameterizedType) arguments[0]).getRawType() != Carrier.class || " + mismatch + ")")
            harness_source = work / (harness + ".java")
            harness_source.write_text(content)
        verify.compile_local([harness_source], harness_dir, classpath=originals)
        original_signature = b"<S:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Function;"
        if args.result_only or args.generic_input or args.zero_arity or args.standard_long:
            original_signature = (b"<S:Ljava/lang/Object;V:Ljava/lang/Object;>"
                                  b"Ljava/lang/Object;Lfixture/Function;")
        interface = ("Lfixture/Function<Lfixture/CarrierA;Lfixture/CarrierB;"
                     + ("Lfixture/Box<Lfixture/CarrierA;>;" if args.generic_result else "Lfixture/CarrierB;")
                     + ("Lfixture/CarrierA;" if args.mixed else "") + ">;")
        if args.boxed_integer:
            interface = "Lfixture/Function<Ljava/lang/Integer;Ljava/lang/Integer;>;"
        if args.result_only:
            interface = "Lfixture/Function<TS;Lfixture/CarrierB;>;"
        if args.generic_input:
            interface = ("Lfixture/Function<Lfixture/Box<TS;>;Lfixture/CarrierA;"
                         "Ljava/lang/Integer;Lfixture/Box<TS;>;>;")
        if args.zero_arity:
            interface = "Lfixture/Function<Lfixture/CarrierB;>;"
        if args.standard_long:
            interface = "Lfixture/Function<Ljava/lang/Long;TS;>;"
        if args.standard_double:
            interface = "Lfixture/Function<Ljava/lang/Double;TS;>;"
        if args.standard_float:
            interface = "Lfixture/Function<Ljava/lang/Float;Lfixture/Box<-Ljava/lang/Boolean;>;Ljava/lang/Object;>;"
        if args.named_primitive:
            interface = "Lfixture/Function<Ljava/lang/Float;Lfixture/Vector;>;"
        if args.primitive_array_result:
            interface = "Lfixture/Function<Ljava/lang/Integer;Ljava/lang/Integer;[I>;"
        if args.reference_carrier:
            interface = "Lfixture/Function<Lfixture/Carrier;Ljava/lang/Boolean;>;"
        if args.array_unit or args.scalar_unit:
            interface = "Lfixture/Function<Lfixture/Carrier;Lkotlin/Unit;>;"
        if args.parameterized_reference:
            interface = interface.replace("Lfixture/Carrier;", "Lfixture/Carrier<+TS;>;" if args.open_reference else "Lfixture/Carrier<Lfixture/Tag;>;")
        input_signature = b"<S:Ljava/lang/Object;>Ljava/lang/Object;" + interface.encode()
        if args.result_only or args.generic_input or args.zero_arity or args.standard_long:
            input_signature = (b"<S:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;"
                               + interface.encode())
        prototype = "(" + "Ljava/lang/Object;" * (1 if args.boxed_integer or args.result_only or args.reference_carrier or args.array_unit or args.scalar_unit else 3 if args.mixed or args.generic_input else 2) + ")Ljava/lang/Object;"
        if args.zero_arity:
            prototype = "()Ljava/lang/Object;"
        if args.standard_long:
            prototype = "(Ljava/lang/Object;)Ljava/lang/Object;"
        if args.named_primitive:
            prototype = "(Lfixture/Context;Ljava/lang/Object;Ljava/lang/Object;Lfixture/Callback;Lfixture/Box;)Ljava/lang/Object;"
        member = "apply" if args.named_primitive else "invoke"
        signature_edit = change_signature(originals / "fixture/Receiver.class",
                                          original_signature, input_signature)
        flag_edit = bridge_flag(originals / "fixture/Receiver.class", member,
                                prototype,
                                original_flags=0x1, input_flags=0x1041)
        (work / "owned-input-encoding.json").write_text(
            json.dumps({"signature": signature_edit, "flags": flag_edit}, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        original_records = class_identity.compiler_classes(originals) if args.standard_double or args.parameterized_reference else None
        harness_hashes = {p.relative_to(harness_dir).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sorted(harness_dir.rglob("*.class"))} if args.standard_double or args.parameterized_reference else None
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               harness, "original"], "baseline")
        counts = ((104, 23, 8) if args.scalar_unit else
                  (159, 75, 8) if args.array_unit else
                  (87, 58, 6) if args.reference_carrier else
                  (453, 134, 8) if args.primitive_array_result else
                  (360, 1444, 9) if args.named_primitive else
                  (270, 545, 11) if args.standard_float else
                  (108, 113, 12) if args.standard_long else (21, 5, 8) if args.zero_arity else
                  (162, 491, 16) if args.generic_input else (126, 131, 8) if args.result_only else
                  (40, 8, 8) if args.boxed_integer else (32, 29, 10) if args.generic_result else
                  (16, 60, 9) if args.mixed else (48, 13, 11))
        expected = "values=%d,identities=%d,failures=%d" % counts
        if baseline.splitlines() != [expected, "reflection=original"]:
            raise RuntimeError("Owned value-carrier baseline missed expected cases")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", args.android_jar.resolve())
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed value-carrier input identities")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["projected_method_count"] != 1
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"):
            raise RuntimeError("Value-carrier recovery lost original bodies or conservative coverage")
        bridge = "Lfixture/Receiver;->" + member + prototype
        projections = coverage.get("generic_signature_projections", [])
        if (len(projections) != 1 or not projections[0].get("metadata_differences")
                or projections[0].get("original_signature") != input_signature.decode()
                or len(projections[0]["changes"]) != 1):
            raise RuntimeError("Value-carrier interface projection lost original metadata")
        change = projections[0]["changes"][0]
        if (change["kind"] != "checked-value-carrier-interface" or change["scope_origin"] != bridge
                or change["original_type"] != interface
                or change["source_type"] != "Lfixture/Function;"):
            raise RuntimeError("Value-carrier projection lost its exact original contract witness")
        row = next(row for row in coverage["methods"] if row["identity"] == bridge)
        unbox_methods = ([] if args.result_only or args.zero_arity else
                         ["Lfixture/Carrier;->unwrap()I"] if args.scalar_unit else
                         ["Lfixture/Carrier;->unwrap()[F"] if args.array_unit else
                         ["Lfixture/Carrier;->unwrap()Lfixture/Payload;"] if args.reference_carrier else
                         ["Ljava/lang/Number;->intValue()I"] * 2 if args.primitive_array_result else
                         ["Ljava/lang/Number;->floatValue()F"] * 2 if args.named_primitive else
                         ["Ljava/lang/Number;->floatValue()F"] if args.standard_float else
                         ["Ljava/lang/Number;->doubleValue()D"] if args.standard_double else
                         ["Ljava/lang/Number;->longValue()J"] if args.standard_long else
                         ["Ljava/lang/Number;->intValue()I"] if args.boxed_integer or args.generic_input else
                         ["Lfixture/CarrierB;->unwrap()J"] if args.mixed else
                         ["Lfixture/CarrierA;->unwrap()J", "Lfixture/CarrierB;->unwrap()J"])
        has_box = not (args.mixed or args.generic_result or args.boxed_integer or args.generic_input
                       or args.standard_long or args.standard_float or args.named_primitive or args.primitive_array_result or args.reference_carrier or args.array_unit or args.scalar_unit)
        if (row["projection_kind"] != "value-carrier-interface-bridge" or row["source_identity"] != bridge
                or not row.get("metadata_differences") or row["bridge_target"] not in methods
                or row["bridge_dispatch"] not in methods
                or (not has_box and "box_method" in row)
                or (has_box and row.get("box_method") not in methods)
                or row["unbox_methods"] != unbox_methods):
            raise RuntimeError("Value-carrier report lost original dispatch, unboxing or boxing ABI")
        if (args.array_unit or args.scalar_unit) and row.get("result_field") != "Lkotlin/Unit;->INSTANCE:Lkotlin/Unit;":
            raise RuntimeError("Array carrier bridge lost the original post-call singleton field")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if actual_classes != classes or not set(methods).issubset(actual_methods):
            raise RuntimeError("Recompiled value-carrier source lost an original descriptor")
        if args.standard_double or args.parameterized_reference:
            helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                       for r in coverage.get("generated_source_helpers", [])}
            if set(actual_methods) != set(methods) | helpers:
                raise RuntimeError("Carrier source added an undisclosed runtime slot")
            actual_records = class_identity.compiler_classes(rebuilt)
            for owner in classes:
                before, after = original_records[owner], actual_records[owner]
                for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                    wanted = original_signature.decode() if owner == "Lfixture/Receiver;" and key == "signature" else before.get(key)
                    if after.get(key) != wanted:
                        raise RuntimeError("Carrier class/field metadata changed beyond the raw source edge: " + owner + ":" + key)
                for identity, metadata in before["methods"].items():
                    wanted = dict(metadata)
                    if identity == bridge:
                        wanted["access"] = 0x1
                    if after["methods"][identity] != wanted:
                        raise RuntimeError("Carrier method metadata changed beyond the bridge flag: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir))),
                             harness, "recovered"], "recovered-execution")
        if actual.splitlines() != [baseline.splitlines()[0], "reflection=reported-raw-interface"]:
            raise RuntimeError("Value-carrier source changed primitive values, effects, identity or failures")
        negative = work / "negative-originals"
        for source in originals.rglob("*.class"):
            destination = negative / source.relative_to(originals)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(source.read_bytes())
        if args.array_unit or args.scalar_unit:
            receipt = change_signature(negative / "fixture/Receiver.class", input_signature,
                                       input_signature.replace(b"Lkotlin/Unit;", b"Ljava/lang/String;"))
        elif args.reference_carrier:
            receipt = change_signature(negative / "fixture/Receiver.class", input_signature,
                                       input_signature.replace(b"Ljava/lang/Boolean;", b"Ljava/lang/Integer;"))
        elif args.primitive_array_result:
            receipt = change_signature(negative / "fixture/Receiver.class", input_signature,
                                       input_signature.replace(b"[I", b"[J"))
        elif args.named_primitive:
            receipt = change_signature(negative / "fixture/Receiver.class",
                b"(Lfixture/Context;FFLfixture/Callback<-Ljava/lang/Float;>;Lfixture/Box<Ljava/lang/Float;Lfixture/Vector;>;)Ljava/lang/Object;",
                b"(Lfixture/Context;FFLfixture/Callback<-Ljava/lang/Float;>;Lfixture/Box<Ljava/lang/Float;Lfixture/Bound;>;)Ljava/lang/Object;")
        elif args.standard_float:
            receipt = change_signature(negative / "fixture/Receiver.class",
                                       b"(FLfixture/Box<-Ljava/lang/Boolean;>;)Ljava/lang/Object;",
                                       b"(FLfixture/Box<+Ljava/lang/Boolean;>;)Ljava/lang/Object;")
        elif args.standard_double:
            receipt = change_signature(negative / "fixture/Receiver.class", b"(D)TS;", b"(D)TV;")
        elif args.standard_long:
            receipt = change_signature(negative / "fixture/Receiver.class", b"(J)TS;", b"(J)TV;")
        elif args.zero_arity:
            receipt = change_signature(negative / "fixture/Receiver.class", input_signature,
                                       input_signature.replace(b"<Lfixture/CarrierB;", b"<Ljava/lang/String;"))
        elif args.generic_input:
            receipt = change_signature(negative / "fixture/Receiver.class",
                                       b"(Lfixture/Box<TS;>;Lfixture/CarrierA;I)Lfixture/Box<TS;>;",
                                       b"(Lfixture/Box<TV;>;Lfixture/CarrierA;I)Lfixture/Box<TS;>;")
        elif args.result_only:
            receipt = change_signature(negative / "fixture/Receiver.class", b"(TS;)J", b"(TV;)J")
        elif args.boxed_integer:
            receipt = change_signature(negative / "fixture/Receiver.class", input_signature,
                                       input_signature.replace(b"<Ljava/lang/Integer;", b"<Ljava/lang/Boolean;"))
        elif args.generic_result:
            receipt = change_signature(negative / "fixture/Receiver.class",
                                       b"(JJ)Lfixture/Box<Lfixture/CarrierA;>;",
                                       b"(JJ)Lfixture/Box<Lfixture/CarrierB;>;")
        else:
            receipt = change_signature(negative / "fixture/Receiver.class", input_signature,
                                       input_signature.replace(b"<Lfixture/CarrierA;Lfixture/CarrierB;",
                                                               b"<Lfixture/CarrierB;Lfixture/CarrierA;"))
        (work / "negative-metadata-change.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = verify.dex(sorted(negative.rglob("*.class")), work / "negative-dex",
                                  args.android_jar.resolve())
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic inheritance" not in result.stderr:
            raise RuntimeError("Changed carrier contract with identical Code and runtime descriptors was accepted")
        if args.open_reference:
            conversion = work / "generic-conversion-originals"
            for source in originals.rglob("*.class"):
                destination = conversion / source.relative_to(originals)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(source.read_bytes())
            result_type = "[F" if args.array_unit else "Lfixture/Payload;"
            receipt = add_parameter_signature(conversion / "fixture/Carrier.class",
                "<R:Ljava/lang/Object;>()" + result_type, method_name="unwrap", prototype="()" + result_type)
            (work / "generic-conversion-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
            conversion_dex = verify.dex(sorted(conversion.rglob("*.class")), work / "generic-conversion-dex",
                                        args.android_jar.resolve())
            rejected = work / "generic-conversion-rejected"
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(conversion_dex), "--classpath",
                                     str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / "generic-conversion-recovery.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("A generic conversion entered the fixed reference conversion proof")
            if hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash:
                raise RuntimeError("Original SDK declarations changed")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted(originals.rglob("*.class"))}:
            raise RuntimeError("Owned original value-carrier input changed")
        if (args.standard_double or args.parameterized_reference) and harness_hashes != {
                p.relative_to(harness_dir).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in sorted(harness_dir.rglob("*.class"))}:
            raise RuntimeError("Original value-carrier JVM harness bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": counts[0], "identity_cases": counts[1], "ordered_failure_cases": counts[2],
                    "original_code_and_descriptors": "unchanged", "wide_register_order": "matched",
                    "checked_virtual_unboxing": "matched", "boxing_after_target_success": "matched",
                    "typed_primitive_calls": "no unboxing or boxing", "metadata_counterexample": "rejected",
                    "raw_interface_and_bridge_flags": "reported", "coverage": "partial",
                    "jvm_output": "matched except reported reflection"}
        if args.open_reference:
            evidence["scoped_carrier_argument"] = "original owner formal and upper wildcard checked before disclosed raw source projection"
            evidence["conversion_result"] = "original literal reference type independent of carrier formals"
            evidence["same_Code_generic_conversion_counterexample"] = "rejected"
            evidence["complete_metadata_input_harness_and_sdk"] = "unchanged beyond disclosed raw edge and bridge flags"
        if args.standard_double:
            evidence["Number_doubleValue_virtual_override"] = "original ordered effects and failures matched"
            evidence["signed_zero_infinities_and_nan_payload_bits"] = "matched"
            evidence["complete_class_field_and_method_metadata"] = "checked beyond reported raw edge and bridge flags"
            evidence["original_harness_bytes"] = "unchanged"
        if args.mixed:
            evidence["boxing_after_target_success"] = "no boxing call introduced"
            evidence["checked_reference_arguments"] = "unchanged, including null"
            evidence["reference_return_identity"] = "matched"
        if args.primitive_array_result:
            evidence["boxing_after_target_success"] = "no boxing call introduced"
            evidence["primitive_register_order"] = "matched"
            evidence["array_reference_identity_and_mutation"] = "original result retained, including null"
            evidence["array_result_and_field_reflection"] = "unchanged"
            evidence["Number_intValue_virtual_override"] = "original ordered effects and failures matched"
            evidence.pop("wide_register_order")
        if args.reference_carrier:
            evidence["boxing_after_target_success"] = "original boxing remains inside the typed body"
            evidence["reference_unboxing_order"] = "original virtual conversion before target"
            evidence["reference_identity_and_mutation"] = "original payload retained, including null"
            evidence["unbox_subclass_override"] = "original virtual effects and failures matched"
            evidence["typed_reference_calls"] = "no additional unboxing"
            evidence["target_and_field_reflection"] = "unchanged"
            evidence.pop("wide_register_order")
            evidence.pop("typed_primitive_calls")
        if args.array_unit or args.scalar_unit:
            evidence["boxing_after_target_success"] = "original Unit field read after the void target"
            if args.array_unit:
                evidence["array_identity_and_float_bits"] = "original array and raw bit mutation retained"
            else:
                evidence["scalar_values_and_mutation"] = "original signed integer result and mutable carrier retained"
            evidence["singleton_initialization_order"] = "after successful target only, independently observed"
            evidence["converter_subclass_override"] = "original virtual effects and failures matched"
            evidence["typed_void_calls"] = "no additional conversion or singleton field read"
            evidence["target_converter_and_field_reflection"] = "unchanged"
            evidence.pop("wide_register_order")
            evidence.pop("typed_primitive_calls")
        if args.result_only or args.zero_arity:
            evidence["checked_virtual_unboxing"] = "no unboxing call introduced"
            evidence["factory_allocation_and_mutation"] = "independent original instances"
            if args.result_only:
                evidence["scoped_reference_arguments"] = "unchanged, complete original target scope"
            else:
                evidence["zero_argument_dispatch"] = "one original self virtual call"
        if args.generic_input:
            evidence["complete_generic_input_and_result"] = "same original declaration scope"
            evidence["boxing_after_target_success"] = "no boxing call introduced"
            evidence["typed_primitive_calls"] = "no unboxing added"
            evidence["Number_intValue_virtual_override"] = "original effects and failures matched"
            evidence["primitive_register_order"] = "matched"
            evidence.pop("wide_register_order")
        if args.standard_long:
            evidence["scoped_target_result"] = "complete original scope and identity retained"
            evidence["boxing_after_target_success"] = "no boxing call introduced"
            evidence["typed_primitive_calls"] = "no unboxing added"
            if not args.standard_double:
                evidence["Number_longValue_virtual_override"] = "original effects and exceptions matched"
        if args.standard_float:
            evidence.pop("wide_register_order")
            evidence["primitive_register_order"] = "matched"
            evidence["floating_point_bits"] = "signed zero, infinities and NaN payload retained"
            evidence["complete_generic_input"] = "original contravariant bounds retained"
            evidence["boxing_after_target_success"] = "no boxing call introduced"
            evidence["Number_floatValue_virtual_override"] = "original effects and exceptions matched"
        if args.named_primitive:
            evidence.pop("wide_register_order")
            evidence["fixed_and_bounded_generic_inputs"] = "complete original substitution and identity retained"
            evidence["primitive_register_order"] = "matched after both original conversions"
            evidence["Number_floatValue_virtual_overrides"] = "original call order and exceptions matched"
            evidence["boxing_after_target_success"] = "no boxing call introduced"
        if args.parameterized_reference:
            evidence["complete_scoped_carrier_argument" if args.open_reference else "complete_closed_carrier_argument"] = "retained for validation, disclosed raw source edge"
            evidence["plain_reference_conversion_result"] = "unchanged original declaration, no formal inferred"
            evidence["full_metadata_and_inventory"] = "unchanged except reported Receiver edge, bridge flags and helpers"
        if args.generic_result:
            evidence["boxing_after_target_success"] = "no boxing call introduced"
            evidence["generic_target_result_reflection"] = "complete type unchanged"
            evidence["reference_return_identity"] = "matched, including null"
        if args.boxed_integer:
            evidence.pop("wide_register_order")
            evidence["primitive_register_order"] = "matched"
            evidence["boxing_after_target_success"] = "original Integer.valueOf retained inside target"
            evidence["typed_primitive_calls"] = "original boxing call retained, no unboxing added"
            evidence["Number_intValue_virtual_override"] = "called once with original effects and failures"
            evidence["non_Integer_numbers_and_integer_edges"] = "matched"
            evidence["Integer_valueOf_cache_identity"] = "matched"
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
