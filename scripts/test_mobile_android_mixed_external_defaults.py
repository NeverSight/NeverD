#!/usr/bin/env python3
"""Verify source interface edges retain an owned external default body."""
from __future__ import annotations

import argparse
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--class-ancestor", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-mixed-default-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "mixed_external_defaults/java"
        inputs = sorted(fixture.rglob("*.java"))
        if args.class_ancestor:
            inputs += sorted((FIXTURES / "mixed_class_default_roots/java").rglob("*.java"))
            receiver = next(p for p in inputs if p.name == "Receiver.java")
            replacement = work / "owned-java/fixture/Receiver.java"
            replacement.parent.mkdir(parents=True)
            replacement.write_text(receiver.read_text().replace("implements Leaf<S,V>", "extends Parent<V,S> implements Leaf<S,V>"))
            inputs = [replacement if p == receiver else p for p in inputs]
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        external = {"Lfixture/Root;", "Lfixture/Trace;"}
        classes = all_classes - external
        methods = {key: value for key, value in all_methods.items() if key.split("->", 1)[0] in classes}
        dependency = work / "owned-contract.jar"
        with zipfile.ZipFile(dependency, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(originals.rglob("*.class")):
                relative = path.relative_to(originals).as_posix()
                if "L" + relative[:-6] + ";" in external:
                    archive.writestr(relative, path.read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in originals.rglob("*.class")}
        verify.compile_local([FIXTURES / "harness/MixedExternalDefaultHarness.java"], harness, classpath=originals)
        harness_hashes = {p.relative_to(harness).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in harness.rglob("*.class")}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "MixedExternalDefaultHarness", "ancestor" if args.class_ancestor else "base"], "baseline")
        if baseline.splitlines() != ["identities=44,failures=4",
                "external-default-owner=retained,full-source-edges=retained,dispatch-and-effects=matched"]:
            raise RuntimeError("Owned baseline missed original default dispatch or identity: " + baseline)

        def owned_dex(directory: Path, output: Path) -> Path:
            output.mkdir()
            paths = [p for p in sorted(directory.rglob("*.class"))
                     if "L" + p.relative_to(directory).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--classpath", dependency, "--output", output, *paths], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned mixed interface fixture did not produce exactly one DEX")
            return dex

        dex = owned_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes or
                {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the independently authored original inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "--classpath", dependency, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] != 0 or coverage.get("projected_method_count", 0) != 0 or
                coverage["status"] != "recovered" or coverage.get("generic_signature_projections")):
            raise RuntimeError("Mixed source edges lost an original descriptor or full generic contract")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = coverage.get("generated_source_helpers", [])
        helper_ids = {r["class"] + "->" + r["name"] + r["prototype"] for r in helpers}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(methods) | helper_ids:
            raise RuntimeError("javac added an unreported slot or changed the original inventory")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "MixedExternalDefaultHarness", "ancestor" if args.class_ancestor else "base"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Mixed interface edges changed default owner, dispatch, identity or effects")
        negative = work / "negative"
        shutil.copytree(originals, negative)
        target = negative / ("fixture/Parent.class" if args.class_ancestor else "fixture/Leaf.class")
        before = target.read_bytes()
        if args.class_ancestor:
            receipt = change_signature(target,
                b"<L:Ljava/lang/Object;R:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Child<TR;>;",
                b"<L:Ljava/lang/Object;R:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Child<TL;>;")
        else:
            receipt = change_signature(target,
                b"<P:Ljava/lang/Object;Q:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Middle<TQ;TP;>;Lfixture/Child<TP;>;",
                b"<P:Ljava/lang/Object;Q:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Middle<TQ;TP;>;Lfixture/Child<TQ;>;")
        if method_code(before) != method_code(target.read_bytes()):
            raise RuntimeError("Mixed edge counterexample changed original Code or descriptors")
        (work / "negative-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = owned_dex(negative, work / "negative-dex")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                 "--classpath", str(args.android_jar.resolve()), "--classpath", str(dependency),
                                 "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic inheritance" not in result.stderr:
            raise RuntimeError("Equal runtime erasure hid different source edge argument scopes")
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if args.class_ancestor and source.name == "Parent.java":
                content = content.replace("Child<R>", "Child<L>")
            elif not args.class_ancestor and source.name == "Leaf.java":
                content = content.replace("Child<P>", "Child<Q>")
            destination = work / "negative-java" / "fixture" / source.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Independent javac accepted incompatible source interface arguments")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in originals.rglob("*.class")} or harness_hashes != {
                      p.relative_to(harness).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in harness.rglob("*.class")} or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash:
            raise RuntimeError("Owned original Code, concrete harness or external dependency changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "identity_cases": 44, "ordered_failure_cases": 4,
                    "original_inventory_and_external_default_code": "unchanged",
                    "full_source_edge_reflection": "matched", "default_declaring_owner": "unchanged",
                    "virtual_dispatch_and_effect_order": "matched", "coverage": "recovered",
                    "transitive_class_interface_arguments": args.class_ancestor,
                    "source_edge_scope_counterexample": "rejected-by-neverd-and-javac"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
