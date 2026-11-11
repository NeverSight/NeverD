#!/usr/bin/env python3
"""Verify original throwing bridge bodies without introducing argument casts."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zipfile

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

CLASS_SIGNATURE = b"<K:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/CountView;"
INPUT_SIGNATURE = CLASS_SIGNATURE[:-1] + b"<Lfixture/Element<+TK;+TV;>;>;"
IDENTITY = "Lfixture/Counter;->add(Ljava/lang/Object;)Z"
TARGET = "Lfixture/Counter;->add(Lfixture/Element;)Z"


def bridge_flags(path: Path) -> dict:
    before = path.read_bytes()
    shape = class_records(before)
    strings = shape["strings"]
    offset = shape["pool_end"] + 6
    offset += 2 + 2 * int.from_bytes(before[offset:offset + 2], "big")

    def attributes(position: int, count: int) -> int:
        for _ in range(count):
            _, length = struct.unpack_from(">HI", before, position)
            position += 6 + length
        return position

    fields = int.from_bytes(before[offset:offset + 2], "big")
    offset += 2
    for _ in range(fields):
        offset = attributes(offset + 8, int.from_bytes(before[offset + 6:offset + 8], "big"))
    count = int.from_bytes(before[offset:offset + 2], "big")
    offset += 2
    targets = []
    for _ in range(count):
        start = offset
        flags, name, prototype, size = struct.unpack_from(">HHHH", before, start)
        offset = attributes(start + 8, size)
        if strings[name] == b"add" and strings[prototype] == b"(Ljava/lang/Object;)Z":
            if flags != 1:
                raise RuntimeError("Owned throwing bridge source flags changed")
            targets.append(start)
    if len(targets) != 1:
        raise RuntimeError("Missing unique owned throwing bridge")
    position = targets[0]
    after = before[:position] + struct.pack(">H", 0x1041) + before[position + 2:]
    if method_code(before) != method_code(after):
        raise RuntimeError("Bridge flags changed original Code or descriptors")
    path.write_bytes(after)
    return {"original_flags": 1, "input_flags": 0x1041, "code_and_descriptors": "unchanged"}


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-terminal-throw-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "terminal_throw_bridge/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness_dir = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - {"Lfixture/CountView;"}
        methods = {k: v for k, v in all_methods.items() if k.split("->", 1)[0] in classes}
        verify.compile_local([FIXTURES / "harness/TerminalThrowBridgeHarness.java"], harness_dir,
                             classpath=originals)
        dependency = work / "owned-contract.jar"
        with zipfile.ZipFile(dependency, "x", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("fixture/CountView.class", (originals / "fixture/CountView.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        counter = originals / "fixture/Counter.class"
        receipt = change_signature(counter, CLASS_SIGNATURE, INPUT_SIGNATURE)
        receipt["bridge_flag_change"] = bridge_flags(counter)
        (work / "input-metadata-change.json").write_text(json.dumps(receipt, indent=2) + "\n")
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               "TerminalThrowBridgeHarness", "original"], "baseline")
        expected = ["values=7,identities=8,failures=12",
                    "terminal-throw=matched,unchecked-arguments=retained,metadata-difference=reported"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned terminal throw baseline missed cases: " + baseline)

        def dex(directory: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--classpath", dependency, "--output", destination,
                        directory / "fixture/Element.class", directory / "fixture/Counter.class"], "d8-" + destination.name)
            return destination / "classes.dex"

        owned = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", owned], "sdk-inventory")
        inventory = parse_dexdump(dump, owned.name, dex_header(owned.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes
                or {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned input inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", owned, "--classpath", args.android_jar.resolve(),
                    "--classpath", dependency, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        projected = [r for r in coverage["methods"] if r["status"] == "source-projected"]
        generic = coverage.get("generic_signature_projections", [])
        kinds = {change["kind"] for row in generic for change in row["changes"]}
        if ({r["identity"] for r in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["projected_method_count"] != 1 or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial" or len(projected) != 1
                or projected[0]["identity"] != IDENTITY or projected[0]["source_identity"] != IDENTITY
                or projected[0]["bridge_target"] != TARGET
                or projected[0]["projection_kind"] != "terminal-throw-interface-bridge"
                or projected[0]["instruction_count"] != 4
                or len(generic) != 2 or kinds != {"terminal-throw-raw-interface", "inherited-nested-return-variance"}):
            raise RuntimeError("Terminal throw bridge lost its complete body or metadata receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if actual_classes != classes or not set(methods).issubset(actual_methods):
            raise RuntimeError("javac lost an original runtime descriptor")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir, dependency))),
                             "TerminalThrowBridgeHarness", "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Throwing bridge changed incompatible argument handling or effects")
        for directory, label in [(originals, "original"), (rebuilt, "rebuilt")]:
            dump = verify.run([Path(verify.javac).parent / "javap", "-c", "-p", "-s",
                               directory / "fixture/Counter.class"], label + "-allocation-order")
            for parameter in ["java.lang.Object", "fixture.Element"]:
                body = re.search(r"public boolean add\(" + re.escape(parameter)
                                 + r"[^\n]*\n(?P<body>.*?)(?=\n  (?:public|private|protected)|\n})",
                                 dump, re.S)
                if body is None:
                    raise RuntimeError("Missing original throwing method in independent javap")
                text = body.group("body")
                allocation = re.search(r"\bnew\s+.*class java/lang/UnsupportedOperationException", text)
                string = re.search(r"\bldc(?:_w)?\s+.*String owned immutable operation", text)
                constructor = re.search(r"\binvokespecial\s+.*UnsupportedOperationException.*<init>", text)
                if not allocation or not string or not constructor or not allocation.start() < string.start() < constructor.start():
                    raise RuntimeError("Source allocation moved after string resolution")
        negative = work / "negative-originals"
        (negative / "fixture").mkdir(parents=True)
        for p in originals.glob("fixture/*.class"):
            (negative / "fixture" / p.name).write_bytes(p.read_bytes())
        receipt = change_signature(negative / "fixture/Counter.class",
                                   b"(Lfixture/Element<+TK;+TV;>;)Z", b"(Lfixture/Element<+TK;+TK;>;)Z")
        (work / "negative-metadata-change.json").write_text(json.dumps(receipt, indent=2) + "\n")
        rejected = work / "rejected"
        negative_dex = dex(negative, work / "negative-dex")
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "--classpath", str(dependency), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic inheritance" not in result.stderr:
            raise RuntimeError("Original inheritance contract was bypassed after source erasure")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted(originals.rglob("*.class"))}:
            raise RuntimeError("Original owned class bytes changed")
        if hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash:
            raise RuntimeError("Original dependency declaration changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "identity_cases": 8, "value_cases": 7, "ordered_failure_cases": 12,
                    "runtime_descriptors_and_bodies": "retained", "dependency_sources_emitted": 0,
                    "unfiltered_argument_exceptions": "matched", "field_and_typed_parameter_reflection": "unchanged",
                    "interface_return_and_bridge_metadata_differences": "reported", "invented_bridges": 0,
                    "coverage": "partial", "original_contract_counterexample": "rejected", "jvm_output": "matched"}
        evidence["allocation_precedes_string_resolution"] = "matched-by-independent-javap"
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
