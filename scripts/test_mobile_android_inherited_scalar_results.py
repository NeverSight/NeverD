#!/usr/bin/env python3
"""Verify original scalar factories with inherited producers and consumers without dropping sibling contracts."""
from __future__ import annotations

import argparse
import copy
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory

BRIDGE = "Lfixture/Core;->write(Lfixture/Token;Ljava/lang/Object;)V"
RETURN_BRIDGE = "Lfixture/Core;->read(Lfixture/Token;)Ljava/lang/Object;"
RAW_SIGNATURE = b"Ljava/lang/Object;Lfixture/Api;Lfixture/Marker<Lfixture/Carrier;>;"
INPUT_SIGNATURE = b"Ljava/lang/Object;Lfixture/Api<Lfixture/Carrier;>;Lfixture/Marker<Lfixture/Carrier;>;"


PRIMITIVES = {"boolean": "Z", "byte": "B", "char": "C", "short": "S",
              "int": "I", "long": "J", "float": "F", "double": "D"}
SCALAR_CASES = {
    "boolean": ("false", "true"),
    "byte": ("Byte.MIN_VALUE", "Byte.MAX_VALUE", "(byte)-1", "(byte)0", "(byte)1"),
    "char": ("Character.MIN_VALUE", "Character.MAX_VALUE", "(char)0x8000", "(char)0x7fff", "(char)1"),
    "short": ("Short.MIN_VALUE", "Short.MAX_VALUE", "(short)-1", "(short)0", "(short)1"),
    "float": ("-0.0f", "0.0f", "Float.MIN_VALUE", "-Float.MAX_VALUE", "Float.MAX_VALUE",
              "Float.NEGATIVE_INFINITY", "Float.POSITIVE_INFINITY", "Float.intBitsToFloat(0x7fc12345)"),
    "double": ("-0.0", "0.0", "Double.MIN_VALUE", "-Double.MAX_VALUE", "Double.MAX_VALUE",
               "Double.NEGATIVE_INFINITY", "Double.POSITIVE_INFINITY", "Double.longBitsToDouble(0x7ff8123456789abCL)"),
}


