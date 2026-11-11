#!/usr/bin/env python3
"""Verify method-owned intersection constructor bounds and original allocation witnesses."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from scripts.test_mobile_android_visibility_generic_bridge import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_visibility_generic_bridge import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-intersection-constructor-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "intersection_constructor_witness/java"
        originals, harness = work / "originals", work / "harness"
        name = "IntersectionConstructorWitnessHarness"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        sdk_hash, cli_hash = sha(sdk), sha(args.neverd)
        compiler.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        compiler.compile_local([FIXTURES / "harness" / ("IntersectionConstructorWitness" + n + ".java") for n in ("Caller", "Harness")], harness, classpath=originals, release=11)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (7, 14):
            raise RuntimeError("Owned intersection constructor inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        identity = "Lfixture/Product;-><init>(Lfixture/Bound;)V"
        signature = "<T:Lfixture/Bound;:Lfixture/Flag;>(TT;)V"
        if facts["Lfixture/Product;"]["methods"][identity]["signature"] != signature:
            raise RuntimeError("Original constructor method scope changed")
        providers = {"Lfixture/Bound;", "Lfixture/Flag;", "Lfixture/Else;", "Lfixture/Product;"}
        excluded = providers if args.dependency_provider else set()
        classes = all_classes - excluded
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        dependency = work / "owned-provider.jar"

        def write_provider(destination, source):
            with zipfile.ZipFile(destination, "x", compression=zipfile.ZIP_DEFLATED) as jar:
                for owner in sorted(providers):
                    entry = owner[1:-1] + ".class"
                    jar.writestr(entry, (source / entry).read_bytes())

        write_provider(dependency, originals)
        dependency_hash = sha(dependency)

        def execute(runtime, label):
            outputs = []
            for route in range(6):
                for future in (("false",) if route == 5 else ("false", "true")):
                    for mode in range(12):
                        text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), future, str(mode), "false"], f"{label}-{route}-{future}-{mode}")
                        if text != "attempts=2,intersection-constructor-witness=matched\n":
                            raise RuntimeError("Frozen intersection constructor observer changed")
                        outputs.append(text)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline) + "\n")
        wrong = [
            ("lost-init", "Product.java", "Trace.hit(2);", ";", 0, 0),
            ("lost-init-callback", "Product.java", "Trace.initAction.run();", ";", 0, 0),
            ("wrong-product", "Product.java", "Trace.product = this;", "Trace.product = null;", 0, 0),
            ("wrong-value", "Product.java", "Trace.value = value;", "Trace.value = null;", 0, 0),
            ("wrong-flag", "Product.java", "Trace.flag = flag;", "Trace.flag = null;", 0, 0),
            ("lost-read", "Product.java", "Trace.observed = value.number;", "Trace.observed = 0;", 0, 0),
            ("lost-dispatch", "Product.java", "flag.touch();", ";", 0, 0),
            ("duplicate-dispatch", "Product.java", "flag.touch();", "flag.touch(); flag.touch();", 0, 0),
            ("lost-callback", "Product.java", "Trace.action.run();", ";", 0, 0),
            ("duplicate-callback", "Product.java", "Trace.action.run();", "Trace.action.run(); Trace.action.run();", 0, 0),
            ("wrapped-checked", "Product.java", "Trace.action.run();", "try { Trace.action.run(); } catch (Throwable failure) { throw new IllegalStateException(failure); }", 0, 8),
            ("lost-completion", "Product.java", "Trace.hit(7);", ";", 0, 0),
            ("wrong-self", "Core.java", "new Product(this)", "new Product(new Value())", 5, 0),
            ("lost-nested-tail", "Trace.java", "Trace.hit(8);", ";", 3, 0),
        ]
        for label, filename, anchor, replacement, route, mode in wrong:
            text = (fixture / "fixture" / filename).read_text()
            if text.count(anchor) != 1:
                raise RuntimeError("Constructor counterexample anchor changed: " + label)
            source = work / label / filename
            source.parent.mkdir()
            source.open("x").write(text.replace(anchor, replacement))
            compiled = work / (label + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), name, str(route), "true", str(mode), "false"], text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned intersection constructor witness mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program: " + label)

        classpath = ["--classpath", sdk] + (["--classpath", dependency] if excluded else [])
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        dexdir = work / "dex"
        dexdir.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dexdir, *paths], "d8")
        dex = dexdir / "classes.dex"
        dex_hash = sha(dex)
        if list(dexdir.glob("classes*.dex")) != [dex]:
            raise RuntimeError("Owned constructor DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if {row["descriptor"] for row in inventory["classes"]} != classes or {row["identity"] for row in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original constructor declaration")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        expected_status = {m: "recovered" if facts[m.split("->", 1)[0]]["methods"][m]["code"] else "declaration-only" for m in methods}
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count") or coverage["status"] != "recovered"
                or any(row["status"] != expected_status[row["identity"]] for row in coverage["methods"])
                or coverage["recovered_method_count"] != sum(s == "recovered" for s in expected_status.values())
                or coverage.get("declaration_only_method_count", 0) != sum(s == "declaration-only" for s in expected_status.values())
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Constructor recovery changed original coverage or declaration contracts")
        rebuilt = work / "rebuilt"
        release = coverage.get("minimum_java_release", 8)
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency if excluded else None, release=release)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        allowed_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed_helpers:
            raise RuntimeError("Constructor recovery added or omitted an original runtime entry")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for c in classes:
            wanted, actual = copy.deepcopy(facts[c]), copy.deepcopy(after[c])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if wanted.pop("major") != 52 or actual.pop("major") != 44 + release:
                raise RuntimeError("Constructor fixture compiler release changed")
            if wanted != actual or any(exceptions[m] != after_exceptions[m] for m in facts[c]["methods"]):
                raise RuntimeError("Original constructor Code, roles, Signature, Exceptions or parameter metadata changed: " + c)
        runtime = (rebuilt, harness, dependency) if excluded else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Constructor recovery changed initialization, checks, dispatch or exception identity")

        controls = [
            ("missing-method-slot", signature.replace("(TT;)", "(TMissing;)")),
            ("wrong-class-bound", signature.replace("T:Lfixture/Bound;", "T:Ljava/lang/Object;")),
            ("missing-interface", signature.replace("Lfixture/Flag;", "Lfixture/Missing;")),
            ("extra-interface", signature.replace(":Lfixture/Flag;>", ":Lfixture/Flag;:Lfixture/Else;>")),
            ("extra-formal", signature.replace(">(", "U:Ljava/lang/Object;>(")),
            ("different-interface", signature.replace("Lfixture/Flag;", "Lfixture/Else;")),
            ("wrong-result", signature.replace(")V", ")Ljava/lang/Object;")),
            ("generic-throws", signature + "^TT;"),
        ]
        for label, changed_signature in controls + [("missing-witness", None)]:
            changed_originals = work / (label + "-originals")
            shutil.copytree(originals, changed_originals)
            target = changed_originals / ("fixture/Product.class" if changed_signature else "fixture/Value.class")
            code_before = method_code(target.read_bytes())
            if changed_signature:
                receipt = change_signature(target, signature.encode(), changed_signature.encode())
                if compiler_facts(changed_originals)["Lfixture/Product;"]["methods"][identity]["signature"] != changed_signature:
                    raise RuntimeError("Constructor control lost its exact original scoped Signature")
            else:
                before = target.read_bytes()
                old, new = b"fixture/Flag", b"fixture/Else"
                pattern = b"\x01" + len(old).to_bytes(2, "big") + old
                replacement = b"\x01" + len(new).to_bytes(2, "big") + new
                if before.count(pattern) != 1:
                    raise RuntimeError("Original constructor witness interface constant changed")
                target.write_bytes(before.replace(pattern, replacement, 1))
                receipt = {"before_sha256": hashlib.sha256(before).hexdigest(), "after_sha256": sha(target), "code": "unchanged"}
                for route in range(6):
                    text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (changed_originals, harness))), name, str(route), "false", "0", "true"], "late-check-" + str(route))
                    if text != "attempts=2,intersection-constructor-witness=matched\n":
                        raise RuntimeError("Original missing-witness cast or initialization order changed")
            if method_code(target.read_bytes()) != code_before:
                raise RuntimeError("Constructor metadata control changed Code")
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            counter_classpath = ["--classpath", sdk]
            if excluded:
                counter_provider = work / (label + "-provider.jar")
                write_provider(counter_provider, changed_originals)
                counter_classpath += ["--classpath", counter_provider]
            counter_dex = work / (label + "-dex")
            counter_dex.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", *counter_classpath, "--output", counter_dex, *[changed_originals / p.relative_to(originals) for p in paths]], label + "-d8")
            if changed_signature and not excluded:
                annotations = verify.run([verify.dexdump, "-a", "-f", "-h", "-l", "plain", counter_dex / "classes.dex"], label + "-annotations")
                # Only Product's one generic constructor has a Signature in this
                # owned input. Require its complete encoded value, not a prefix.
                values = re.findall(r'VISIBILITY_SYSTEM Ldalvik/annotation/Signature; value=\{ (.*?) \}', annotations)
                pattern = r'"(?:[^"\\]|\\.)*"'
                encoded = []
                for value in values:
                    pieces = re.findall(pattern, value)
                    if re.sub(pattern, "", value).strip():
                        raise RuntimeError("Unexpected DEX Signature encoding")
                    encoded.append("".join(json.loads(piece) for piece in pieces))
                if encoded != [changed_signature]:
                    raise RuntimeError("D8 normalized the intended constructor Signature control: " + label)
                (work / (label + "-dex-signature.json")).open("x").write(json.dumps({"expected": changed_signature, "encoded": encoded, "status": "exact"}) + "\n")
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(counter_dex / "classes.dex"), *map(str, counter_classpath), "-o", str(rejected)], text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (label + "-rejection.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Unproved intersection constructor was accepted: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(sdk) != sdk_hash
                or sha(dependency) != dependency_hash or sha(dex) != dex_hash or sha(args.neverd) != cli_hash):
            raise RuntimeError("Frozen original constructor program, observer, SDK or CLI changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "same_code_signature_controls": len(controls), "same_code_witness_control": "original late failure qualified and recovery rejected",
                   "metadata_roles_exceptions": "unchanged", "dependency_provider": args.dependency_provider, "minimum_java_release": release}
        (work / "evidence.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
