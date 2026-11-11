#!/usr/bin/env python3
"""Verify original default guards, late casts and inherited default dispatch."""
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def hashes(directory: Path) -> dict:
    return {p.relative_to(directory).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(directory.rglob("*.class"))}


def encode_original_guard(path: Path) -> dict:
    """Use the authored erasedDefault body for the JVM-only erased return entry."""
    original = path.read_bytes()
    shape = class_records(original)
    strings = shape["strings"]
    position = shape["pool_end"] + 6
    position += 2 + 2 * int.from_bytes(original[position:position + 2], "big")

    def attributes(start: int, count: int) -> tuple:
        rows = []
        for _ in range(count):
            name, size = struct.unpack_from(">HI", original, start)
            end = start + 6 + size
            rows.append((strings[name], original[start:end]))
            start = end
        return start, rows

    fields = int.from_bytes(original[position:position + 2], "big")
    position += 2
    for _ in range(fields):
        position, _ = attributes(position + 8, int.from_bytes(original[position + 6:position + 8], "big"))
    count_position = position
    count = int.from_bytes(original[position:position + 2], "big")
    position += 2
    rows = []
    for _ in range(count):
        start = position
        flags, name, prototype, size = struct.unpack_from(">HHHH", original, start)
        position, attrs = attributes(start + 8, size)
        rows.append({"flags": flags, "name": strings[name], "prototype": strings[prototype],
                     "header": original[start:start + 8], "attributes": attrs})
    prototype = b"(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"
    templates = [r for r in rows if r["name"] == b"erasedDefault" and r["prototype"] == prototype]
    erased = [r for r in rows if r["name"] == b"getOrDefault" and r["prototype"] == prototype]
    if len(templates) != 1 or len(erased) != 1 or erased[0]["flags"] != 0x1041:
        raise RuntimeError("Missing unique authored guard template and javac return bridge")
    code = [value for name, value in templates[0]["attributes"] if name == b"Code"]
    if len(code) != 1:
        raise RuntimeError("Authored erased guard has no unique complete Code")
    rebuilt = bytearray(original[:count_position])
    rebuilt += struct.pack(">H", count - 1)
    for row in rows:
        if row is templates[0]:
            continue
        if row is erased[0]:
            rebuilt += struct.pack(">H", 0x1051) + row["header"][2:]
            for name, value in row["attributes"]:
                rebuilt += code[0] if name == b"Code" else value
        else:
            rebuilt += row["header"]
            for _, value in row["attributes"]:
                rebuilt += value
    rebuilt += original[position:]
    changed = bytes(rebuilt)
    expected = method_code(original)
    expected[b"getOrDefault" + prototype] = expected.pop(b"erasedDefault" + prototype)
    if method_code(changed) != expected:
        raise RuntimeError("Owned guard encoding altered an unrelated Code body or descriptor")
    path.write_bytes(changed)
    flags = [bridge_flag(path, "getOrDefault", "(Ljava/lang/Object;Lfixture/Holder;)Lfixture/Holder;", 0x11, 0x51),
             bridge_flag(path, "getOrDefault", "(Lfixture/Element;Lfixture/Holder;)Lfixture/Holder;", 0x1, 0x41)]
    return {"authored_guard_template": "erasedDefault", "erased_return_entry": "getOrDefault" + prototype.decode(),
            "original_erased_guard_code": "copied exactly from independently authored template before baseline",
            "template_removed_before_baseline": True, "other_code_and_descriptors": "unchanged",
            "erased_access": "public final bridge synthetic", "flag_changes": flags,
            "compiled_template_sha256": hashlib.sha256(original).hexdigest(),
            "input_sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-root", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-guarded-default-super-adapters-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "guarded_default_super_adapters/java"
        inputs = sorted(fixture.rglob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/GuardedDefaultSuperAdapterHarness.java"],
                             harness, classpath=originals)
        receipt = encode_original_guard(originals / "fixture/Counter.class")
        (work / "input-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        declarations = {"Lfixture/CountView;"}
        if args.dependency_root:
            declarations.add("Lfixture/Root;")
        classes = all_classes - declarations
        methods = {k: v for k, v in all_methods.items() if k.split("->", 1)[0] in classes}
        original_records = class_identity.compiler_classes(originals)
        dependency = work / "owned-declarations.jar"
        with zipfile.ZipFile(dependency, "x", zipfile.ZIP_DEFLATED) as archive:
            for descriptor in sorted(declarations):
                path = descriptor[1:-1] + ".class"
                archive.writestr(path, (originals / path).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "GuardedDefaultSuperAdapterHarness", "original"], "baseline")
        if baseline.splitlines() != ["values=17,identities=73,failures=15",
                "negative-default=unchanged,late-cast-and-super-default=matched,metadata-differences=reported"]:
            raise RuntimeError("Original checked result oracle missed its independently authored cases")

        def dex(source: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath",
                        args.android_jar.resolve(), "--classpath", dependency, "--output", destination,
                        *(source / (descriptor[1:-1] + ".class") for descriptor in sorted(classes))],
                       "d8-" + destination.name)
            if list(destination.glob("classes*.dex")) != [destination / "classes.dex"]:
                raise RuntimeError("Owned checked reference fixture did not produce exactly one DEX")
            return destination / "classes.dex"

        owned = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", owned], "sdk-inventory")
        inventory = parse_dexdump(dump, owned.name, dex_header(owned.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes
                or {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original checked reference declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", owned, "--classpath", args.android_jar.resolve(),
                    "--classpath", dependency, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        projected = {r["identity"]: r for r in coverage["methods"] if r["status"] == "source-projected"}
        typed = "Lfixture/Counter;->getOrDefault(Lfixture/Element;Lfixture/Holder;)Lfixture/Holder;"
        wrapper = "Lfixture/Counter;->getOrDefault(Ljava/lang/Object;Lfixture/Holder;)Lfixture/Holder;"
        erased = "Lfixture/Counter;->getOrDefault(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"
        if ({r["identity"] for r in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage["projected_method_count"] != 3
                or set(projected) != {typed, wrapper, erased}):
            raise RuntimeError("Original checked bridge inventory or generic metadata changed")
        for identity, kind, target in (
                (typed, "checked-reference-superclass-forwarding-bridge", "Lfixture/Middle;->getOrDefault(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"),
                (wrapper, "guarded-default-result-forwarding-bridge", typed),
                (erased, "guarded-default-result-forwarding-bridge", typed)):
            row = projected[identity]
            if (row.get("projection_kind") != kind or row.get("bridge_target") != target
                    or not row.get("metadata_differences")):
                raise RuntimeError("Original checked guard or superclass call lost its receipt")
        projections = coverage.get("generic_signature_projections", [])
        expected_signature = "<T:Ljava/lang/Object;>Lfixture/Middle<Lfixture/Holder<TT;>;>;Lfixture/CountView<Lfixture/Holder<TT;>;>;"
        if (len(projections) != 1 or projections[0]["declaration"] != "Lfixture/Counter;"
                or projections[0]["original_signature"] != expected_signature
                or {r["kind"] for r in projections[0]["changes"]} != {
                    "guarded-default-raw-superclass", "guarded-default-raw-interface"}
                or {r["source_type"] for r in projections[0]["changes"]} != {
                    "Lfixture/Middle;", "Lfixture/CountView;"}
                or not projections[0].get("metadata_differences")):
            raise RuntimeError("Original complete ancestry or raw source projection lost its receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("Rebuilt inventory contains an unreported or missing declaration")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            expected, actual = original_records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                wanted_value = "<T:Ljava/lang/Object;>Lfixture/Middle;Lfixture/CountView;" if owner == "Lfixture/Counter;" and key == "signature" else expected.get(key)
                if actual.get(key) != wanted_value:
                    raise RuntimeError("Class metadata changed beyond reported raw ancestry: " + owner + ":" + key)
            for identity, method in expected["methods"].items():
                wanted = dict(method)
                if identity in {typed, wrapper, erased}:
                    wanted["access"] &= ~0x1040
                if actual["methods"][identity] != wanted:
                    raise RuntimeError("Method metadata changed beyond reported bridge/final flags: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "GuardedDefaultSuperAdapterHarness", "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Checked result, virtual dispatch, guard or ordered superclass effects changed")
        for label, owner, old, wrong in (
                ("wrapper-result", "Counter", b"(Ljava/lang/Object;Lfixture/Holder<TT;>;)Lfixture/Holder<TT;>;",
                 b"(Ljava/lang/Object;Lfixture/Holder<Ljava/lang/String;>;)Lfixture/Holder<Ljava/lang/String;>;"),
                ("superclass-result", "Counter", b"<T:Ljava/lang/Object;>Lfixture/Middle<Lfixture/Holder<TT;>;>;Lfixture/CountView<Lfixture/Holder<TT;>;>;",
                 b"<T:Ljava/lang/Object;>Lfixture/Middle<Lfixture/Holder<Ljava/lang/String;>;>;Lfixture/CountView<Lfixture/Holder<TT;>;>;")):
            negative = work / ("negative-" + label)
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / ("fixture/" + owner + ".class"), old, wrong)
            (work / (label + "-metadata-change.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / ("negative-" + label + "-dex"))
            rejected = work / ("rejected-" + label)
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                    str(args.android_jar.resolve()), "--classpath", str(dependency), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Same-erasure checked return metadata drift was accepted")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original input or independent harness bytes changed")
        if hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash:
            raise RuntimeError("Original dependency declarations changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 17, "identity_cases": 73, "ordered_failure_cases": 15,
                    "original_bridge_count": 3, "rebuilt_bridge_count": 0,
                    "complete_runtime_inventory": "original methods and reported helpers only",
                    "generic_reflection": "method types unchanged; raw superclass and interface reported", "superclass_invocation_and_original_check": "retained", "negative_default_before_cast": "matched",
                    "virtual_override_and_null_guard": "retained", "effects_before_failure": "matched",
                    "dependency_root": args.dependency_root, "bridge_flag_differences_and_raw_ancestry": "reported",
                    "same_erasure_metadata_counterexamples": "rejected with unchanged Code and descriptors",
                    "original_class_sha256": original_hashes, "harness_class_sha256": harness_hashes,
                    "dependency_sha256": dependency_hash, "coverage": "partial", "jvm_output": "matched"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
