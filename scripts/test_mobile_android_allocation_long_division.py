#!/usr/bin/env python3
"""Verify throwing long allocation preparations retain values, effects and handlers."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from scripts.test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "java-home", "compiler-java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-allocation-long-division-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "allocation_long_division/java"
        originals, harness = work / "originals", work / "harness"
        name = "AllocationLongDivisionHarness"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        sdk_hash, cli_hash = sha(sdk), sha(args.neverd)
        compiler.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        compiler.compile_local([FIXTURES / "harness" / (n + ".java") for n in (name, "AllocationLongDivisionCaller")],
                               harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (5, 13):
            raise RuntimeError("Owned long division allocation inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        excluded = {"Lfixture/Provider;"} if args.dependency_provider else set()
        classes = all_classes - excluded
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            jar.writestr("fixture/Provider.class", (originals / "fixture/Provider.class").read_bytes())
        dependency_hash = sha(dependency)

        def execute(runtime, label):
            outputs = []
            for route in range(4):
                for mode in range(30):
                    text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), str(mode)],
                                      f"{label}-{route}-{mode}")
                    if text != "attempts=2,long-division-allocation=matched\n":
                        raise RuntimeError("Frozen long division allocation observer changed")
                    outputs.append(text)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        texts = {p.name: p.read_text() for p in fixture.rglob("*.java")}
        wrong = [
            ("quotient-to-remainder", "Core.java", "Provider.left() / Provider.right()", "Provider.left() % Provider.right()", 0, 0),
            ("remainder-to-quotient", "Core.java", "Provider.left() % Provider.right()", "Provider.left() / Provider.right()", 0, 0),
            ("reversed-operands", "Core.java", "Provider.left() / Provider.right()", "Provider.right() / Provider.left()", 0, 0),
            ("unsigned-division", "Core.java", "Provider.left() / Provider.right()", "Long.divideUnsigned(Provider.left(), Provider.right())", 0, 0),
            ("narrowed-quotient", "Core.java", "Provider.left() / Provider.right()", "(long)(int)(Provider.left() / Provider.right())", 0, 0),
            ("unsigned-int-divisor", "Core.java", "(long)Provider.divisor()", "Integer.toUnsignedLong(Provider.divisor())", 0, 0),
            ("repeated-provider", "Core.java", "Provider.left() / Provider.right()", "(Provider.left() + Provider.left()) / Provider.right()", 0, 0),
            ("late-numerator", "Core.java", "Provider.left() / Provider.right()", "(Provider.left()*0L + Provider.right()*0L + Trace.left) / Trace.right", 0, 24),
            ("lost-unused-division", "Core.java", "(Provider.left() / Provider.right()) * 0L", "(Provider.left()*0L + Provider.right()*0L)", 0, 28),
            ("lost-unused-remainder", "Core.java", "(Provider.left() % Provider.right()) * 0L", "(Provider.left()*0L + Provider.right()*0L)", 0, 29),
            ("lost-zero-product", "Core.java", "(Provider.left() / Provider.right()) * 0L", "(Provider.left() / Provider.right())", 0, 0),
            ("early-field-write", "Pair.java", "Trace.partial=this; Trace.hit(4);", "Trace.partial=this; this.a=a; Trace.hit(4);", 0, 19),
            ("class-initialization-order", "Target.java", "Trace.initializations*10+1", "Trace.initializations*10+2", 0, 0),
            ("wrapped-provider-exception", "Provider.java", "Trace.hit(2);", "try { Trace.hit(2); } catch (RuntimeException e) { throw new IllegalArgumentException(e); }", 0, 17),
            ("swallowed-provider-exception", "Provider.java", "Trace.hit(2);", "try { Trace.hit(2); } catch (RuntimeException e) {}", 0, 17),
            ("replaced-rethrow", "Core.java", "throw failure;", "throw new ArithmeticException();", 3, 13),
            ("swallowed-divide-failure", "Core.java", "throw failure;", "return null;", 3, 13),
            ("lost-cleanup", "Core.java", "Trace.hit(9);", ";", 3, 27),
        ]
        for label, filename, anchor, replacement, route, mode in wrong:
            text = texts[filename]
            if anchor not in text:
                raise RuntimeError("Owned long division counterexample anchor changed")
            if label == "early-field-write":
                text = text.replace("this.a=a;this.b=b;", "this.b=b;")
            changed = work / label / filename
            changed.parent.mkdir()
            changed.open("x").write(text.replace(anchor, replacement))
            wrong_classes = work / (label + "-classes")
            compiler.compile_local([changed], wrong_classes, classpath=originals, release=8)
            result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (wrong_classes, originals, harness))),
                                     name, str(route), str(mode)], text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned long division allocation" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect division semantics: " + label)
        classpath = ["--classpath", sdk]
        if excluded:
            classpath += ["--classpath", dependency]
        encoded = work / "dex"
        encoded.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--release", "--min-api", "24", *classpath, "--output", encoded, *paths], "d8")
        dex = encoded / "classes.dex"
        dex_hash = sha(dex)
        if list(encoded.glob("classes*.dex")) != [dex]:
            raise RuntimeError("Owned long division DEX inventory changed")
        dump = verify.run([verify.dexdump, "-d", "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original long division entries")
        if "div-long" not in dump or "rem-long" not in dump:
            raise RuntimeError("Owned DEX no longer tests both throwing long operations")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Long division recovery changed original definitions or metadata")
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        allowed = {o + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt = work / "rebuilt"
        dependencies = [dependency] if excluded else []
        release = coverage.get("minimum_java_release", 8)
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=release,
                             classpath=os.pathsep.join(map(str, dependencies)) if dependencies else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if (actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed
                or any(actual_methods[m] != all_methods[m] for m in methods)):
            raise RuntimeError("Long division recovery lost or invented runtime definitions")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(facts[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if actual != wanted or any(after_exceptions[m] != exceptions[m] for m in facts[owner]["methods"]):
                raise RuntimeError("Long division metadata/roles/Exceptions changed: " + owner)
        if execute((rebuilt, harness, *dependencies), "rebuilt") != baseline:
            raise RuntimeError("Long division values, snapshots, effects, allocation or failures changed")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(sdk) != sdk_hash
                or sha(dependency) != dependency_hash or sha(dex) != dex_hash or sha(args.neverd) != cli_hash):
            raise RuntimeError("Frozen original program, observer, SDK or CLI changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": 120, "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "dependency_provider": args.dependency_provider, "minimum_java_release": release,
                   "metadata_roles_exceptions_and_runtime_definitions": "unchanged",
                   "signed_wide_values_effect_order_partial_fields_snapshots_and_handler_identity": "matched"}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
