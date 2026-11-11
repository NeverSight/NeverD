#!/usr/bin/env python3
"""Verify branch-dependent defaults, captured outer identity and getter failures."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

try:
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, class_identity
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, class_identity
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code

MEMBER = "Lfixture/Outer$Member;"
PROTOTYPE = "(Lfixture/Outer;IIIILjava/lang/Object;)V"


def synthetic_constructor(path: Path) -> dict:
    """Change exactly the owned constructor's synthetic flag, preserving Code."""
    original = path.read_bytes()
    shape = class_records(original)
    strings = shape["strings"]
    offset = shape["pool_end"] + 6
    offset += 2 + 2 * int.from_bytes(original[offset:offset + 2], "big")

    def skip(position: int, count: int) -> int:
        for _ in range(count):
            length = int.from_bytes(original[position + 2:position + 6], "big")
            position += 6 + length
        return position

    count = int.from_bytes(original[offset:offset + 2], "big")
    offset += 2
    for _ in range(count):
        offset = skip(offset + 8, int.from_bytes(original[offset + 6:offset + 8], "big"))
    count = int.from_bytes(original[offset:offset + 2], "big")
    offset += 2
    selected = []
    for _ in range(count):
        start = offset
        flags, name, descriptor, attributes = struct.unpack_from(">HHHH", original, start)
        offset = skip(start + 8, attributes)
        if strings[name] == b"<init>" and strings[descriptor] == PROTOTYPE.encode():
            if flags != 1:
                raise RuntimeError("Owned default constructor access changed")
            selected.append(start)
    if len(selected) != 1:
        raise RuntimeError("Missing unique owned default constructor")
    at = selected[0]
    changed = original[:at] + struct.pack(">H", 0x1001) + original[at + 2:]
    wanted = class_identity.ClassFile(original).facts()
    wanted["methods"][MEMBER + "-><init>" + PROTOTYPE]["access"] = 0x1001
    if (method_code(original) != method_code(changed)
            or class_identity.ClassFile(changed).facts() != wanted):
        raise RuntimeError("Synthetic flag edit changed Code or other full metadata")
    path.write_bytes(changed)
    return {"original_flags": 1, "input_flags": 0x1001,
            "code_and_other_metadata": "unchanged"}


def hashes(directory: Path) -> dict:
    return {p.relative_to(directory).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(directory.rglob("*")) if p.is_file()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-guarded-constructor-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), work / "unused-d8", 300)
        fixture = FIXTURES / "guarded_constructor_captures"
        smali = fixture / "smali"
        inputs = sorted((fixture / "java").rglob("*.java"))
        harness_source = FIXTURES / "harness/GuardedConstructorCaptureHarness.java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals, release=8)
        receipt = synthetic_constructor(originals / "fixture/Outer$Member.class")
        (work / "owned-input-change.json").write_text(json.dumps(receipt, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        verify.compile_local([harness_source], harness, classpath=originals, release=25)
        input_hashes, harness_hashes, smali_hashes = hashes(originals), hashes(harness), hashes(smali)
        baseline = verify.run([verify.java, "-cp", str(originals) + ":" + str(harness),
                               "GuardedConstructorCaptureHarness"], "baseline")
        expected = ["values=480,outer-identities=160,failures=80",
                    "getter-order-and-count=matched,construction-count=matched", "static=false"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned constructor baseline missed cases: " + baseline)
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", smali, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        bindings = coverage.get("class_source_bindings", [])
        prologues = {r["identity"] for r in coverage["methods"] if r.get("minimum_java_release") == 25}
        if (coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or {r["identity"] for r in coverage["methods"]} != set(methods)
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage["minimum_java_release"] != 25 or len(bindings) != 1
                or bindings[0].get("binding_kind") != "inner-to-static-member"
                or not bindings[0].get("metadata_differences")
                or prologues != {MEMBER + "-><init>(Lfixture/Outer;III)V", MEMBER + "-><init>" + PROTOTYPE}):
            raise RuntimeError("Guarded constructor lost its common capture proof or source binding")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=25)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(methods) | helpers:
            raise RuntimeError("Guarded constructor changed inventory beyond reported helpers")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            wanted = copy.deepcopy(records[owner])
            after = actual_records[owner]
            if owner == MEMBER:
                wanted["inner_class"]["access"] |= 8
                wanted["fields"][MEMBER + "->this$0:Lfixture/Outer;"]["access"] &= ~0x1000
                wanted["methods"][MEMBER + "-><init>" + PROTOTYPE]["access"] &= ~0x1000
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                if wanted.get(key) != after.get(key):
                    raise RuntimeError("Guarded constructor changed unreported metadata: " + owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                if metadata != after["methods"][identity]:
                    raise RuntimeError("Guarded constructor changed unreported member metadata: " + identity)
        actual = verify.run([verify.java, "-cp", str(rebuilt) + ":" + str(harness),
                             "GuardedConstructorCaptureHarness"], "rebuilt-execution")
        if actual.splitlines() != expected[:-1] + ["static=true"]:
            raise RuntimeError("Guarded defaults changed values, capture, failure identity or effect order")
        for case, (before, after) in enumerate([
            ("{p0, p1, p2, p3, p4}", "{p0, p6, p2, p3, p4}"),
            ("if-eqz p5, :initialize", "if-eqz p5, :done"),
            ("and-int/lit8 p5, p5, 4", "move-object p1, p6")
        ]):
            negative = work / ("negative-" + str(case))
            negative.mkdir()
            for source in smali.rglob("*.smali"):
                text = source.read_text()
                if source.name == "Outer$Member.smali":
                    if text.count(before) != 1:
                        raise RuntimeError("Missing exact owned constructor counterexample")
                    text = text.replace(before, after, 1)
                    if case == 1:
                        end = "    invoke-direct {p0, p1, p2, p3, p4}, Lfixture/Outer$Member;-><init>(Lfixture/Outer;III)V\n    return-void"
                        text = text.replace(end, end.replace("    return-void", "    :done\n    return-void"), 1)
                (negative / source.name).write_text(text)
            rejected = work / ("rejected-" + str(case))
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / ("negative-" + str(case) + ".log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Unproven outer constructor path was accepted")
        if (hashes(originals) != input_hashes or hashes(harness) != harness_hashes
                or hashes(smali) != smali_hashes):
            raise RuntimeError("Original independently authored oracle or harness bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "java_release": 25, "value_probes": 480, "outer_identity_cases": 160,
                    "failure_cases": 80, "getter_order_and_count": "matched",
                    "construction_count": "once per successful instance", "failure_identity": "unchanged",
                    "original_metadata_and_inventory": "unchanged except reported member binding, synthetic flags and helpers",
                    "counterexamples": 3, "coverage": "partial", "jvm_output": "matched"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
