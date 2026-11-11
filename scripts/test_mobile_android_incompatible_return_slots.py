#!/usr/bin/env python3
"""Verify disclosed source names for independently authored JVM return slots."""
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
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def owned_return_names(path: Path) -> dict:
    """Change only three owned UTF8 names, preserving every instruction byte."""
    before = path.read_bytes()
    after = before
    names = {}
    for old, new in ((b"persistentRemove", b"remove"), (b"persistentPut", b"put"), (b"persistentClear", b"clear")):
        encoded = b"\x01" + len(old).to_bytes(2, "big") + old
        if encoded not in after:
            continue
        if after.count(encoded) != 1:
            raise RuntimeError("Ambiguous independently authored method name")
        after = after.replace(encoded, b"\x01" + len(new).to_bytes(2, "big") + new, 1)
        names[old] = new
    expected = {}
    for identity, code in method_code(before).items():
        name, prototype = identity.split(b"(", 1)
        expected[names.get(name, name) + b"(" + prototype] = code
    if expected != method_code(after) or before[class_records(before)["pool_end"]:] != after[class_records(after)["pool_end"]:]:
        raise RuntimeError("Owned name edit changed Code, descriptors or attributes")
    path.write_bytes(after)
    return {"name_changes": {a.decode(): b.decode() for a, b in names.items()},
            "code_descriptors_and_attributes": "unchanged"}


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-return-slots-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "incompatible_return_slots/java"
        inputs = sorted(fixture.rglob("*.java"))
        if args.class_ancestor:
            inputs += sorted((FIXTURES / "inherited_return_slots/java").rglob("*.java"))
            source = next(p for p in inputs if p.name == "Actor.java")
            replacement = work / "owned-java/fixture/Actor.java"
            replacement.parent.mkdir(parents=True)
            replacement.write_text(source.read_text().replace("implements Leaf<A>", "extends ReturnParent<A> implements Leaf<A>"))
            inputs = [replacement if p == source else p for p in inputs]
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        harness_source = FIXTURES / "harness/IncompatibleReturnSlotHarness.java"
        if args.class_ancestor:
            replacement = work / "owned-harness" / harness_source.name
            replacement.parent.mkdir()
            content = harness_source.read_text().replace("== RootBase.class ||", "== ReturnParent.class ||")
            content = content.replace('        check(RootBase.class.getDeclaredMethod',
                '        check(Actor.class.getGenericSuperclass().getTypeName().equals("fixture.ReturnParent<A>"));\n'
                '        check(ReturnParent.class.getGenericInterfaces()[0].getTypeName().equals("fixture.RootBase<T>"));\n'
                '        check(RootBase.class.getDeclaredMethod')
            replacement.write_text(content)
            harness_source = replacement
        verify.compile_local([harness_source], harness, classpath=originals)
        receipts = {path.relative_to(originals).as_posix(): owned_return_names(path)
                    for path in sorted(originals.rglob("*.class")) if path.stem not in ("RootBase", "Trace", "ReturnParent")}
        (work / "owned-input-name-edits.json").write_text(json.dumps(receipts, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        external = {"Lfixture/RootBase;", "Lfixture/Trace;"}
        if args.class_ancestor:
            external.add("Lfixture/ReturnParent;")
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
        harness_hashes = {p.relative_to(harness).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in harness.rglob("*.class")}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "IncompatibleReturnSlotHarness", "remove", "put", "clear"], "baseline")
        if baseline.splitlines() != ["identities=66,failures=6",
                "original-return-dispatch=matched,external-defaults=retained,source-name-abi-difference=reported"]:
            raise RuntimeError("Owned baseline missed distinct return dispatch or identity: " + baseline)

        def owned_dex(directory: Path, output: Path) -> Path:
            output.mkdir()
            paths = [p for p in sorted(directory.rglob("*.class"))
                     if "L" + p.relative_to(directory).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--classpath", dependency, "--output", output, *paths], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned return slots did not produce exactly one DEX")
            return dex

        dex = owned_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original return slot")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "--classpath", dependency, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        named = {r["identity"]: r for r in coverage["methods"]
                 if r.get("projection_kind") == "incompatible-inherited-return-slot"}
        if ({r["identity"] for r in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] != 0 or coverage["projected_method_count"] != 9 or
                coverage["status"] != "partial" or coverage.get("generic_signature_projections") or len(named) != 9):
            raise RuntimeError("Return slot projection lost original inventory or complete Signatures")
        names = []
        for original, prototype in (("remove", "(Ljava/lang/Object;Ljava/lang/Object;)"),
                                    ("put", "(Ljava/lang/Object;)"), ("clear", "()")):
            row = named["Lfixture/Persistent;->" + original + prototype + "Lfixture/Persistent;"]
            spelling = row["source_name"]
            if spelling == "__neverdReturnSlot" or not row["metadata_differences"] or not row["conflicting_inherited_contracts"]:
                raise RuntimeError("Return slot name collision or ABI difference was not disclosed")
            for owner in ("Actor", "Derived"):
                implementation = named["Lfixture/" + owner + ";->" + original + prototype + "Lfixture/Persistent;"]
                if implementation["source_name"] != spelling:
                    raise RuntimeError("Virtual implementations selected inconsistent source names")
            names.append(spelling)
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = coverage.get("generated_source_helpers", [])
        helper_ids = {r["class"] + "->" + r["name"] + r["prototype"] for r in helpers}
        source_methods = {named[key]["source_identity"] if key in named else key for key in methods}
        if rebuilt_classes != classes or set(rebuilt_methods) != source_methods | helper_ids:
            raise RuntimeError("javac changed an undisclosed return slot or original descriptor")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "IncompatibleReturnSlotHarness", *names], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Source name mapping changed default dispatch, identity, state or ordered effects")
        negative = work / "negative"
        shutil.copytree(originals, negative)
        target = negative / "fixture/Actor.class"
        before = target.read_bytes()
        receipt = change_signature(target, b"(TA;)Lfixture/Persistent<TA;>;",
                                   b"<A:Ljava/lang/Object;>(TA;)Lfixture/Persistent<TA;>;")
        if method_code(before) != method_code(target.read_bytes()):
            raise RuntimeError("Return slot counterexample changed original Code or descriptors")
        (work / "negative-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = owned_dex(negative, work / "negative-dex")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                 "--classpath", str(args.android_jar.resolve()), "--classpath", str(dependency),
                                 "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic inheritance" not in result.stderr:
            raise RuntimeError("Source name hid a different original formal scope")
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "Actor.java":
                content = content.replace("public Persistent<A> persistentPut(A value)",
                                          "public <A> Persistent<A> persistentPut(A value)")
            destination = work / "negative-java" / "fixture" / source.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Independent javac accepted a different implementation formal scope")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in originals.rglob("*.class")} or harness_hashes != {
                      p.relative_to(harness).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in harness.rglob("*.class")} or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash:
            raise RuntimeError("Owned original Code, harness or external default dependency changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "class_ancestor": args.class_ancestor,
                    "identity_cases": 66, "ordered_failure_cases": 6, "source_named_methods": 9,
                    "original_code_and_external_defaults": "unchanged", "original_inventory": "retained in report",
                    "source_name_binary_abi_and_reflection_differences": "explicitly reported",
                    "virtual_dispatch_and_effect_order": "matched", "coverage": "partial",
                    "source_formal_scope_counterexample": "rejected-by-neverd-and-javac"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
