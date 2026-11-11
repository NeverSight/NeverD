#!/usr/bin/env python3
"""Compare generic metadata projections against independent JVM bytecode."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import json
from pathlib import Path
import struct
import tempfile

try:
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk


def encode_fixture_signatures(path: Path, input_descriptor: bytes = b"Ljava/lang/Object;",
                              narrowed_descriptor: bytes = b"Ljava/lang/String;") -> None:
    """Change only generic metadata in the owned compiler-produced baseline."""
    data = path.read_bytes()
    if data[:4] != b"\xca\xfe\xba\xbe":
        raise RuntimeError("Expected the compiled checked-erasure fixture")
    count = int.from_bytes(data[8:10], "big")
    offset, index = 10, 1
    utf8 = {}
    while index < count:
        tag = data[offset]
        offset += 1
        if tag == 1:
            size = int.from_bytes(data[offset:offset + 2], "big")
            offset += 2
            utf8[index] = data[offset:offset + size]
            offset += size
        elif tag in (3, 4, 9, 10, 11, 12, 17, 18):
            offset += 4
        elif tag in (5, 6):
            offset += 8
            index += 1
        elif tag in (7, 8, 16, 19, 20):
            offset += 2
        elif tag == 15:
            offset += 3
        else:
            raise RuntimeError("Unexpected fixture constant-pool tag")
        index += 1
    pool_end = offset
    signature_name = next(key for key, value in utf8.items() if value == b"Signature")
    offset += 6
    interfaces = int.from_bytes(data[offset:offset + 2], "big")
    offset += 2 + interfaces * 2

    def skip_attributes(position: int, attributes: int) -> int:
        for _ in range(attributes):
            size = int.from_bytes(data[position + 2:position + 6], "big")
            position += 6 + size
        return position

    fields = int.from_bytes(data[offset:offset + 2], "big")
    offset += 2
    for _ in range(fields):
        attributes = int.from_bytes(data[offset + 6:offset + 8], "big")
        offset = skip_attributes(offset + 8, attributes)
    methods = int.from_bytes(data[offset:offset + 2], "big")
    offset += 2
    target = None
    for _ in range(methods):
        _, name, descriptor, attributes = struct.unpack_from(">HHHH", data, offset)
        end = skip_attributes(offset + 8, attributes)
        if utf8[name] == b"apply" and utf8[descriptor] == b"(" + input_descriptor + b")Ljava/lang/String;":
            if target is not None:
                raise RuntimeError("Duplicate fixture implementation")
            position = offset + 8
            for _ in range(attributes):
                if int.from_bytes(data[position:position + 2], "big") == signature_name:
                    raise RuntimeError("Fixture implementation already has a Signature")
                position = skip_attributes(position, 1)
            target = offset, end, attributes
        offset = end
    if target is None:
        raise RuntimeError("Missing fixture implementation")
    start, end, attributes = target
    signature = b"(" + narrowed_descriptor + b")Ljava/lang/String;"
    record = b"\x01" + len(signature).to_bytes(2, "big") + signature
    method_attribute = struct.pack(">HIH", signature_name, 2, count)
    tail = (data[pool_end:start + 6] + (attributes + 1).to_bytes(2, "big")
            + data[start + 8:end] + method_attribute + data[end:])
    data = data[:8] + (count + 1).to_bytes(2, "big") + data[10:pool_end] + record + tail
    original = b"Ljava/lang/Object;Lfixture/Transform<" + input_descriptor + b"Ljava/lang/String;>;"
    narrowed = b"Ljava/lang/Object;Lfixture/Transform<" + narrowed_descriptor + b"Ljava/lang/String;>;"
    old = b"\x01" + len(original).to_bytes(2, "big") + original
    new = b"\x01" + len(narrowed).to_bytes(2, "big") + narrowed
    if data.count(old) != 1:
        raise RuntimeError("Expected one fixture interface Signature")
    path.write_bytes(data.replace(old, new, 1))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--android-jar", type=Path)
    variants = parser.add_mutually_exclusive_group()
    variants.add_argument("--static-carrier", action="store_true",
                          help="Verify a class type variable retained in static carrier metadata")
    variants.add_argument("--marked-carrier", action="store_true",
                          help="Verify private carrier helpers, static state and empty marker interfaces")
    variants.add_argument("--interface-parameter", action="store_true",
                          help="Preserve the bridge's interface cast before the body's concrete cast")
    args = parser.parse_args()
    args.static_carrier = args.static_carrier or args.marked_carrier
    if args.interface_parameter and (not args.android_jar or not args.android_jar.is_file()):
        parser.error("--interface-parameter requires an existing --android-jar for subtype declarations")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-checked-erasure-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), work / "unused-d8", 300)
        fixture = FIXTURES / ("static_carrier_markers" if args.marked_carrier else
                              "static_carrier" if args.static_carrier else "checked_erasure")
        inputs = sorted((fixture / "java").rglob("*.java"))
        harness_name = ("MarkedCarrierHarness" if args.marked_carrier else
                        "StaticCarrierHarness" if args.static_carrier else "CheckedErasureHarness")
        harness = FIXTURES / ("harness/" + harness_name + ".java")
        smali = fixture / "smali"
        if args.interface_parameter:
            transformed = work / "owned-input"
            inputs = []
            for source in sorted((fixture / "java").rglob("*.java")):
                content = source.read_text().replace("Transform<Object, String>", "Transform<CharSequence, String>")
                content = content.replace("apply(Object input)", "apply(CharSequence input)")
                target = transformed / "java" / source.relative_to(fixture / "java")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content)
                inputs.append(target)
            smali = transformed / "smali"
            smali.mkdir()
            for source in sorted((fixture / "smali").rglob("*.smali")):
                content = source.read_text()
                if source.name == "CheckedTransform.smali":
                    content = content.replace("apply(Ljava/lang/Object;)Ljava/lang/String;",
                                              "apply(Ljava/lang/CharSequence;)Ljava/lang/String;")
                    content = content.replace("    invoke-virtual {p0, p1},", """    check-cast p1, Ljava/lang/CharSequence;
    invoke-virtual {p0, p1},""", 1)
                (smali / source.name).write_text(content)
            content = harness.read_text().replace("target.apply(input)", "target.apply((CharSequence) input)")
            content = content.replace("CheckedTransform.calls = 0;", """if (!bridge && input != null && !(input instanceof CharSequence)) continue;
                CheckedTransform.calls = 0;""", 1)
            content = content.replace("return new Object();", "return new StringBuilder();", 1)
            content = content.replace("absent.apply(invalidArgument())", "absent.apply((CharSequence) invalidArgument())")
            content = content.replace("implementation.getParameterTypes()[0] != Object.class",
                                      "implementation.getParameterTypes()[0] != CharSequence.class")
            content = content.replace("} catch (ClassCastException error) {", """} catch (ClassCastException error) {
                    String expectedCast = input instanceof CharSequence ? "java.lang.String" : "java.lang.CharSequence";
                    if (!error.getMessage().contains(expectedCast))
                        throw new AssertionError("bridge and body cast stages changed", error);""", 1)
            harness = transformed / harness.name
            harness.write_text(content)
        originals = work / "originals"
        verify.compile_local(inputs, originals)
        classes, methods = compiler_inventory(verify, originals)
        baseline_dir = work / "baseline"
        verify.compile_local([*inputs, harness], baseline_dir)
        if args.static_carrier:
            path = baseline_dir / "fixture/Carrier.class"
            original = b"(Ljava/lang/Object;Ljava/util/List<Ljava/lang/Object;>;)Ljava/lang/Object;"
            encoded = b"(Ljava/lang/Object;Ljava/util/List<TT;>;)Ljava/lang/Object;"
            old = b"\x01" + len(original).to_bytes(2, "big") + original
            new = b"\x01" + len(encoded).to_bytes(2, "big") + encoded
            data = path.read_bytes()
            if data.count(old) != 1:
                raise RuntimeError("Expected one fixture static carrier Signature")
            path.write_bytes(data.replace(old, new, 1))
        else:
            encode_fixture_signatures(baseline_dir / "fixture/CheckedTransform.class",
                                      b"Ljava/lang/CharSequence;" if args.interface_parameter else b"Ljava/lang/Object;")
        baseline = verify.run([verify.java, "-cp", baseline_dir, harness_name], "baseline")
        output = work / "recovered"
        dependencies = ["--classpath", args.android_jar] if args.interface_parameter else []
        verify.run([args.neverd.resolve(), "mobile", smali, *dependencies, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes)
                or coverage["unrecovered_method_count"] != 0
                or coverage.get("projected_method_count", 0) != (0 if args.static_carrier else 1)
                or coverage["status"] != "partial"):
            raise RuntimeError("Checked erasure changed inventory or coverage status")
        projections = coverage.get("generic_signature_projections", [])
        expected = ({"(Ljava/lang/Object;Ljava/util/List<TT;>;)Ljava/lang/Object;"} if args.static_carrier else
                    {"Ljava/lang/Object;Lfixture/Transform<Ljava/lang/String;Ljava/lang/String;>;",
                     "(Ljava/lang/String;)Ljava/lang/String;"})
        kinds = ({"static-carrier-type-variable"} if args.static_carrier else
                 {"checked-erased-parameter", "checked-erased-interface-argument"})
        if ({row["original_signature"] for row in projections} != expected
                or {change["kind"] for row in projections for change in row["changes"]}
                != kinds
                or any(not row.get("metadata_differences") for row in projections)):
            raise RuntimeError("Signature differences are not explicitly reported")
        rebuilt = work / "rebuilt"
        verify.compile_local([*sorted((output / "sources").rglob("*.java")), harness], rebuilt)
        _, rebuilt_methods = compiler_inventory(verify, rebuilt)
        if not set(methods).issubset(rebuilt_methods):
            raise RuntimeError("Recovered source lost original runtime descriptors")
        actual = verify.run([verify.java, "-cp", rebuilt, harness_name], "recovered-execution")
        before, after = baseline.splitlines(), actual.splitlines()
        original_metadata = "generic=T" if args.static_carrier else "generic=java.lang.String:java.lang.String"
        source_metadata = "generic=java.lang.Object" if args.static_carrier else "generic=java.lang.Object:java.lang.Object"
        if args.interface_parameter:
            source_metadata = "generic=java.lang.CharSequence:java.lang.CharSequence"
        if before[-1] != original_metadata or after[-1] != source_metadata:
            raise RuntimeError("Generic reflection does not match the recorded source change")
        if before[:-1] != after[:-1]:
            raise RuntimeError("Checked erasure changed JVM behavior")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "input_probes": 6, "bridge_count": 1, "original_descriptors": "preserved",
                    "body_cast_and_side_effect_order": "matched", "null_receiver_order": "matched",
                    "reference_identity": "matched", "generic_reflection_difference": "confirmed"}
        if args.static_carrier:
            evidence.update(input_probes=4, bridge_count=0, wrapper_reference_identity="matched",
                            original_class_formal="preserved")
            evidence.pop("body_cast_and_side_effect_order")
        if args.marked_carrier:
            evidence.update(private_method_access="preserved", transitive_marker_interfaces="preserved",
                            static_state_and_initialization="matched", static_call_count=8)
        if args.interface_parameter:
            evidence.update(interface_parameter="preserved", bridge_and_body_cast_stages="matched")
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
