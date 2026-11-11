#!/usr/bin/env python3
"""Compare an owned original post-call upcast bridge with recovered Java."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def result_cast(path: Path, *, wrong_type: bool = False, primitive_inputs: bool = False,
                reference_inputs: bool = False) -> dict:
    """Edit only the identified owned bridge's Code and debug length records."""
    original = path.read_bytes()
    records = class_records(original)
    strings = records["strings"]
    cast_classes = [index for index, name in records["classes"].items()
                    if strings[name] == b"fixture/Base"]
    if len(cast_classes) != 1:
        raise RuntimeError("Owned bridge lacks one exact Base constant")
    base = cast_classes[0]
    offset = records["pool_end"] + 6
    offset += 2 + 2 * int.from_bytes(original[offset:offset + 2], "big")

    def attributes(position: int, size: int) -> tuple:
        rows = []
        for _ in range(size):
            name, length = struct.unpack_from(">HI", original, position)
            end = position + 6 + length
            rows.append((strings[name], position, end))
            position = end
        return position, rows

    fields = int.from_bytes(original[offset:offset + 2], "big")
    offset += 2
    for _ in range(fields):
        offset, _ = attributes(offset + 8, int.from_bytes(original[offset + 6:offset + 8], "big"))
    count = int.from_bytes(original[offset:offset + 2], "big")
    offset += 2
    selected = []
    identity = (b"create(Ljava/lang/Object;I)Lfixture/Base;" if reference_inputs else
                b"create(II)Lfixture/Base;" if primitive_inputs else b"create()Lfixture/Base;")
    for _ in range(count):
        flags, name, prototype, size = struct.unpack_from(">HHHH", original, offset)
        offset, rows = attributes(offset + 8, size)
        if strings[name] + strings[prototype] == identity:
            if flags != 0x1041:
                raise RuntimeError("Owned result bridge flags changed")
            selected.extend((start, end) for key, start, end in rows if key == b"Code")
    if len(selected) != 1:
        raise RuntimeError("Owned result bridge does not have one Code attribute")
    start, end = selected[0]
    header = original[start:start + 6]
    body = bytearray(original[start + 6:end])
    stack, locals_, size = struct.unpack_from(">HHI", body)
    code = bytes(body[8:8 + size])
    prefix = b"\x2a\x2b\x1c\xb6" if reference_inputs else b"\x2a\x1b\x1c\xb6" if primitive_inputs else b"\x2a\xb6"
    frame = 3 if primitive_inputs or reference_inputs else 1
    original_size = len(prefix) + 3
    cast_pc = original_size - 1
    if stack != frame or locals_ != frame or not code.startswith(prefix) or code[-1:] != b"\xb0":
        raise RuntimeError("Owned bridge no longer performs one self virtual call and return")
    if wrong_type:
        if size != original_size + 3 or code[cast_pc:cast_pc + 3] != b"\xc0" + base.to_bytes(2, "big"):
            raise RuntimeError("Counterexample lacks the original exact upcast")
        body[9 + cast_pc:11 + cast_pc] = records["this_class"].to_bytes(2, "big")
    else:
        if size != original_size:
            raise RuntimeError("Compiler changed the owned original forwarding bridge")
        if int.from_bytes(body[8 + size:10 + size], "big") != 0:
            raise RuntimeError("Owned bridge has an unexpected exception handler")
        position = 10 + size
        nested_count = int.from_bytes(body[position:position + 2], "big")
        position += 2
        for _ in range(nested_count):
            name, length = struct.unpack_from(">HI", body, position)
            data_start = position + 6
            if strings[name] in (b"LocalVariableTable", b"LocalVariableTypeTable"):
                entries = int.from_bytes(body[data_start:data_start + 2], "big")
                if length != 2 + entries * 10:
                    raise RuntimeError("Malformed owned bridge local variable debug records")
                for entry in range(entries):
                    row = data_start + 2 + entry * 10
                    pc, span = struct.unpack_from(">HH", body, row)
                    if pc != 0 or span != size:
                        raise RuntimeError("Unexpected owned bridge local variable range")
                    body[row + 2:row + 4] = (size + 3).to_bytes(2, "big")
            elif strings[name] == b"LineNumberTable":
                entries = int.from_bytes(body[data_start:data_start + 2], "big")
                if length != 2 + entries * 4 or any(
                        int.from_bytes(body[data_start + 2 + row * 4:data_start + 4 + row * 4], "big")
                        != 0 for row in range(entries)):
                    raise RuntimeError("Unexpected owned bridge line records")
            else:
                raise RuntimeError("Unsupported owned bridge debug attribute")
            position = data_start + length
        if position != len(body):
            raise RuntimeError("Owned bridge Code attribute boundary changed")
        body[4:8] = (size + 3).to_bytes(4, "big")
        body[8:8 + size] = code[:-1] + b"\xc0" + base.to_bytes(2, "big") + code[-1:]
    changed = original[:start] + header[:2] + len(body).to_bytes(4, "big") + bytes(body) + original[end:]
    before, after = method_code(original), method_code(changed)
    if before.keys() != after.keys() or any(before[key] != after[key] for key in before if key != identity):
        raise RuntimeError("Result-cast edit changed another original method")
    updated = class_records(changed)
    if (original[:start] != changed[:start] or original[end:] != changed[start + 6 + len(body):]
            or records["strings"] != updated["strings"] or records["classes"] != updated["classes"]):
        raise RuntimeError("Result-cast edit changed declarations or constant pool")
    path.write_bytes(changed)
    return {"method": identity.decode(), "edit": "cast target Receiver" if wrong_type else "insert CHECKCAST Base",
            "runtime_descriptors": "unchanged", "flags": "unchanged", "metadata": "unchanged",
            "other_code_attributes": "unchanged", "before_sha256": hashlib.sha256(original).hexdigest(),
            "after_sha256": hashlib.sha256(changed).hexdigest()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--generic-getter", action="store_true")
    parser.add_argument("--ancestor-chain", action="store_true")
    args = parser.parse_args()
    if args.ancestor_chain:
        args.generic_getter = True
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-result-upcast-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = FIXTURES / ("chained_covariant_getters/java" if args.ancestor_chain else
                              "covariant_reference_getters/java" if args.generic_getter else "upcast_result_bridges/java")
        inputs = sorted(fixture.rglob("*.java"))
        inputs.append(FIXTURES / "value_carrier_bridges/java/fixture/Trace.java")
        verify.compile_local(inputs, originals)
        harness_name = ("ChainedCovariantGetterHarness" if args.ancestor_chain else
                        "CovariantReferenceGetterHarness" if args.generic_getter else "UpcastResultBridgeHarness")
        verify.compile_local([FIXTURES / ("harness/" + harness_name + ".java")], harness,
                             classpath=originals)
        receipt = result_cast(originals / "fixture/Receiver.class")
        (work / "owned-bridge-code-edit.json").write_text(json.dumps(receipt, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_name], "baseline")
        expected = (["identities=" + ("24" if args.ancestor_chain else "18") + ",failures=5",
                     "full-generic-covariance=retained,original-upcast-and-dispatch=matched"] if args.generic_getter else
                    ["identities=19,failures=7",
                     "result-upcast-and-virtual-dispatch=matched,generic-reflection=unchanged"])
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned result upcast baseline missed cases")

        def make_dex(source: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath",
                        args.android_jar.resolve(), "--output", destination,
                        *sorted(source.rglob("*.class"))], "d8-" + destination.name)
            dex = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned result upcast fixture did not retain one DEX")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the original result bridge inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        bridge = "Lfixture/Receiver;->create()Lfixture/Base;"
        row = next(row for row in coverage["methods"] if row["identity"] == bridge)
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["projected_method_count"] != 1 or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial" or coverage.get("generic_signature_projections")
                or row["projection_kind"] != "generic-superclass-bridge"
                or row["bridge_target"] != "Lfixture/Receiver;->create()Lfixture/Leaf;"):
            raise RuntimeError("Result upcast recovery lost the original generic or method contract")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if (actual_classes != classes or not set(methods).issubset(actual_methods)
                or set(actual_methods) - set(methods) != helpers):
            raise RuntimeError("Result upcast recompile changed original method ownership")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             harness_name], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Result upcast changed identity, dispatch, effects or exceptions")
        negative = work / "negative-originals"
        for source in originals.rglob("*.class"):
            destination = negative / source.relative_to(originals)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(source.read_bytes())
        if args.ancestor_chain:
            target = negative / "fixture/Upper.class"
            before = target.read_bytes()
            receipt = change_signature(target,
                b"<K:Ljava/lang/Object;L:Ljava/lang/Object;>Lfixture/Middle<TL;TK;>;",
                b"<K:Ljava/lang/Object;L:Ljava/lang/Object;>Lfixture/Middle<TK;TL;>;")
            if method_code(before) != method_code(target.read_bytes()):
                raise RuntimeError("Ancestor argument counterexample changed Code or descriptors")
        else:
            receipt = (change_signature(negative / "fixture/Leaf.class",
            b"<T:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Base<TT;>;",
            b"<T:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Base<Ljava/lang/Object;>;")
            if args.generic_getter else result_cast(negative / "fixture/Receiver.class", wrong_type=True))
        (work / "negative-code-change.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = make_dex(negative, work / "negative-dex")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if (result.returncode == 0 or rejected.exists()
                or ("generic inheritance" if args.generic_getter else "generic bridge") not in result.stderr):
            raise RuntimeError("Changed result cast was accepted as a proven upcast")
        if args.generic_getter:
            negative_java = work / "negative-java"
            compiler_inputs = []
            for source in inputs:
                content = source.read_text()
                if source.name == "Leaf.java":
                    if content.count("implements Base<T>") != 1:
                        raise RuntimeError("Owned Leaf source ancestry changed")
                    if not args.ancestor_chain:
                        content = content.replace("implements Base<T>", "implements Base<Object>", 1)
                if args.ancestor_chain and source.name == "Upper.java":
                    if content.count("extends Middle<L, K>") != 1:
                        raise RuntimeError("Owned intermediate superclass changed")
                    content = content.replace("extends Middle<L, K>", "extends Middle<K, L>", 1)
                destination = negative_java / "fixture" / source.name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                compiler_inputs.append(str(destination))
            compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                str(work / "negative-compiler"), *compiler_inputs], text=True,
                capture_output=True, timeout=300, env=verify.environment)
            (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
            if compiler.returncode == 0:
                raise RuntimeError("javac accepted changed full generic getter ancestry")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted(originals.rglob("*.class"))}:
            raise RuntimeError("Owned original result bridge changed during recovery")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "identity_cases": 24 if args.ancestor_chain else 18 if args.generic_getter else 19,
                    "ordered_failure_cases": 5 if args.generic_getter else 7, "result_upcast": "proven original",
                    "original_descriptors_and_flags": "unchanged", "generic_reflection": "unchanged",
                    "virtual_dispatch": "matched", "cast_counterexample": "rejected",
                    "coverage": "partial", "jvm_output": "matched"}
        if args.generic_getter:
            evidence["ancestry_counterexample"] = "NeverD and javac rejected unchanged-erasure generic drift"
            evidence["cast_counterexample"] = "covered separately by the original upcast profile"
        if args.ancestor_chain:
            evidence["source_superclass_edges"] = 3
            evidence["composed_ancestor_formals"] = "retained across argument permutation"
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
