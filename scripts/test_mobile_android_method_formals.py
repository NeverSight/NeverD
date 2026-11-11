#!/usr/bin/env python3
"""Verify alpha-equivalent method formals, arrays and distinct class scopes against an unchanged interface JAR."""
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--nominal-bounds", action="store_true")
    parser.add_argument("--interface-bounds", action="store_true")
    parser.add_argument("--recursive-interface-bounds", action="store_true")
    parser.add_argument("--recursive-class-bounds", action="store_true")
    args = parser.parse_args()
    args.interface_bounds |= args.recursive_interface_bounds
    if args.recursive_class_bounds and args.interface_bounds:
        parser.error("recursive class and interface profiles are distinct")
    args.nominal_bounds |= args.interface_bounds or args.recursive_class_bounds
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-method-formal-inheritance-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / ("recursive_class_method_formals/java" if args.recursive_class_bounds else
                             "recursive_interface_method_formals/java" if args.recursive_interface_bounds else
                             "nominal_interface_method_formals/java" if args.interface_bounds else
                             "nominal_method_formal_inheritance/java" if args.nominal_bounds
                              else "method_formal_inheritance/java")
        inputs = sorted(fixture.rglob("*.java"))
        if args.nominal_bounds:
            inputs.append(FIXTURES / "value_carrier_bridges/java/fixture/Trace.java")
        originals, harness_dir = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        original_records = class_identity.compiler_classes(originals)
        classes = all_classes - {"Lfixture/Contract;"}
        methods = {identity: row for identity, row in all_methods.items()
                   if identity.split("->", 1)[0] in classes}
        dependency = work / "owned-contract.jar"
        with zipfile.ZipFile(dependency, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.write(originals / "fixture/Contract.class", "fixture/Contract.class")
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        harness = ("RecursiveClassMethodFormalHarness" if args.recursive_class_bounds else
                   "RecursiveInterfaceMethodFormalHarness" if args.recursive_interface_bounds else
                   "InterfaceMethodFormalHarness" if args.interface_bounds else
                   "NominalMethodFormalHarness" if args.nominal_bounds else "MethodFormalInheritanceHarness")
        verify.compile_local([FIXTURES / ("harness/" + harness + ".java")], harness_dir,
                             classpath=originals)
        harness_hashes = {p.relative_to(harness_dir).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sorted(harness_dir.rglob("*.class"))}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               harness], "baseline")
        expected = (["identities=53,failures=4,bridges=0",
                     "nominal-bounds-and-full-scopes=retained,raw-identity-and-virtual-dispatch=matched"]
                    if args.nominal_bounds else ["identities=35,failures=7,bridges=0",
                     "method-formal-alpha=retained,class-and-method-scopes=distinct,array-identity-and-effects=matched"])
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned generic inheritance baseline did not exercise every expected case")
        def owned_dex(directory: Path, output: Path) -> Path:
            output.mkdir(parents=True)
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath",
                        args.android_jar.resolve(), "--classpath", dependency, "--output", output,
                        *[path for path in sorted(directory.rglob("*.class"))
                          if "L" + path.relative_to(directory).as_posix()[:-6] + ";" in classes]], "d8-" + output.name)
            dex = output / "classes.dex"
            if not dex.is_file() or list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned method-formal fixture did not produce exactly one DEX")
            return dex

        dex = owned_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the independently authored generic inheritance inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "--classpath", dependency, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage.get("projected_method_count", 0) != 0
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"] != 0 or coverage.get("generic_signature_projections")
                or coverage["status"] != "recovered"):
            raise RuntimeError("Owned generic inheritance lost an original descriptor or source formal")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("javac did not regenerate every original generic member and bridge descriptor")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            before, after = original_records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original class/field generic metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original method formal scope or bounds changed: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir, dependency))),
                             harness], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Owned source changed reflection, inherited dispatch or ordered effects")
        negative = work / "negative-originals"
        shutil.copytree(originals, negative)
        target = negative / "fixture/View.class"
        before = target.read_bytes()
        signature = b"<T:Ljava/lang/Object;U:Ljava/lang/Object;>([TT;TU;)[TT;"
        changed_signature = b"<T:Ljava/lang/Object;U:Ljava/lang/Object;>([TU;TU;)[TT;"
        if args.nominal_bounds:
            signature = (b"<R::Lfixture/Bound;>(Lfixture/Pair<TT;TR;>;)Lfixture/Pair<TR;TR;>;"
                         if args.interface_bounds else
                         b"<R:Lfixture/Bound;>(Lfixture/Pair<TT;TR;>;)Lfixture/Pair<TR;TR;>;")
            changed_signature = signature.replace(b":Lfixture/Bound;", b":Lfixture/OtherBound;")
            if args.recursive_interface_bounds:
                signature = b"<R::Lfixture/Bound<-TR;>;>(Lfixture/Pair<TT;TR;>;)Lfixture/Pair<TR;TR;>;"
                changed_signature = signature.replace(b"Bound<-TR;>", b"Bound<TR;>")
            if args.recursive_class_bounds:
                signature = b"<R:Lfixture/Bound<TR;>;>(Lfixture/Pair<TT;TR;>;)Lfixture/Pair<TR;TR;>;"
                changed_signature = signature.replace(b"Bound<TR;>", b"Bound<+TR;>")
        old = b"\x01" + len(signature).to_bytes(2, "big") + signature
        new = b"\x01" + len(changed_signature).to_bytes(2, "big") + changed_signature
        if before.count(old) != 1:
            raise RuntimeError("Missing exact owned generic target Signature")
        changed = before.replace(old, new, 1)
        if method_code(before) != method_code(changed):
            raise RuntimeError("Generic scope counterexample changed Code bytes or method descriptors")
        target.write_bytes(changed)
        (work / "negative-signature.json").write_text(json.dumps(
            {"original_signature": signature.decode(), "changed_signature": changed_signature.decode(),
             "code_and_descriptors": "unchanged", "changed_sha256": hashlib.sha256(changed).hexdigest()},
            indent=2) + "\n")
        negative_dex = owned_dex(negative, work / "negative-dex")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "--classpath", str(dependency), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        expected_error = ("generic target method bound differs" if args.nominal_bounds else
                          "generic target parameter differs from the full inherited substitution")
        if (result.returncode == 0 or rejected.exists() or expected_error not in result.stderr):
            raise RuntimeError("Erasure hid reordered owned method formal arguments")
        negative_sources = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "View.java":
                declaration = ("public <R extends Bound> Pair<R, R> make(Pair<T, R> input)"
                               if args.nominal_bounds else "public <T, U> T[] copy(T[] input, U tag)")
                if args.recursive_interface_bounds:
                    declaration = "public <R extends Bound<? super R>> Pair<R, R> make(Pair<T, R> input)"
                if args.recursive_class_bounds:
                    declaration = "public <R extends Bound<R>> Pair<R, R> make(Pair<T, R> input)"
                if content.count(declaration) != 1:
                    raise RuntimeError("Missing owned source override declaration")
                changed_declaration = (declaration.replace("extends Bound", "extends OtherBound")
                                       if args.nominal_bounds else declaration.replace("copy(T[] input", "copy(U[] input"))
                if args.recursive_interface_bounds:
                    changed_declaration = declaration.replace("Bound<? super R>", "Bound<R>")
                if args.recursive_class_bounds:
                    changed_declaration = declaration.replace("Bound<R>", "Bound<? extends R>")
                content = content.replace(declaration, "@Override\n    " + changed_declaration, 1)
            relative = source.relative_to(fixture) if source.is_relative_to(fixture) else Path("fixture/Trace.java")
            destination = negative_sources / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or "does not override" not in compiler.stderr:
            raise RuntimeError("Independent javac did not reject the different owned formal")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted(originals.rglob("*.class"))}:
            raise RuntimeError("Original independently authored class bytes changed")
        if hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash:
            raise RuntimeError("Owned external method declarations changed")
        if harness_hashes != {p.relative_to(harness_dir).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in sorted(harness_dir.rglob("*.class"))}:
            raise RuntimeError("Original immutable JVM harness bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "bridge_count": 0, "identity_cases": 35, "ordered_failure_cases": 7,
                    "method_formal_alpha_equivalence": "retained", "generic_reflection": "unchanged",
                    "unchanged_dependency_and_mutable_state": "matched",
                    "original_descriptors": "retained", "scope_counterexample": "rejected-by-neverd-and-javac",
                    "original_class_sha256": hashes, "coverage": "recovered", "jvm_output": "matched"}
        if args.nominal_bounds:
            evidence["identity_cases"] = 53
            evidence["ordered_failure_cases"] = 4
            evidence["nominal_method_bounds"] = ("same complete known interface bound" if args.interface_bounds
                                                  else "same complete known class bound")
            evidence["raw_reference_identity"] = "no added cast or constraint"
            evidence["bound_counterexample"] = "unchanged Code and ABI, rejected by neverd and javac"
            if args.recursive_interface_bounds:
                evidence["nominal_method_bounds"] = "same complete recursive interface bound and wildcard variance"
            if args.recursive_class_bounds:
                evidence["nominal_method_bounds"] = "same complete recursive class bound and invariant self scope"
        evidence["class_field_and_method_generic_metadata"] = "unchanged"
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