def scalar_fixture(work: Path, primitive: str, prefix: str) -> tuple[Path, list[Path]]:
    """Render owned source templates before compiling and freezing the oracle."""
    root = FIXTURES / "inherited_scalar_results"
    if primitive in ("int", "long"):
        return root / primitive / "java", [FIXTURES / "harness" / (prefix + suffix + ".java")
                                          for suffix in ("Harness", "Caller")]
    source, harness = work / "owned-source", work / "owned-harness-source"
    shutil.copytree(root / "int" / "java", source)
    harness.mkdir()
    equal = "left == right"
    if primitive == "float":
        equal = "Float.floatToRawIntBits(left) == Float.floatToRawIntBits(right)"
    elif primitive == "double":
        equal = "Double.doubleToRawLongBits(left) == Double.doubleToRawLongBits(right)"
    substitutions = {"@PRIMITIVE@": primitive, "@PREFIX@": prefix,
                     "@VALUES@": ", ".join(SCALAR_CASES[primitive]), "@EQUAL@": equal}
    for name in ("Core", "Carrier", "Token", "Harness", "Caller"):
        text = (root / "templates" / (name + ".java.in")).read_text()
        for old, new in substitutions.items():
            text = text.replace(old, new)
        if any(token in text for token in substitutions):
            raise RuntimeError("Unexpanded owned scalar fixture template")
        target = (harness / (prefix + name + ".java") if name in ("Harness", "Caller")
                  else source / "fixture" / (name + ".java"))
        target.write_text(text)
    return source, sorted(harness.glob("*.java"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parents", action="store_true")
    parser.add_argument("--primitive", choices=tuple(PRIMITIVES), required=True)
    args = parser.parse_args()
    descriptor = PRIMITIVES[args.primitive]
    sample_count = len(SCALAR_CASES.get(args.primitive, range(5)))
    value_count, identity_count, state_count = 19 * sample_count + 31, 12 * sample_count + 60, 12 * sample_count + 73
    prefix = args.primitive.capitalize() + "ScalarResult"
    harness_name = prefix + "Harness"
    target_identity = "Lfixture/Core;->writeValue(Lfixture/Token;" + descriptor + ")V"
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-inherited-scalar-results-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture, harness_sources = scalar_fixture(work, args.primitive, prefix)
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local(harness_sources, harness, classpath=originals)
        receipts = [bridge_flag(originals / "fixture/Core.class", "write", "(Lfixture/Token;Ljava/lang/Object;)V", 0x1, 0x1041),
                    bridge_flag(originals / "fixture/Core.class", "read", "(Lfixture/Token;)Ljava/lang/Object;", 0x1, 0x1041),
                    change_signature(originals / "fixture/Core.class", RAW_SIGNATURE, INPUT_SIGNATURE)]
        (work / "owned-original-encoding.json").write_text(json.dumps(receipts, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        parents = {"Lfixture/" + name + ";" for name in ("Api", "Consumer", "Producer", "Marker", "Carrier")}
        classes = all_classes - (parents if args.dependency_parents else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, original_exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-parents.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for owner in sorted(parents):
                name = owner[1:-1] + ".class"
                archive.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_parents:
            classpath += ["--classpath", dependency]
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                              harness_name], "baseline")
        if baseline.splitlines() != [f"values={value_count},identities={identity_count},states={state_count},failures=60",
                "inherited-consumer-producer-and-metadata-contracts=retained,original-scalar-unbox-box-and-named-delegate-effects=retained"]:
            raise RuntimeError("Owned inherited scalar result matrix changed: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned inherited scalar result fixture changed DEX inventory")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original scalar result runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count") != 2 or projected != {BRIDGE, RETURN_BRIDGE}
                or coverage["status"] != "partial" or coverage.get("class_source_bindings")):
            raise RuntimeError("Inherited consumer recovery changed original inventory or bridge ownership")
        projections = coverage.get("generic_signature_projections", [])
        if (len(projections) != 1 or projections[0]["declaration"] != "Lfixture/Core;"
                or projections[0]["original_signature"] != INPUT_SIGNATURE.decode()
                or len(projections[0]["changes"]) != 1
                or projections[0]["changes"][0]["kind"] != "checked-value-carrier-interface"):
            raise RuntimeError("Original consumer edge source erasure was not disclosed exactly")
        row = rows[BRIDGE]
        if (row.get("projection_kind") != "value-carrier-interface-bridge"
                or row.get("bridge_target") != target_identity or row.get("source_identity") != BRIDGE
                or row.get("unbox_methods") != ["Lfixture/Carrier;->readBits()" + descriptor]
                or row.get("checked_inherited_void_contracts") != [{"identity": "Lfixture/Consumer;->write(Lfixture/Token;Ljava/lang/Object;)V",
                    "parameters": ["Lfixture/Token;", "Lfixture/Carrier;"], "returns": "V"}]
                or not row.get("metadata_differences")
                or rows[RETURN_BRIDGE].get("projection_kind") != "value-carrier-interface-bridge"):
            raise RuntimeError("Complete inherited consumer contract was not reported")
        row = rows[RETURN_BRIDGE]
        if (row.get("source_identity") != RETURN_BRIDGE
                or row.get("bridge_target") != "Lfixture/Core;->readValue(Lfixture/Token;)" + descriptor
                or row.get("box_method") != "Lfixture/Carrier;->box(" + descriptor + ")Lfixture/Carrier;"
                or row.get("checked_inherited_result_contracts") != [{"identity": "Lfixture/Producer;->read(Lfixture/Token;)Ljava/lang/Object;",
                    "parameters": ["Lfixture/Token;"], "returns": "Lfixture/Carrier;"}]
                or not row.get("metadata_differences")):
            raise RuntimeError("Complete inherited substitutions or original wide bridge were not reported")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_parents else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in ("Lfixture/Core;", "Lfixture/Token;", "Lfixture/Carrier;") if owner in classes}
        if helpers != expected_helpers:
            raise RuntimeError("Scalar recovery added an unexpected helper or replaced an original entry")
        if actual_classes != classes or set(actual_methods) != methods | helpers:
            raise RuntimeError("Rebuilt scalar result inventory changed or copied an inherited method")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                wanted = RAW_SIGNATURE.decode() if identity == "Lfixture/Core;" and key == "signature" else before.get(key)
                if wanted != after.get(key):
                    raise RuntimeError("Unreported class or field metadata change: " + identity + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = copy.deepcopy(metadata)
                if method in (BRIDGE, RETURN_BRIDGE):
                    wanted["access"] = 1
                if after["methods"][method] != wanted or actual_exceptions[method] != original_exceptions[method]:
                    raise RuntimeError("Unreported method contract or Exceptions change: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_parents else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                             harness_name, "rebuilt"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original inherited dispatch, external overrides, mutable aliases or exception identity changed")
        for label in ("missing-synthetic", "result-missing-synthetic", "producer-binding", "consumer-binding", "open-owner-scope"):
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            target = negative / "fixture/Core.class"
            if label == "missing-synthetic":
                receipt = change_method_flags(target, b"(Lfixture/Token;Ljava/lang/Object;)V", 0x1041, 0x41, name=b"write")
            elif label == "result-missing-synthetic":
                receipt = change_method_flags(target, b"(Lfixture/Token;)Ljava/lang/Object;", 0x1041, 0x41, name=b"read")
            elif label == "open-owner-scope":
                receipt = change_signature(target, INPUT_SIGNATURE,
                    b"<T:Ljava/lang/Object;>" + INPUT_SIGNATURE)
            elif label == "consumer-binding":
                receipt = change_signature(target, INPUT_SIGNATURE, INPUT_SIGNATURE.replace(b"Api<Lfixture/Carrier;>", b"Api<Lfixture/Token;>"))
            else:
                receipt = change_signature(negative / "fixture/Api.class",
                    b"<T:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Consumer<TT;>;Lfixture/Producer<TT;>;",
                    b"<T:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Consumer<TT;>;Lfixture/Producer<Lfixture/Token;>;")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_classpath = list(classpath)
            if args.dependency_parents:
                negative_dependency = work / (label + "-declarations.jar")
                with zipfile.ZipFile(negative_dependency, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                    for owner in sorted(parents):
                        name = owner[1:-1] + ".class"
                        archive.writestr(name, (negative / name).read_bytes())
                negative_classpath[-1] = negative_dependency
            negative_dex = dex(negative, work / (label + "-dex"))
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, negative_classpath),
                                     "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Inherited consumer accepted changed flags or sibling binding: " + label)
        negative_sources = work / "javac-negative"
        shutil.copytree(fixture, negative_sources)
        target = negative_sources / "fixture/Core.java"
        text = target.read_text()
        assert text.count("implements Api, Marker<Carrier>") == 1
        target.write_text(text.replace("implements Api, Marker<Carrier>", "implements Api<Token>, Marker<Carrier>"))
        result = subprocess.run([str(verify.javac), "--release", "8", "-proc:none", "-d", str(work / "javac-negative-classes"),
                                 *map(str, sorted(negative_sources.rglob("*.java")))], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "javac-negative.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0:
            raise RuntimeError("Independent javac accepted incompatible producer and consumer contracts")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original inputs, immutable callers or dependency bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "values": value_count, "identities": identity_count, "states": state_count, "failures": 60, "primitive": args.primitive,
                    "source_coverage": coverage["status"], "dependency_parents": args.dependency_parents,
                    "complete_inventory_metadata_and_exceptions": "checked",
                    "original_scalar_factories_and_named_sibling_dispatch": "retained",
                    "same_code_counterexamples": "rejected", "immutable_inputs_and_callers": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
