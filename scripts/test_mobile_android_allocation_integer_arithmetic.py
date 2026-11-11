#!/usr/bin/env python3
"""Verify total integer preparations in original allocation arguments."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
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
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-target", action="store_true")
    parser.add_argument("--calls", action="store_true")
    parser.add_argument("--multiply-fields", action="store_true")
    parser.add_argument("--divide-fields", action="store_true")
    args = parser.parse_args()
    if sum((args.calls, args.multiply_fields, args.divide_fields)) > 1:
        parser.error("Choose calls, field multiplication, or nonzero literal division")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-integer-arithmetic-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "allocation_integer_arithmetic/java"
        harness_sources = [FIXTURES / "harness" / (name + ".java") for name in
                           ("AllocationIntegerArithmeticCaller", "AllocationIntegerArithmeticHarness")]
        if args.divide_fields:
            authored = work / "division-source"
            shutil.copytree(fixture, authored)
            core = authored / "fixture/Core.java"
            text = core.read_text()
            replacements = {
                "new Target(x - delta, y - delta, x + delta, y + delta)":
                    "new Target(Trace.x / 5, Trace.y % 5, Trace.x / 13, Trace.y % 13)",
                "new Target(x - 13, y - 13, x + 13, y + 13)":
                    "new Target(Trace.x / -1, Trace.y % -1, Trace.x / 257, Trace.y % 257)",
            }
            for before, after in replacements.items():
                if text.count(before) != 1:
                    raise RuntimeError("Original division source anchor changed")
                text = text.replace(before, after)
            core.write_text(text)
            fixture = authored
            oracle = work / "division-harness"
            oracle.mkdir()
            for original in harness_sources:
                text = original.read_text()
                if original.name.endswith("Harness.java"):
                    text = text.replace("require(Trace.failAt == 0 && result != previous",
                        "int actualX = attempt == 1 ? 99 : x, actualY = attempt == 1 ? -71 : y;\n                require(Trace.failAt == 0 && result != previous")
                    replacements = {
                        "x - delta": "(int)((long)actualX / (kind == 0 ? 5 : -1))",
                        "y - delta": "(int)((long)actualY % (kind == 0 ? 5 : -1))",
                        "x + delta": "(int)((long)actualX / (kind == 0 ? 13 : 257))",
                        "y + delta": "(int)((long)actualY % (kind == 0 ? 13 : 257))",
                    }
                    for before, after in replacements.items():
                        if text.count(before) != 1:
                            raise RuntimeError("Original division observer anchor changed")
                        text = text.replace(before, after)
                (oracle / original.name).open("x").write(text)
            harness_sources = sorted(oracle.glob("*.java"))
        if args.multiply_fields:
            authored = work / "field-source"
            shutil.copytree(fixture, authored)
            core = authored / "fixture/Core.java"
            text = core.read_text().replace(" - ", " * ").replace(" + ", " * ")
            for value in ("x", "y"):
                text = text.replace(value + " * ", "Trace." + value + " * ")
            core.write_text(text)
            fixture = authored
            oracle = work / "field-harness"
            oracle.mkdir()
            for original in harness_sources:
                text = original.read_text()
                if original.name.endswith("Harness.java"):
                    text = text.replace(
                        "require(Trace.failAt == 0 && result != previous",
                        "int actualX = attempt == 1 ? 99 : x, actualY = attempt == 1 ? -71 : y;\n                require(Trace.failAt == 0 && result != previous")
                    for value, actual in (("x", "actualX"), ("y", "actualY")):
                        text = text.replace(value + " - delta", actual + " * delta").replace(value + " + delta", actual + " * delta")
                (oracle / original.name).write_text(text)
            harness_sources = sorted(oracle.glob("*.java"))
        if args.calls:
            authored = work / "call-source"
            shutil.copytree(fixture, authored)
            core = authored / "fixture/Core.java"
            text = core.read_text()
            for value in ("x", "y"):
                for operation in (" - ", " + "):
                    text = text.replace(value + operation, "Trace.read(" + value + ")" + operation)
            core.write_text(text)
            trace = authored / "fixture/Trace.java"
            text = trace.read_text()
            if not text.endswith("}\n"):
                raise RuntimeError("Original getter source anchor changed")
            trace.write_text(text[:-2] + "    public static int read(int value) { event(5); return value; }\n}\n")
            fixture = authored
            oracle = work / "call-harness"
            oracle.mkdir()
            for original in harness_sources:
                text = original.read_text()
                if original.name.endswith("Harness.java"):
                    text = text.replace("variant == 7 ? 7 : 0", "variant == 7 ? 7 : variant == 8 ? 5 : 0")
                    text = text.replace("else if (Trace.failAt == 7)", "else if (Trace.failAt == 7 || Trace.failAt == 5)")
                    text = text.replace("if (attempt == 1) expectedTrace = Trace.failAt == 1 ? 1 : 17;",
                        "if (attempt == 1) expectedTrace = Trace.failAt == 1 ? 1 : Trace.failAt == 5 ? 15 : 155557;")
                    text = text.replace("else if (Trace.failAt != 1) expectedTrace = expectedTrace * 10 + 7;",
                        "else if (Trace.failAt != 1) { if (Trace.failAt == 5) expectedTrace = expectedTrace * 10 + 5; else expectedTrace = expectedTrace * 100000 + 55557; }")
                (oracle / original.name).write_text(text)
            harness_sources = sorted(oracle.glob("*.java"))
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local(harness_sources, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != (8 if args.calls else 7):
            raise RuntimeError("Owned integer-arithmetic inventory changed")
        providers = {"Lfixture/Target;"} if args.dependency_target else set()
        classes = all_classes - providers
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if providers:
            classpath += ["--classpath", dependency]
        def execute(runtime, label):
            result = []
            for variant in range(9 if args.calls else 8):
                for kind in range(2):
                    for route in range(4):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "AllocationIntegerArithmeticHarness", str(variant), str(kind), str(route)],
                            f"{label}-{variant}-{kind}-{route}")
                        if not re.fullmatch(f"variant={variant},kind={kind},route={route},trace=[0-9]+\n", output):
                            raise RuntimeError("Owned integer argument observer changed")
                        result.append(output)
            return result
        baseline = execute((originals, harness), "baseline")
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        encoded = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [encoded]:
            raise RuntimeError("Owned integer arithmetic argument DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", encoded], "sdk-inventory")
        inventory = parse_dexdump(dump, encoded.name, dex_header(encoded.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed original integer arithmetic argument runtime entries")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", encoded], "original-array-disassembly")
        witnessed = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] not in ("make", "bias"):
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            if (ops[0] != "new-instance" or ops.count("new-instance") != 1
                    or ops[-2].split("/")[0] != "invoke-direct" or ops[-1] != "return-object"):
                raise RuntimeError("D8 changed original arithmetic/allocation witness")
            if args.calls and sum(op.startswith("invoke-static") for op in ops) != 4:
                raise RuntimeError("D8 lost original integer-producing calls")
            arithmetic = [op.split("/", 1)[0] for op in ops]
            if args.multiply_fields and (arithmetic.count("mul-int") != 4 or ops.count("sget") != 4):
                raise RuntimeError("D8 changed original field multiplication witness")
            if not args.multiply_fields and not args.divide_fields and name[1] == "make" and (arithmetic.count("sub-int") != 2 or arithmetic.count("add-int") != 2):
                raise RuntimeError("D8 changed the four original register arithmetic operations")
            if not args.divide_fields and name[1] == "bias" and not any(op.startswith("mul-int/lit" if args.multiply_fields else "add-int/lit") for op in ops):
                raise RuntimeError("Missing original literal-addition witness")
            if args.divide_fields:
                suffix = "/lit8" if name[1] == "make" else "/lit16"
                if (not any(op == "div-int" + suffix for op in ops)
                        or not any(op == "rem-int" + suffix for op in ops)
                        or ops.count("sget") < 2):
                    raise RuntimeError("D8 lost original nonzero literal division/remainder field witnesses")
            witnessed.add(name[1])
        if witnessed != {"make", "bias"}:
            raise RuntimeError("Missing original integer arithmetic witnesses")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", encoded, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Integer arithmetic recovery changed original contracts or release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=8,
                             classpath=dependency if providers else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Integer arithmetic recovery invented or lost runtime entries")
        actual, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original integer arithmetic argument class metadata changed: " + owner + ":" + key)
            for identity, row in before["methods"].items():
                if after["methods"].get(identity) != row or actual_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original integer arithmetic argument method metadata/Exceptions changed")
        runtime = (rebuilt, harness, dependency) if providers else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Integer arithmetic argument, identity, failure or class-init order changed")
        original = (fixture / "fixture/Core.java").read_text()
        marker = "new Target(x - delta, y - delta, x + delta, y + delta)"
        bias = "new Target(x - 13, y - 13, x + 13, y + 13)"
        mutants = {
            "checked-overflow": (marker, "new Target(Math.subtractExact(x, delta), Math.subtractExact(y, delta), Math.addExact(x, delta), Math.addExact(y, delta))", 0, 0),
            "late-input": (marker, "new Target(Trace.x - delta, Trace.y - delta, Trace.x + delta, Trace.y + delta)", 4, 0),
            "literal-sign": (bias, "new Target(x + 13, y - 13, x + 13, y + 13)", 4, 1),
        }
        if args.multiply_fields:
            marker = "new Target(Trace.x * delta, Trace.y * delta, Trace.x * delta, Trace.y * delta)"
            bias = "new Target(Trace.x * 13, Trace.y * 13, Trace.x * 13, Trace.y * 13)"
            mutants = {
                "checked-overflow": (marker, "new Target(Math.multiplyExact(Trace.x, delta), Math.multiplyExact(Trace.y, delta), Math.multiplyExact(Trace.x, delta), Math.multiplyExact(Trace.y, delta))", 5, 0),
                "early-field-read": (marker, "new Target(x * delta, y * delta, x * delta, y * delta)", 4, 0),
                "literal-sign": (bias, "new Target(Trace.x * -13, Trace.y * 13, Trace.x * 13, Trace.y * 13)", 4, 1),
            }
        if args.divide_fields:
            marker = "new Target(Trace.x / 5, Trace.y % 5, Trace.x / 13, Trace.y % 13)"
            bias = "new Target(Trace.x / -1, Trace.y % -1, Trace.x / 257, Trace.y % 257)"
            mutants = {
                "divisor-sign": (marker, marker.replace("/ 5", "/ -5"), 4, 0),
                "division-remainder": (marker, marker.replace("Trace.x / 5", "Trace.x % 5"), 4, 0),
                "early-field-read": (marker, marker.replace("Trace.x", "x").replace("Trace.y", "y"), 4, 0),
                "unsigned-division": (marker, marker.replace("Trace.x / 5", "Integer.divideUnsigned(Trace.x, 5)"), 0, 0),
                "zero-divisor": (marker, marker.replace("/ 5", "/ 0"), 4, 0),
                "wide-literal": (bias, bias.replace("257", "256"), 0, 1),
                "floor-remainder": (marker, marker.replace("Trace.y % 5", "Math.floorMod(Trace.y, 5)"), 4, 0),
                "checked-overflow": (bias, bias.replace("Trace.x / -1", "Math.negateExact(Trace.x)"), 0, 1),
                "lost-argument": (marker, marker.replace("Trace.y % 13", "0"), 4, 0),
            }
        if args.calls:
            for label, (before, after, variant, kind) in list(mutants.items()):
                for expression in ("x", "y"):
                    before = before.replace(expression + " - ", "Trace.read(" + expression + ") - ").replace(expression + " + ", "Trace.read(" + expression + ") + ")
                    after = after.replace("(" + expression + ",", "(Trace.read(" + expression + "),")
                    after = re.sub(r"(?<![A-Za-z0-9_.])" + expression + r"(?= [+-] )", "Trace.read(" + expression + ")", after)
                after = after.replace("Trace.x", "Trace.read(Trace.x)").replace("Trace.y", "Trace.read(Trace.y)")
                mutants[label] = (before, after, variant, kind)
        for label, (before, after, variant, kind) in mutants.items():
            if original.count(before) != 1:
                raise RuntimeError("Owned integer arithmetic source anchor changed")
            wrong = work / label / "fixture/Core.java"
            wrong.parent.mkdir(parents=True)
            wrong.write_text(original.replace(before, after))
            wrong_classes = work / (label + "-classes")
            verify.compile_local([wrong], wrong_classes, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong_classes, originals, harness))),
                                    "AllocationIntegerArithmeticHarness", str(variant), str(kind), "1"],
                                   text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "integer arithmetic allocation contract" not in result.stderr:
                raise RuntimeError("Immutable observer missed integer arithmetic mutation: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, frozen callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "calls": args.calls, "multiply_fields": args.multiply_fields, "divide_fields": args.divide_fields,
                    "independent_wrong_programs": len(mutants),
                    "attempts_per_case": 2, "dependency_target": args.dependency_target,
                    "original_metadata_and_runtime_inventory": "matched",
                    "overflow_literals_input_snapshots_and_init_order": "matched", "original_failure_identity": "matched",
                    "overflow_late_input_and_literal_counterexamples": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0
if __name__ == "__main__":
    raise SystemExit(main())
