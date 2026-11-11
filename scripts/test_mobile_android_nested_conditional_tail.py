#!/usr/bin/env python3
"""Verify nested conditional construction before later outer argument effects."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
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
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--present-branch", action="store_true")
    parser.add_argument("--comparison-tail", choices=("long", "float-less", "float-greater", "double-less", "double-greater"))
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-nested-conditional-tail-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "nested_conditional_tail/java"
        inputs = sorted(fixture.rglob("*.java"))
        core = (fixture / "fixture/Core.java").read_text()
        selection = "(value = provider.payload) == null ? provider.fallback() : value"
        if args.present_branch:
            if core.count(selection) != 1:
                raise RuntimeError("Original nested conditional source anchor changed")
            replacement = "(value = provider.payload) != null ? value : provider.fallback()"
            core = core.replace(selection, replacement)
            selection = replacement
            changed = work / "present-java/fixture/Core.java"
            changed.parent.mkdir(parents=True)
            changed.open("x").write(core)
            inputs = [changed if p.name == "Core.java" else p for p in inputs]
        harness_sources = [FIXTURES / "harness" / (name + ".java") for name in
                           ("NestedConditionalTailCaller", "NestedConditionalTailHarness")]
        comparison = None
        if args.comparison_tail:
            kind = args.comparison_tail.split("-", 1)[0]
            relation = ">" if args.comparison_tail.endswith("greater") else "<"
            def operand(value):
                if kind == "float":
                    return "Float.intBitsToFloat((int)" + value + ")"
                if kind == "double":
                    return "Double.longBitsToDouble(" + value + ")"
                return value
            comparison = "(" + operand("provider.finish(bits)") + " " + relation + " " + operand("bits") + " ? 1L : 0L)"
            if core.count("provider.finish(bits)") != 1:
                raise RuntimeError("Original comparison tail call anchor changed")
            core = core.replace("provider.finish(bits)", comparison)
            changed = work / "comparison-java/fixture/Core.java"
            changed.parent.mkdir(parents=True)
            changed.open("x").write(core)
            inputs = [changed if path.name == "Core.java" else path for path in inputs]
            observer = harness_sources[1].read_text()
            if kind == "long":
                pairs = [("Long.MIN_VALUE", "Long.MAX_VALUE"), ("Long.MAX_VALUE", "Long.MIN_VALUE"),
                         ("0", "0"), ("Long.MIN_VALUE", "Long.MIN_VALUE"), ("Long.MAX_VALUE", "Long.MAX_VALUE"),
                         ("-1", "0"), ("0", "-1"), ("0", "Long.MAX_VALUE")] * 2
            else:
                values = ({"nan": "0x7fc01234L", "nz": "0x80000000L", "ni": "0xff800000L", "pi": "0x7f800000L",
                           "one": "0x3f800000L", "minus": "0xbf800000L", "sub": "1L", "nsub": "0x80000001L",
                           "max": "0x7f7fffffL", "normal": "0x00800000L"} if kind == "float" else
                          {"nan": "0x7ff8123456789abcL", "nz": "0x8000000000000000L", "ni": "0xfff0000000000000L",
                           "pi": "0x7ff0000000000000L", "one": "0x3ff0000000000000L", "minus": "0xbff0000000000000L",
                           "sub": "1L", "nsub": "0x8000000000000001L", "max": "0x7fefffffffffffffL", "normal": "0x0010000000000000L"})
                values["zero"] = "0L"
                pairs = [(values[a], values[b]) for a, b in (("nan", "zero"), ("zero", "nan"), ("nz", "zero"),
                    ("zero", "nz"), ("ni", "pi"), ("pi", "ni"), ("one", "zero"), ("minus", "zero"),
                    ("sub", "zero"), ("nsub", "zero"), ("nan", "nan"), ("max", "pi"), ("pi", "max"),
                    ("one", "one"), ("ni", "ni"), ("normal", "sub"))]
            matrix = "new long[][]{" + ",".join("{" + a + "," + b + "}" for a, b in pairs) + "}"
            anchor = "            long returnedBits = inputBits ^ 0x0102030405060708L;"
            if observer.count(anchor) != 1:
                raise RuntimeError("Original comparison observer input anchor changed")
            observer = observer.replace(anchor, anchor + "\n" +
                "            if (mode >= 28) { long[] pair = " + matrix + "[(mode - 28 + attempt * 7) % 16]; returnedBits = pair[0]; inputBits = pair[1]; }\n" +
                "            final long originalBits = inputBits, actualReturnedBits = returnedBits;\n" +
                "            long wantedBits = " + operand("actualReturnedBits") + " " + relation + " " + operand("originalBits") + " ? 1L : 0L;")
            # The observer lambda captures final values after independently authored input selection.
            before, after = observer.split("            final long originalBits", 1)
            after = after.replace("inputBits", "originalBits").replace("returnedBits", "actualReturnedBits")
            observer = before + "            final long originalBits" + after
            observer = observer.replace("Trace.originalBits", "Trace.inputBits")
            observer = observer.replace("Trace.outer.bits == actualReturnedBits", "Trace.outer.bits == wantedBits")
            observer = observer.replace("0 : actualReturnedBits)", "0 : wantedBits)")
            # Keep the final aliases initialized from the mutable pre-observer local inputs.
            observer = observer.replace("final long originalBits = originalBits, actualReturnedBits = actualReturnedBits;",
                                        "final long originalBits = inputBits, actualReturnedBits = returnedBits;")
            changed = work / "NestedConditionalTailHarness.java"
            changed.open("x").write(observer)
            harness_sources[1] = changed
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals, release=8)
        verify.compile_local(harness_sources, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 11:
            raise RuntimeError("Original nested conditional tail inventory changed")
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        providers = {"Lfixture/Provider;"}
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            jar.writestr("fixture/Provider.class", (originals / "fixture/Provider.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def command(runtime, route, mode):
            return [verify.java, "-cp", os.pathsep.join(map(str, runtime)), "NestedConditionalTailHarness",
                    str(route), str(mode)]

        def execute(runtime, label):
            outputs = []
            for route in range(4):
                for mode in range(44 if args.comparison_tail else 28):
                    output = verify.run(command(runtime, route, mode), f"{label}-{route}-{mode}")
                    pattern = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                        raise RuntimeError("Original nested construction observer output changed: " + output)
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original nested conditional tail DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original nested construction entry")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-nested-disassembly")
        witnessed = []
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] != "make":
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
            def unique(predicate):
                positions = [i for i, (op, ref) in enumerate(ops) if predicate(op, ref)]
                if len(positions) != 1:
                    raise RuntimeError("Missing exact original nested allocation witness")
                return positions[0]
            outer = unique(lambda op, ref: op == "new-instance" and "Lfixture/Envelope;" in ref)
            inner = unique(lambda op, ref: op == "new-instance" and "Lfixture/Item;" in ref)
            branches = [i for i, (op, ref) in enumerate(ops) if op.startswith("if-")]
            if len(branches) != (2 if comparison else 1):
                raise RuntimeError("Original conditional comparison branch inventory changed")
            branch = branches[0]
            fallback = unique(lambda op, ref: op.startswith("invoke-virtual") and "Lfixture/Provider;.fallback:" in ref)
            child_init = unique(lambda op, ref: op.startswith("invoke-direct") and "Lfixture/Item;.<init>:" in ref)
            tail = unique(lambda op, ref: op.startswith("invoke-virtual") and "Lfixture/Provider;.finish:" in ref)
            extra = unique(lambda op, ref: op == "iget-object" and "Lfixture/Provider;.extra:" in ref)
            parent_init = unique(lambda op, ref: op.startswith("invoke-direct") and "Lfixture/Envelope;.<init>:" in ref)
            if (not outer < inner < branch < fallback < child_init < tail < extra < parent_init
                    or sum(op == "new-instance" for op, _ in ops) != 2
                    or sum(op == "iget-object" and "Lfixture/Provider;.payload:" in ref for op, ref in ops) != 1
                    or ops[-1][0] != "return-object"):
                raise RuntimeError("D8 changed nested selection and later-argument order")
            if comparison:
                expected = {"long": "cmp-long", "float-less": "cmpg-float", "float-greater": "cmpl-float",
                            "double-less": "cmpg-double", "double-greater": "cmpl-double"}[args.comparison_tail]
                compare = unique(lambda op, ref: op == expected)
                if not tail < compare < branches[1] < extra:
                    raise RuntimeError("D8 changed original comparison result evaluation order")
            witnessed.append(ops)
        if len(witnessed) != 1:
            raise RuntimeError("Missing original nested constructor argument witness")
        (work / "original-nested-opcodes.json").open("x").write(json.dumps(witnessed, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("minimum_java_release") != 14 or coverage.get("class_source_bindings")
                or coverage.get("field_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Nested construction changed original entries or contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
            classpath=dependency if args.dependency_provider else None, release=coverage["minimum_java_release"])
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Nested construction invented or omitted a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != actual_records[owner].get(key):
                    raise RuntimeError("Original nested construction class metadata changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                if actual_records[owner]["methods"].get(identity) != metadata or actual_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original nested construction method metadata changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Nested construction changed initialization, arguments or effects")
        inner_expression = "new Item(" + selection + ")"
        wrong_programs = (
            ("early-inner", "Core", ((inner_expression, "item"), ("Object value;", "Object value; Item item = " + inner_expression + ";")), 0),
            ("early-field", "Core", (("(value = provider.payload)", "value"), ("Object value;", "Object value = provider.payload;")), 0),
            ("early-tail", "Core", (("provider.finish(bits)", "saved"), ("Object value;", "Object value; long saved = provider.finish(bits);")), 0),
            ("early-extra", "Core", (("provider.extra", "saved"), ("Object value;", "Object value; Object saved = provider.extra;")), 0),
            ("duplicate-fallback", "Core", (("provider.fallback()", "(new Object[]{provider.fallback(), provider.fallback()})[1]"),), 1),
            ("missing-inner", "Core", ((inner_expression, "null"),), 0),
            ("missing-tail", "Core", (("provider.finish(bits)", "0L"),), 0),
            ("duplicate-tail", "Core", (("provider.finish(bits)", "(provider.finish(bits) + provider.finish(bits))"),), 0),
            ("late-item-store", "Envelope", (("item = value;\n        Trace.step(9);", "Trace.step(9);\n        item = value;"),), 12),
            ("wrong-tail", "Provider", (("return Trace.tailBits;", "return bits;"),),
             28 if args.comparison_tail == "long" else 33 if comparison and relation == ">" else 32 if comparison else 0),
        )
        if comparison:
            wrong_programs += (("inverted-comparison", "Core", ((comparison, "(1L - " + comparison + ")"),), 0),)
            if kind != "long":
                incorrect = "(!(" + operand("provider.finish(bits)") + (" <= " if relation == ">" else " >= ") + operand("bits") + ") ? 1L : 0L)"
                wrong_programs += (("wrong-nan-polarity", "Core", ((comparison, incorrect),), 28),)
        for label, owner, changes, mode in wrong_programs:
            changed_source = core if owner == "Core" else (fixture / "fixture" / (owner + ".java")).read_text()
            for before, after in changes:
                if changed_source.count(before) != 1:
                    raise RuntimeError("Original nested counterexample anchor changed: " + label)
                changed_source = changed_source.replace(before, after)
            changed = work / label / "fixture" / (owner + ".java")
            changed.parent.mkdir(parents=True)
            changed.open("x").write(changed_source)
            wrong = work / (label + "-classes")
            verify.compile_local([changed], wrong, classpath=originals, release=8)
            result = subprocess.run(command((wrong, originals, harness), 1, mode), text=True,
                capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned nested conditional tail" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect nested construction: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original nested inputs or immutable observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
            "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_provider": args.dependency_provider,
            "present_branch": args.present_branch, "comparison_tail": args.comparison_tail,
            "independent_wrong_programs": len(wrong_programs),
            "all_original_entries_metadata_and_exceptions": "unchanged", "all_java": "compiled",
            "both_initialization_sites_conditional_calls_and_later_arguments": "matched",
            "partial_defaults_stores_opaque_wide_results_and_failures": "matched",
            "original_inputs_and_immutable_callers": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
