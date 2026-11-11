#!/usr/bin/env python3
"""Verify independently authored indexed and result-only terminal throw bridges."""
from __future__ import annotations

import argparse
import copy
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
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

PREFIX = b"<S:Ljava/lang/Object;>Ljava/lang/Object;"
SOURCE = PREFIX + b"Lfixture/Contract;"
INPUT = PREFIX + b"Lfixture/Contract<Lfixture/Element;>;"


def terminal_result_body(path: Path, method_name: str = "remove", parameters: str = "I") -> dict:
    """Copy the owned typed throw into only its owned javac result bridge."""
    before = path.read_bytes()
    shape = class_records(before)
    strings = shape["strings"]
    position = shape["pool_end"] + 6
    position += 2 + 2 * int.from_bytes(before[position:position + 2], "big")

    def attributes(offset: int, count: int) -> tuple:
        rows = []
        for _ in range(count):
            name, size = struct.unpack_from(">HI", before, offset)
            end = offset + 6 + size
            rows.append((strings[name], offset, end))
            offset = end
        return offset, rows

    fields = int.from_bytes(before[position:position + 2], "big")
    position += 2
    for _ in range(fields):
        position, _ = attributes(position + 8, int.from_bytes(before[position + 6:position + 8], "big"))
    methods = int.from_bytes(before[position:position + 2], "big")
    position += 2
    selected = {}
    erased = ("(" + parameters + ")Ljava/lang/Object;").encode()
    typed = ("(" + parameters + ")Lfixture/Element;").encode()
    for _ in range(methods):
        flags, name, descriptor, count = struct.unpack_from(">HHHH", before, position)
        position, rows = attributes(position + 8, count)
        if strings[name] == method_name.encode():
            expected = 0x1041 if strings[descriptor] == erased else 1
            code = [(start, end) for key, start, end in rows if key == b"Code"]
            if flags != expected or len(code) != 1:
                raise RuntimeError("Owned remove declaration changed")
            selected[strings[descriptor]] = code[0]
    if set(selected) != {erased, typed}:
        raise RuntimeError("Missing exact owned result bridge pair")
    start, end = selected[erased]
    typed_start, typed_end = selected[typed]
    changed = before[:start] + before[typed_start:typed_end] + before[end:]
    original_code, changed_code = method_code(before), method_code(changed)
    identity = method_name.encode() + erased
    different = {key for key in original_code if original_code[key] != changed_code[key]}
    if different != {identity}:
        raise RuntimeError("Owned throw edit changed another method: " + repr(different))
    path.write_bytes(changed)
    return {"changed_code": identity.decode(), "body_source": "independently authored typed throw",
            "other_code_and_descriptors": "unchanged", "before_sha256": hashlib.sha256(before).hexdigest(),
            "after_sha256": hashlib.sha256(changed).hexdigest()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    profiles = parser.add_mutually_exclusive_group()
    profiles.add_argument("--captured-member", action="store_true")
    profiles.add_argument("--plain-superclass", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-indexed-throw-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "indexed_terminal_throw/java").rglob("*.java"))
        harness_source = FIXTURES / "harness/IndexedTerminalThrowHarness.java"
        view_path = "fixture/View.class"
        source_signature, input_signature = SOURCE, INPUT
        signature_prefix = PREFIX
        if args.plain_superclass:
            source_dir = work / "plain-java/fixture"
            source_dir.mkdir(parents=True)
            root = source_dir / "PlainRoot.java"
            root.write_text("package fixture;\npublic class PlainRoot {\n"
                            "    public int root;\n"
                            "    public PlainRoot() { root = 17; }\n"
                            "    public int rootValue() { return root; }\n}\n")
            parent = source_dir / "PlainParent.java"
            parent.write_text("package fixture;\npublic class PlainParent extends PlainRoot {\n"
                              "    public long parent;\n"
                              "    public PlainParent() { parent = 0x123456789abcdefL; }\n"
                              "    public long parentValue() { return parent; }\n}\n")
            view_source = next(p for p in inputs if p.name == "View.java")
            declaration = "public final class View<S> implements Contract"
            content = view_source.read_text()
            if content.count(declaration) != 1:
                raise RuntimeError("Missing exact owned plain-superclass view declaration")
            replacement = source_dir / "View.java"
            replacement.write_text(content.replace(declaration,
                "public final class View<S> extends PlainParent implements Contract", 1))
            inputs = [p for p in inputs if p != view_source] + [replacement, root, parent]
            signature_prefix = b"<S:Ljava/lang/Object;>Lfixture/PlainParent;"
            source_signature = signature_prefix + b"Lfixture/Contract;"
            input_signature = signature_prefix + b"Lfixture/Contract<Lfixture/Element;>;"
            content = harness_source.read_text()
            creation = "View view = new View();"
            if content.count(creation) != 1:
                raise RuntimeError("Missing exact owned plain-superclass view creation")
            content = content.replace(creation, creation + "\n"
                "        if (View.class.getSuperclass() != PlainParent.class ||\n"
                "                PlainParent.class.getSuperclass() != PlainRoot.class ||\n"
                "                PlainRoot.class.getSuperclass() != Object.class)\n"
                "            throw new AssertionError(\"plain superclass chain changed\");\n"
                "        if (view.rootValue() != 17 || view.parentValue() != 0x123456789abcdefL)\n"
                "            throw new AssertionError(\"inherited constructor state changed\");\n"
                "        view.root = -71; view.parent = Long.MIN_VALUE;\n"
                "        if (view.rootValue() != -71 || view.parentValue() != Long.MIN_VALUE)\n"
                "            throw new AssertionError(\"inherited field mutation changed\");", 1)
            harness_source = work / "IndexedTerminalThrowHarness.java"
            harness_source.write_text(content)
        if args.captured_member:
            view_source = next(p for p in inputs if p.name == "View.java")
            content = view_source.read_text()
            if content.count("package fixture;\n") != 1 or content.count("public View() {}") != 1:
                raise RuntimeError("Missing exact owned view declaration")
            content = content.replace("package fixture;\n", "", 1)
            content = content.replace("public View() {}", "public View() {}\n    public Outer owner() { return Outer.this; }", 1)
            outer_source = work / "captured-java/fixture/Outer.java"
            outer_source.parent.mkdir(parents=True)
            outer_source.write_text("package fixture;\npublic class Outer {\n" + content + "}\n")
            inputs = [p for p in inputs if p != view_source] + [outer_source]
            content = harness_source.read_text()
            content = content.replace("import fixture.*;", "import fixture.*;\nimport fixture.Outer.View;", 1)
            content = content.replace('"fixture.View"', '"fixture.Outer$View"')
            original_creation = "View view = new View();"
            if content.count(original_creation) != 1:
                raise RuntimeError("Missing exact owned captured view creation")
            content = content.replace(original_creation,
                "Outer owner = new Outer(); View view = owner.new View();\n"
                "        if (view.owner() != owner) throw new AssertionError(\"outer identity changed\");\n"
                "        if (Modifier.isStatic(View.class.getModifiers()) != recovered)\n"
                "            throw new AssertionError(\"reported static binding missing\");")
            harness_source = work / "IndexedTerminalThrowHarness.java"
            harness_source.write_text(content)
            view_path = "fixture/Outer$View.class"
        verify.compile_local(inputs, originals)
        verify.compile_local([harness_source], harness, classpath=originals)
        harness_hashes = {p.relative_to(harness).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sorted(harness.rglob("*.class"))}
        view = originals / view_path
        receipt = {"result_bodies": [terminal_result_body(view), terminal_result_body(view, "first", "")], "flags": []}
        for name, descriptor in [("add", "(Ljava/lang/Object;)Z"),
                                 ("insert", "(ILjava/lang/Object;)V"),
                                 ("replace", "(ILjava/lang/Object;)Ljava/lang/Object;"),
                                 ("begin", "(Ljava/lang/Object;)V")]:
            receipt["flags"].append(bridge_flag(view, name, descriptor, 1, 0x1041))
        receipt["signature"] = change_signature(view, source_signature, input_signature)
        (work / "input-change.json").write_text(json.dumps(receipt, indent=2) + "\n")
        original_records = class_identity.compiler_classes(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - {"Lfixture/Contract;", "Lfixture/Base;"}
        methods = {k: v for k, v in all_methods.items() if k.split("->", 1)[0] in classes}
        dependency = work / "owned-contract.jar"
        with zipfile.ZipFile(dependency, "x", zipfile.ZIP_DEFLATED) as archive:
            for name in ("Contract", "Base"):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in originals.rglob("*.class")}
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "IndexedTerminalThrowHarness", "original"], "baseline")
        expected = ["failures=108,fresh-exceptions=108,stack-frame-cases=108",
                    "unchecked-arguments=retained,result-only-stack-difference=reported"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned baseline missed cases: " + baseline)

        def dex(source: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--classpath", dependency, "--output", destination,
                        *[p for p in sorted(source.rglob("*.class"))
                          if "L" + p.relative_to(source).as_posix()[:-6] + ";" in classes]], "d8-" + destination.name)
            return destination / "classes.dex"

        owned = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", owned], "inventory")
        inventory = parse_dexdump(dump, owned.name, dex_header(owned.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes
                or {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned inventory")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", owned, "--classpath", args.android_jar.resolve(),
                    "--classpath", dependency, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = [row for row in coverage["methods"] if row["status"] == "source-projected"]
        kinds = [row["projection_kind"] for row in rows]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] != 0
                or coverage["projected_method_count"] != (14 if args.captured_member else 6)
                or kinds.count("terminal-throw-interface-bridge") != 4
                or kinds.count("terminal-throw-result-bridge") != 2
                or any(row["instruction_count"] != 4 or not row.get("metadata_differences")
                       for row in rows if row["projection_kind"].startswith("terminal-throw-"))
                or coverage["status"] != "partial"):
            raise RuntimeError("Throw bodies or result-only stack frame receipt were lost")
        rebuilt = work / "rebuilt"
        if args.captured_member:
            bindings = coverage.get("class_source_bindings", [])
            if (len(bindings) != 1 or bindings[0].get("binding_kind") != "inner-to-static-member"
                    or not bindings[0].get("metadata_differences") or coverage["minimum_java_release"] != 25):
                raise RuntimeError("Captured terminal throw lost its proven outer source binding")
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency, release=25 if args.captured_member else 8)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        extras = set(actual_methods) - set(methods)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if (actual_classes != classes or not set(methods).issubset(actual_methods)
                or extras != helpers):
            raise RuntimeError("javac changed an original runtime descriptor")
        actual_records = class_identity.compiler_classes(rebuilt)
        retained = {row["identity"] for row in rows
                    if row["projection_kind"] == "terminal-throw-interface-bridge"}
        for owner in classes:
            wanted = copy.deepcopy(original_records[owner])
            after = actual_records[owner]
            if owner == "L" + view_path[:-6] + ";":
                wanted["signature"] = source_signature.decode()
                if args.captured_member:
                    wanted["inner_class"]["access"] |= 0x8
                    wanted["fields"][owner + "->this$0:Lfixture/Outer;"]["access"] &= ~0x1000
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                if after.get(key) != wanted.get(key):
                    raise RuntimeError("Terminal throw changed unreported class metadata: " + owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                if identity in retained:
                    metadata["access"] &= ~0x1040
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Terminal throw changed unreported method metadata: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "IndexedTerminalThrowHarness", "recovered"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original exception behavior changed")
        negative = work / "negative-originals"
        (negative / "fixture").mkdir(parents=True)
        for path in originals.glob("fixture/*.class"):
            (negative / "fixture" / path.name).write_bytes(path.read_bytes())
        negative_receipt = change_signature(negative / view_path, input_signature,
                                            signature_prefix + b"Lfixture/Contract<Ljava/lang/String;>;")
        (work / "negative-change.json").write_text(json.dumps(negative_receipt, indent=2) + "\n")
        rejected = work / "rejected"
        bad = dex(negative, work / "negative-dex")
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(bad), "--classpath",
                                 str(args.android_jar.resolve()), "--classpath", str(dependency), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic inheritance" not in result.stderr:
            raise RuntimeError("Complete original inherited contract was bypassed")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in originals.rglob("*.class")} or dependency_hash != hashlib.sha256(dependency.read_bytes()).hexdigest():
            raise RuntimeError("Original fixture or contract bytes changed")
        if harness_hashes != {p.relative_to(harness).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in sorted(harness.rglob("*.class"))}:
            raise RuntimeError("Original independently compiled JVM harness bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "captured_member": args.captured_member, "original_outer_identity": "unchanged",
                    "plain_superclass_chain": args.plain_superclass,
                    "original_superclass_metadata": "unchanged",
                    "inherited_constructor_state_and_field_mutation": "matched" if args.plain_superclass else "not applicable",
                    "ordered_failure_cases": 108, "fresh_exception_cases": 108, "stack_frame_cases": 108,
                    "runtime_descriptors": "unchanged", "parameter_casts_introduced": 0,
                    "original_metadata_and_complete_inventory": "unchanged except reported parent edge, member binding, bridge flags and helpers",
                    "generated_helpers": len(extras),
                    "retained_erased_bodies": 4, "result_only_bridges": 2,
                    "default_override": "original-throw-retained",
                    "result_only_bridge": "compiler-regenerated-with-reported-stack-difference",
                    "generic_contract_counterexample": "rejected", "coverage": "partial", "jvm_output": "matched"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
