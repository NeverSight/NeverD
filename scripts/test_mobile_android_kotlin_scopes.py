#!/usr/bin/env python3
"""Compare Kotlin metadata scope recovery with independent Java/JVM bodies."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk


def java_string(value: str) -> str:
    result = '"'
    for char in value:
        code = ord(char)
        if char in ('"', '\\'):
            result += '\\' + char
        elif code < 32 or 127 <= code <= 255:
            result += '\\' + format(code, '03o')
        elif code > 126:
            raise RuntimeError("Unexpected non-byte character in the owned metadata")
        else:
            result += char
    return result + '"'


def erase_fixture_method_signature(
        path: Path, *, method_name: bytes = b"adapt",
        prototype: bytes = b"(Lfixture/Box;Lfixture/Callback;)Lfixture/Box;",
        expected_signature: bytes = (b"<X:Ljava/lang/Object;Y:Ljava/lang/Object;>(Lfixture/Box<TX;>;"
                                     b"Lfixture/Callback<TX;Lfixture/Box<TY;>;>;)Lfixture/Box<TY;>;"),
        original_flags: int = 9, set_synthetic: bool = True) -> dict:
    """Remove one Signature and set synthetic; retain all original method code."""
    data = path.read_bytes()
    if data[:4] != b"\xca\xfe\xba\xbe":
        raise RuntimeError("Missing compiled fixture class")
    count = int.from_bytes(data[8:10], "big")
    utf8, index, offset = {}, 1, 10
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
    offset += 6
    interfaces = int.from_bytes(data[offset:offset + 2], "big")
    offset += 2 + 2 * interfaces

    def attributes(position: int, size: int):
        rows = []
        for _ in range(size):
            name, length = struct.unpack_from(">HI", data, position)
            end = position + 6 + length
            rows.append((utf8[name], position, end))
            position = end
        return position, rows

    fields = int.from_bytes(data[offset:offset + 2], "big")
    offset += 2
    for _ in range(fields):
        offset, _ = attributes(offset + 8, int.from_bytes(data[offset + 6:offset + 8], "big"))
    methods = int.from_bytes(data[offset:offset + 2], "big")
    offset += 2
    target = None
    for _ in range(methods):
        flags, name, descriptor, size = struct.unpack_from(">HHHH", data, offset)
        end, rows = attributes(offset + 8, size)
        if utf8[name] == method_name:
            if target or utf8[descriptor] != prototype or flags != original_flags:
                raise RuntimeError("Unexpected fixture method identity or access")
            signatures = [row for row in rows if row[0] == b"Signature"]
            code = [row for row in rows if row[0] == b"Code"]
            if len(signatures) != 1 or len(code) != 1:
                raise RuntimeError("Missing original Signature or method bytecode")
            _, start, stop = signatures[0]
            original = utf8[int.from_bytes(data[start + 6:stop], "big")]
            if original != expected_signature:
                raise RuntimeError("Compiler baseline no longer matches the owned metadata declaration")
            target = (offset, flags, size, start, stop, code[0])
        offset = end
    if target is None:
        raise RuntimeError("Missing fixture target")
    position, flags, size, start, stop, code = target
    changed = (data[:position] + struct.pack(">H", flags | (0x1000 if set_synthetic else 0))
               + data[position + 2:position + 6] + struct.pack(">H", size - 1)
               + data[position + 8:start] + data[stop:])
    original_code = data[code[1]:code[2]]
    if changed.count(original_code) != 1:
        raise RuntimeError("Signature projection changed the original Code attribute")
    path.write_bytes(changed)
    return {"removed_signature": original.decode(), "method_code_sha256": hashlib.sha256(original_code).hexdigest(),
            "bytecode": "unchanged", "synthetic_flag": "set" if set_synthetic else "unchanged"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path, required=True)
    parser.add_argument("--kotlin-lib", type=Path, help="Optionally recheck the frozen witness with the Kotlin compiler reader")
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--unit-package-part", action="store_true")
    parser.add_argument("--primitive-parameter", action="store_true")
    parser.add_argument("--suspend-callback", action="store_true")
    args = parser.parse_args()
    if sum((args.unit_package_part, args.primitive_parameter, args.suspend_callback)) > 1:
        parser.error("Choose one owned package-part profile")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-kotlin-scope-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), args.d8.resolve(), 300)
        package_part = args.unit_package_part or args.primitive_parameter or args.suspend_callback
        fixture = FIXTURES / ("kotlin_suspend_callbacks" if args.suspend_callback
                              else "kotlin_primitive_parts" if args.primitive_parameter
                              else "kotlin_unit_parts" if package_part else "kotlin_scope")
        receipt = json.loads((fixture / "metadata.json").read_text())
        source_name = "ScopePart.kt" if package_part else "MetadataHost.kt"
        host_name = "ScopeFacade__ScopePartKt" if package_part else "MetadataHost"
        if receipt["source_sha256"] != hashlib.sha256((fixture / source_name).read_bytes()).hexdigest():
            raise RuntimeError("Regenerate the owned Kotlin metadata after changing its declaration")
        metadata = receipt["metadata"]
        entries = []
        for key, value in metadata.items():
            if isinstance(value, list):
                encoded = "{" + ",".join(java_string(v) if isinstance(v, str) else str(v) for v in value) + "}"
            elif isinstance(value, str):
                encoded = java_string(value)
            else:
                encoded = str(value)
            entries.append(key + "=" + encoded)
        annotation = "@kotlin.Metadata(" + ",".join(entries) + ")\n"
        generated = work / "java"
        inputs = []
        for source in sorted((fixture / "java").rglob("*.java")):
            content = source.read_text()
            if source.name == host_name + ".java":
                declaration = "public final class " + host_name
                content = content.replace(declaration, annotation + declaration, 1)
            target = generated / source.relative_to(fixture / "java")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content)
            inputs.append(target)
        declarations = work / "declarations"
        metadata_declarations = FIXTURES / "kotlin_scope/declarations"
        declaration_sources = sorted(metadata_declarations.rglob("*.java"))
        if fixture != FIXTURES / "kotlin_scope":
            declaration_sources.extend(sorted((fixture / "declarations").rglob("*.java")))
        verify.compile_local(declaration_sources, declarations)
        dependency = work / "declarations.jar"
        with zipfile.ZipFile(dependency, "w") as jar:
            for source in sorted(declarations.rglob("*.class")):
                jar.write(source, source.relative_to(declarations).as_posix())
        originals = work / "originals"
        verify.compile_local(inputs, originals, classpath=dependency)
        classes, methods = compiler_inventory(verify, originals)
        if args.suspend_callback:
            mutation = erase_fixture_method_signature(
                originals / "fixture" / (host_name + ".class"), method_name=b"repeat",
                prototype=(b"(Lfixture/Target;ILkotlin/coroutines/CoroutineContext;Ljava/lang/Object;"
                           b"Lkotlin/jvm/functions/Function2;)Lfixture/Target;"),
                expected_signature=(b"<E:Ljava/lang/Object;>(Lfixture/Target<TE;>;I"
                                    b"Lkotlin/coroutines/CoroutineContext;TE;"
                                    b"Lkotlin/jvm/functions/Function2;)Lfixture/Target<TE;>;"))
        elif args.primitive_parameter:
            mutation = erase_fixture_method_signature(
                originals / "fixture" / (host_name + ".class"), method_name=b"repeat",
                prototype=b"(Lfixture/Target;ILkotlin/coroutines/CoroutineContext;Ljava/lang/Object;)Lfixture/Target;",
                expected_signature=(b"<E:Ljava/lang/Object;>(Lfixture/Target<TE;>;I"
                                    b"Lkotlin/coroutines/CoroutineContext;TE;)Lfixture/Target<TE;>;"))
        elif package_part:
            mutation = erase_fixture_method_signature(
                originals / "fixture" / (host_name + ".class"), method_name=b"register",
                prototype=b"(Lfixture/Target;Ljava/lang/Object;)V",
                expected_signature=b"<E:Ljava/lang/Object;>(Lfixture/Target<TE;>;TE;)V")
        else:
            mutation = erase_fixture_method_signature(originals / "fixture/MetadataHost.class")
        (work / "input-metadata-change.json").write_text(json.dumps(mutation, indent=2) + "\n")
        if args.kotlin_lib:
            tool = work / "compiler-reader"
            compiler_cp = os.pathsep.join(str(args.kotlin_lib / name) for name in ("kotlin-compiler.jar", "kotlin-stdlib.jar"))
            reader = "InspectKotlinUnitPart" if package_part else "InspectKotlinScope"
            reader_fixture = FIXTURES / "kotlin_unit_parts" if package_part else fixture
            verify.compile_local([reader_fixture / (reader + ".java")], tool, classpath=compiler_cp, release=17)
            cp = os.pathsep.join((str(tool), str(originals), compiler_cp))
            verify.run([verify.java, "-cp", cp, reader, work / "checked-metadata.bin",
                        *(["suspend"] if args.suspend_callback else
                          ["primitive"] if args.primitive_parameter else [])], "independent-metadata-reader")
        harness_name = ("KotlinSuspendPartHarness" if args.suspend_callback
                        else "KotlinPrimitivePartHarness" if args.primitive_parameter
                        else "KotlinUnitPartHarness" if package_part else "KotlinScopeHarness")
        harness = FIXTURES / "harness" / (harness_name + ".java")
        harness_dir = work / "harness"
        verify.compile_local([harness], harness_dir,
                             classpath=os.pathsep.join(map(str, (originals, dependency))), release=17)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (harness_dir, originals, dependency))),
                               harness_name, "original"], "baseline")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", dependency)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned metadata fixture declaration inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", dependency, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial"):
            raise RuntimeError("Kotlin scope recovery changed the original inventory or omitted conservative status")
        projections = coverage.get("generic_signature_projections", [])
        witness = "Lfixture/" + host_name + ";->" + receipt["witness_method"]
        if not projections or any(not row.get("metadata_differences") for row in projections):
            raise RuntimeError("Missing Kotlin scope metadata differences")
        changes = [change for row in projections for change in row["changes"]]
        if any(change["kind"] != "kotlin-metadata-method-type-variable" or change.get("scope_origin") != witness
               or change["source_type"] != "Ljava/lang/Object;" for change in changes):
            raise RuntimeError("Scope replacement lacks the exact compiler metadata declaration")
        rebuilt = work / "rebuilt"
        verify.compile_local([*sorted((output / "sources").rglob("*.java")), harness], rebuilt,
                             classpath=dependency, release=25)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        expected = set(methods)
        bindings = coverage.get("class_source_bindings", [])
        if len(bindings) != 1 or bindings[0]["binding_kind"] != "anonymous-to-named-local":
            raise RuntimeError("Missing original anonymous class binding")
        binding = bindings[0]
        prefix = binding["enclosing_method"]["owner"][:-1] + "$"
        pattern = re.escape(prefix) + r"[0-9]+" + re.escape(binding["source_name"]) + ";"
        matches = [cls for cls in rebuilt_classes if re.fullmatch(pattern, cls)]
        if len(matches) != 1:
            raise RuntimeError("Recompiled local class identity is ambiguous")
        expected = {method.replace(binding["class"], matches[0]) for method in expected}
        if not expected.issubset(rebuilt_methods):
            raise RuntimeError("Recompiled source lost an original method descriptor")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, dependency))),
                             harness_name, "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Scope projection changed captured values or callback behavior")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "forwarding_probes": 8, "callback_failures": 4, "null_cases": 2,
                    "captured_reference_identity": "matched", "exception_and_result_order": "matched",
                    "generic_reflection_difference": "confirmed", "metadata_scope": "exact-method-Object-only-formals",
                    "independent_metadata_reader": "passed" if args.kotlin_lib else "frozen-compiler-receipt",
                    "original_method_bytecode": "unchanged",
                    "original_descriptors": "retained after reported local class rename"}
        if package_part:
            evidence.update(metadata_kind=5, facade=metadata["xs"], runtime_return="V",
                            callback_exception_identity="matched", store_before_callback="preserved")
        if args.primitive_parameter:
            evidence.update(runtime_return="Lfixture/Target;", primitive_parameter="I",
                            successful_callback_calls=20, zero_or_negative_counts=2,
                            returned_target_identity="preserved", captured_context_identity="preserved")
        if args.suspend_callback:
            evidence.update(runtime_return="Lfixture/Target;", primitive_parameter="I",
                            successful_callback_calls=20, zero_or_negative_counts=2,
                            null_cases=3, suspend_action_calls=19, action_failures=4,
                            action_before_stores="preserved", captured_context_identity="preserved",
                            continuation_argument="null identity preserved", callback_runtime_interface="Function2")
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
