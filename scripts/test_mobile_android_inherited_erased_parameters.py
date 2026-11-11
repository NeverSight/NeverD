#!/usr/bin/env python3
"""Verify unchanged class-formal parameters inherited over plain interface defaults."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def add_parameter_signature(path: Path, signature: str, *, method_name: str = "remove",
                            prototype: str = "(Ljava/lang/Object;Ljava/lang/Object;)Z",
                            original_flags: int = 1, code_count: int = 1) -> dict:
    """Add only the identified owned method Signature, retaining all Code."""
    original = path.read_bytes()
    shape = class_records(original)
    strings = shape["strings"]
    offset = shape["pool_end"] + 6
    offset += 2 + 2 * int.from_bytes(original[offset:offset + 2], "big")

    def attributes(position: int, size: int) -> tuple:
        names = []
        for _ in range(size):
            name, length = struct.unpack_from(">HI", original, position)
            names.append(strings[name])
            position += 6 + length
        return position, names

    fields = int.from_bytes(original[offset:offset + 2], "big")
    offset += 2
    for _ in range(fields):
        offset, _ = attributes(offset + 8, int.from_bytes(original[offset + 6:offset + 8], "big"))
    methods = int.from_bytes(original[offset:offset + 2], "big")
    offset += 2
    targets = []
    for _ in range(methods):
        start = offset
        flags, name, descriptor, size = struct.unpack_from(">HHHH", original, start)
        offset, names = attributes(start + 8, size)
        if strings[name] == method_name.encode() and strings[descriptor] == prototype.encode():
            if (flags != original_flags or strings[descriptor] != prototype.encode()
                    or b"Signature" in names or names.count(b"Code") != code_count):
                raise RuntimeError("Owned method changed ABI, access or metadata")
            targets.append((start, offset, size))
    names = [index for index, text in strings.items() if text == b"Signature"]
    if len(targets) != 1 or len(names) > 1:
        raise RuntimeError("Missing exact owned parameter Signature insertion target")
    start, end, size = targets[0]
    count, pool_end = shape["count"], shape["pool_end"]
    encoded = signature.encode()
    # A plain class need not already name the Signature attribute. Append its
    # UTF-8 entry without changing any original constant-pool index or Code.
    name_record = b"" if names else b"\x01\x00\x09Signature"
    signature_index = count + int(not names)
    if signature_index + 1 > 65535:
        raise RuntimeError("Owned class constant pool cannot hold its counterexample")
    record = name_record + b"\x01" + struct.pack(">H", len(encoded)) + encoded
    attribute = struct.pack(">HIH", names[0] if names else count, 2, signature_index)
    tail = (original[pool_end:start + 6] + struct.pack(">H", size + 1)
            + original[start + 8:end] + attribute + original[end:])
    changed = original[:8] + struct.pack(">H", signature_index + 1) + original[10:pool_end] + record + tail
    if method_code(original) != method_code(changed):
        raise RuntimeError("Parameter Signature insertion changed Code or descriptors")
    path.write_bytes(changed)
    return {"signature": signature, "code_and_descriptors": "unchanged",
            "before_sha256": hashlib.sha256(original).hexdigest(),
            "after_sha256": hashlib.sha256(changed).hexdigest()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-inherited-erased-parameters-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "inherited_erased_parameters/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness_dir = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        declarations = {"Lfixture/Contract;", "Lfixture/Trace;", "Lfixture/ClosedContract;"}
        classes = all_classes - declarations
        methods = {key: value for key, value in all_methods.items() if key.split("->", 1)[0] in classes}
        verify.compile_local([FIXTURES / "harness/InheritedErasedParametersHarness.java"],
                             harness_dir, classpath=originals)
        dependency = work / "owned-contract.jar"
        with zipfile.ZipFile(dependency, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(originals.rglob("*.class")):
                relative = path.relative_to(originals).as_posix()
                if "L" + relative[:-6] + ";" in declarations:
                    archive.writestr(relative, path.read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        negative = work / "negative-originals"
        shutil.copytree(originals, negative)
        receipt = add_parameter_signature(originals / "fixture/Root.class", "(TK;TV;)Z")
        (work / "input-metadata-change.json").write_text(json.dumps(receipt, indent=2) + "\n")
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               "InheritedErasedParametersHarness", "original"], "baseline")
        if baseline.splitlines() != ["identities=29,failures=12,bridges=0",
                                     "inherited-erased-parameters=matched,default-effects=absent"]:
            raise RuntimeError("Owned inherited parameter baseline missed expected cases")

        def owned_dex(directory: Path, output: Path) -> Path:
            output.mkdir(parents=True)
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath",
                        args.android_jar.resolve(), "--classpath", dependency, "--output", output,
                        *[path for path in sorted(directory.rglob("*.class"))
                          if "L" + path.relative_to(directory).as_posix()[:-6] + ";" in classes]],
                       "d8-" + output.name)
            dex = output / "classes.dex"
            if not dex.is_file() or list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned parameter fixture did not produce exactly one DEX")
            return dex

        dex = owned_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned parameter fixture inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "--classpath", dependency, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage.get("projected_method_count", 0) != 0
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "recovered"
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Inherited class-formal parameters lost descriptors or original metadata")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if actual_classes != classes or not set(methods).issubset(actual_methods):
            raise RuntimeError("javac did not regenerate every original inherited method descriptor")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir, dependency))),
                             "InheritedErasedParametersHarness", "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Recovered parameter erasure changed inherited dispatch or ordered effects")
        negative_receipt = add_parameter_signature(negative / "fixture/Root.class",
                                                   "<K:Ljava/lang/Object;V:Ljava/lang/Object;>(TK;TV;)Z")
        (work / "negative-metadata-change.json").write_text(json.dumps(negative_receipt, indent=2) + "\n")
        negative_dex = owned_dex(negative, work / "negative-dex")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "--classpath", str(dependency), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic inheritance" not in result.stderr:
            raise RuntimeError("An unproven method-formal scope authorized parameter erasure")
        negative_sources = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "Root.java":
                content = content.replace("public boolean remove(Object key, Object value)",
                                          "public boolean remove(K key, V value)")
            destination = negative_sources / source.relative_to(fixture)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode != 0:
            raise RuntimeError("Independent javac rejected the original class-formal parameters")
        independent = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (
            work / "negative-compiler", harness_dir))), "InheritedErasedParametersHarness", "original"],
            "independent-source-execution")
        if independent != baseline:
            raise RuntimeError("Independent class-formal source changed JVM dispatch or ordered effects")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted(originals.rglob("*.class"))}:
            raise RuntimeError("Original independently authored class bytes changed")
        if hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash:
            raise RuntimeError("Owned external declarations changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "bridge_count": 0, "identity_cases": 29, "ordered_failure_cases": 12,
                    "class_provider_precedence": "matched", "default_effects": "absent",
                    "generic_reflection": "unchanged", "coverage": "recovered",
                    "source_projection_count": 0, "original_descriptors": "retained",
                    "method_formal_counterexample": "rejected-by-neverd",
                    "original_class_formal_parameters": "matched-by-javac-and-jvm",
                    "object_wildcard_equivalence": "matched-with-original-signatures",
                    "closed_object_wildcard_substitution": "matched",
                    "original_class_sha256": hashes, "jvm_output": "matched"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
