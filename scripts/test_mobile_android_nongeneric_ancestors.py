#!/usr/bin/env python3
"""Verify concrete or abstract generic parents with proven nongeneric ancestor contracts."""
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

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_static_defaults import method_code
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_static_defaults import method_code
    from test_mobile_android_protected_array_callbacks import change_method_flags


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--generic-interface", action="store_true")
    parser.add_argument("--abstract-parent", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-nongeneric-ancestors-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / ("mixed_generic_ancestors/java" if args.generic_interface else "nongeneric_ancestors/java")
        inputs = sorted(fixture.rglob("*.java"))
        if args.abstract_parent:
            abstract_fixture = work / "abstract-java"
            for source in inputs:
                content = source.read_text()
                edits = ()
                if source.name == "Parent.java":
                    edits = (("public class Parent<T>", "public abstract class Parent<T>"),
                             ("public T echo(T value) { return value; }", "public abstract T echo(T value);"))
                elif source.name == "Root.java":
                    edits = (("public class Root {", "public abstract class Root {\n    public abstract int view(int position);"),)
                for before, after in edits:
                    if content.count(before) != 1:
                        raise RuntimeError("Missing exact owned abstract ancestor declaration")
                    content = content.replace(before, after, 1)
                destination = abstract_fixture / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
            fixture = abstract_fixture
            inputs = sorted(fixture.rglob("*.java"))
        originals, harness_dir = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        classes, methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        harness_name = "GenericInterfaceAncestorHarness" if args.generic_interface else "NongenericAncestorHarness"
        harness_source = FIXTURES / ("harness/" + harness_name + ".java")
        if args.abstract_parent:
            harness_content = harness_source.read_text()
            marker = '        check(bridges == 1);'
            if harness_content.count(marker) != 1:
                raise RuntimeError("Missing owned ancestor harness reflection anchor")
            harness_content = harness_content.replace(marker, marker + """
        check(java.lang.reflect.Modifier.isAbstract(Parent.class.getModifiers())
                && java.lang.reflect.Modifier.isAbstract(Root.class.getModifiers()));
        check(java.lang.reflect.Modifier.isAbstract(Parent.class.getDeclaredMethod("echo", Object.class).getModifiers())
                && java.lang.reflect.Modifier.isAbstract(Root.class.getDeclaredMethod("view", int.class).getModifiers()));
        check(!java.lang.reflect.Modifier.isAbstract(Parent.class.getDeclaredMethod("view", int.class).getModifiers())
                && !java.lang.reflect.Modifier.isAbstract(Child.class.getDeclaredMethod("view", int.class).getModifiers()));
""", 1)
            harness_source = work / (harness_name + ".java")
            harness_source.write_text(harness_content)
        verify.compile_local([harness_source], harness_dir, classpath=originals)
        harness_hashes = {p.relative_to(harness_dir).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sorted(harness_dir.rglob("*.class"))}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               harness_name], "baseline")
        identity_count = 34 if args.generic_interface else 25
        if baseline.splitlines() != ["identities=" + str(identity_count) + ",failures=8,bridges=1,views=32",
                                     "generic-and-ordinary-ancestors=retained,mutable-super-state=matched,virtual-dispatch=matched"]:
            raise RuntimeError("Owned generic inheritance baseline did not exercise every expected case")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", args.android_jar.resolve())
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the independently authored generic inheritance inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["projected_method_count"] != 1
                or coverage["unrecovered_method_count"] != 0 or coverage.get("generic_signature_projections")
                or coverage["status"] != "partial"):
            raise RuntimeError("Owned generic inheritance lost an original descriptor or source formal")
        for row in coverage["methods"]:
            if row["status"] == "source-projected" and (
                    row["projection_kind"] != "generic-superclass-bridge"
                    or row["bridge_target"] not in methods or row["bridge_dispatch"] not in methods):
                raise RuntimeError("Bounded generic bridge lost its checked superclass delegation")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if actual_classes != classes or not set(methods).issubset(actual_methods):
            raise RuntimeError("javac did not regenerate every original generic member and bridge descriptor")
        if args.generic_interface or args.abstract_parent:
            helpers = coverage.get("generated_source_helpers", [])
            helper_ids = {row["class"] + "->" + row["name"] + row["prototype"] for row in helpers}
            if (set(actual_methods) - set(methods) != helper_ids or
                    any(row["kind"] != "throw-helper" or not row["static"] for row in helpers)):
                raise RuntimeError("Recompile added an unreported member outside the original inventory")
        if args.abstract_parent:
            actual_records = class_identity.compiler_classes(rebuilt)
            for owner in classes:
                wanted, after = records[owner], actual_records[owner]
                for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                            "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                    if after.get(key) != wanted.get(key):
                        raise RuntimeError("Original abstract ancestor metadata changed: " + owner + ":" + key)
                for identity, metadata in wanted["methods"].items():
                    if after["methods"][identity] != metadata:
                        raise RuntimeError("Original abstract ancestor method contract changed: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir))),
                             harness_name], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Owned source changed reflection, inherited dispatch or ordered effects")
        negative = work / "negative-originals"
        shutil.copytree(originals, negative)
        target = negative / "fixture/Child.class"
        before = target.read_bytes()
        signature = b"(TS;)TS;"
        changed_signature = b"(TU;)TS;"
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
        negative_dex = verify.dex(sorted(negative.rglob("*.class")), work / "negative-dex",
                                  args.android_jar.resolve())
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if (result.returncode == 0 or rejected.exists()
                or "generic target parameter differs from the full inherited substitution" not in result.stderr):
            raise RuntimeError("Erasure hid a target parameter from the wrong owned generic formal")
        negative_sources = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "Child.java":
                declaration = "public S echo(S value)"
                if content.count(declaration) != 1:
                    raise RuntimeError("Missing owned source override declaration")
                content = content.replace(declaration, "@Override\n    " + declaration.replace("S value", "U value"), 1)
            destination = negative_sources / source.relative_to(fixture)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or "does not override" not in compiler.stderr:
            raise RuntimeError("Independent javac did not reject the different owned formal")
        if args.abstract_parent:
            private_provider = work / "private-provider-originals"
            shutil.copytree(originals, private_provider)
            receipt = change_method_flags(private_provider / "fixture/Child.class",
                b"(I)I", 0x1, 0x2, name=b"view")
            (work / "private-provider-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
            private_dex = verify.dex(sorted(private_provider.rglob("*.class")),
                                     work / "private-provider-dex", args.android_jar.resolve())
            private_output = work / "private-provider-rejected"
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(private_dex), "--classpath",
                                     str(args.android_jar.resolve()), "-o", str(private_output)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "private-provider-recovery.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or private_output.exists() or "generic inheritance" not in result.stderr:
                raise RuntimeError("Abstract ancestor proof bypassed a nearer private method")
        if harness_hashes != {p.relative_to(harness_dir).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in sorted(harness_dir.rglob("*.class"))}:
            raise RuntimeError("Immutable independent harness bytes changed")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted(originals.rglob("*.class"))}:
            raise RuntimeError("Original independently authored class bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "bridge_count": 1, "identity_cases": identity_count, "ordered_failure_cases": 8,
                    "parameterized_interface_with_plain_superclass": args.generic_interface,
                    "abstract_parent_and_ancestor": args.abstract_parent,
                    "abstract_provider_counterexample": "rejected with original Code" if args.abstract_parent else "not applicable",
                    "complete_abstract_metadata": "unchanged except reported helpers" if args.abstract_parent else "not applicable",
                    "nongeneric_ancestors_and_owned_bounds": "retained", "generic_reflection": "unchanged",
                    "mutable_super_state_and_virtual_dispatch": "matched",
                    "original_descriptors": "retained", "scope_counterexample": "rejected-by-neverd-and-javac",
                    "original_class_sha256": hashes, "coverage": "partial", "jvm_output": "matched"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
