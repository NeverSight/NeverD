#!/usr/bin/env python3
"""Verify primitive constructor selection before boxing against generic reference overloads."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import shutil
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--byte-forwarder", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-primitive-constructor-overloads-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "primitive_constructor_overloads/java"
        harness_sources = [FIXTURES / "harness" / (name + ".java") for name in
                           ("PrimitiveConstructorOverloadsHarness", "PrimitiveConstructorOverloadsCaller")]
        if args.byte_forwarder:
            authored = work / "byte-source"
            shutil.copytree(fixture, authored)
            parent = authored / "fixture/Parent.java"
            text = parent.read_text()
            anchor = "    public Parent(int value)"
            if text.count(anchor) != 1:
                raise RuntimeError("Primitive sibling source anchor changed")
            text = text.replace(anchor, "    public Parent(short value) { selected = 8; number = value; Trace.last = this; Trace.hit(8); }\n" + anchor)
            parent.write_text(text)
            core = authored / "fixture/Core.java"
            text = core.read_text()
            if not text.endswith("}\n"):
                raise RuntimeError("Primitive forwarder source anchor changed")
            core.write_text(text[:-2] + "    public Core(byte value) { super((int) value); Trace.hit(5); }\n}\n")
            fixture = authored
            oracle = work / "byte-harness"
            oracle.mkdir()
            for original in harness_sources:
                text = original.read_text()
                if original.name.endswith("Caller.java"):
                    text = text.replace(": new Core<String, Integer>(Trace.read(input));",
                        ": width == 1 ? new Core<String, Integer>(Trace.read(input)) : new Core<String, Integer>((byte) Trace.read(input));")
                else:
                    changes = ((": new Core<String, Integer>(Trace.read(input));", ": width == 1 ? new Core<String, Integer>(Trace.read(input)) : new Core<String, Integer>((byte) Trace.read(input));"),
                               ("width == 0 ? int.class : long.class", "width == 0 ? int.class : width == 1 ? long.class : byte.class"),
                               (": (Object) Long.valueOf(Trace.read(input));", ": width == 1 ? (Object) Long.valueOf(Trace.read(input)) : (Object) Byte.valueOf((byte) Trace.read(input));"),
                               ("        return (Core<?, ?>) MethodHandles.publicLookup()", "        if (width == 2) return (Core<?, ?>) MethodHandles.publicLookup().findConstructor(Core.class, MethodType.methodType(void.class, byte.class)).invoke((byte) Trace.read(input));\n        return (Core<?, ?>) MethodHandles.publicLookup()"),
                               ("width == 0 ? 1 : 3", "width == 1 ? 3 : 1"),
                               ("if (width == 0) number = (int) number;", "if (width == 0) number = (int) number; else if (width == 2) number = (byte) number;"))
                    for before, after in changes:
                        if before not in text:
                            raise RuntimeError("Independent primitive byte oracle anchor changed")
                        text = text.replace(before, after)
                (oracle / original.name).write_text(text)
            harness_sources = sorted(oracle.glob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local(harness_sources, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != (13 if args.byte_forwarder else 11):
            raise RuntimeError("Owned primitive constructor inventory changed")
        classes = all_classes - ({"Lfixture/Parent;"} if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x") as archive:
            archive.writestr("fixture/Parent.class", (originals / "fixture/Parent.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            outputs = []
            for width in range(3 if args.byte_forwarder else 2):
                for route in range(4):
                    for kind in range(3):
                        for mode in range(7):
                            output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                "PrimitiveConstructorOverloadsHarness", str(width), str(route), str(kind), str(mode)],
                                f"{label}-{width}-{route}-{kind}-{mode}")
                            expected = f"width={width},route={route},kind={kind},mode={mode},attempt=[01],trace=[0-9]+"
                            if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                                raise RuntimeError("Original primitive constructor matrix changed: " + output)
                            outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        dex_dir = work / "dex"
        dex_dir.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex_dir, *paths], "d8-original")
        source_dex = dex_dir / "classes.dex"
        if list(dex_dir.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned primitive constructor fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original primitive constructor runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-constructor-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            prototype = re.search(r"(?m)^\s+type\s+: '(\([IJB]\)V)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] != "<init>" or not prototype:
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
            if not ops or ops[0][0].split("/")[0] != "invoke-direct" or ("Lfixture/Parent;.<init>:" + ("(I)V" if prototype[1] == "(B)V" else prototype[1])) not in ops[0][1]:
                raise RuntimeError("D8 changed original primitive superclass constructor selection")
            witnessed.add(prototype[1])
        if witnessed != ({"(I)V", "(J)V", "(B)V"} if args.byte_forwarder else {"(I)V", "(J)V"}):
            raise RuntimeError("Missing both original primitive constructor witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count", 0)
                or coverage["status"] != "recovered" or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Allocation payload recovery changed original inventory or contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=8, classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Allocation payload recovery invented or omitted a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original payload class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after["methods"].get(identity) != metadata or actual_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Original payload method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Primitive constructor changed values, ordered effects, aliases or failure identity")
        mutations = (
            ("wrong-overload", "Core", "super(value);", "super((long) value);", 1),
            ("wrong-selected-entry", "Parent", "selected = 1;", "selected = 2;", 1),
            ("wrong-argument", "Trace", "[0]++ + seed;", "[0]++ + 1;", 1),
        )
        if args.byte_forwarder:
            mutations += (("omitted-widening-view", "Core", "super((int) value);", "super(value);", 1),)
        for label, owner, old, new, count in mutations:
            text = (fixture / ("fixture/" + owner + ".java")).read_text()
            if text.count(old) < count:
                raise RuntimeError("Independent primitive overload counterexample anchor changed")
            wrong = work / label / ("fixture/" + owner + ".java")
            wrong.parent.mkdir(parents=True)
            wrong.write_text(text.replace(old, new, count))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                    "PrimitiveConstructorOverloadsHarness", "2" if label == "omitted-widening-view" else "0", "1", "0", "0"], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned primitive constructor overload contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_provider": args.dependency_provider, "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "original_inventory_metadata_and_exceptions": "unchanged", "selected_primitive_constructors": 3 if args.byte_forwarder else 2, "byte_forwarder": args.byte_forwarder,
                    "generic_copy_method_formal_and_varargs_siblings": "not selected",
                    "partial_objects_argument_order_and_failure_identity": "matched",
                    "independent_compiler_counterexamples": len(mutations), "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
