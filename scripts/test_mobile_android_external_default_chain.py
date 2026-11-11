#!/usr/bin/env python3
"""Verify inherited external defaults, nearest interface declarations and unchanged dependency effects."""
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
    from scripts.test_mobile_android_static_defaults import method_code
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_static_defaults import method_code
    from test_mobile_android_nested_return import change_signature


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--class-ancestor", action="store_true")
    parser.add_argument("--class-interface-branches", action="store_true")
    parser.add_argument("--plain-default-override", action="store_true")
    args = parser.parse_args()
    args.class_ancestor |= args.class_interface_branches or args.plain_default_override
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-external-default-chain-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "external_default_chain/java"
        inputs = sorted(fixture.rglob("*.java"))
        if args.class_ancestor:
            inputs += sorted((FIXTURES / "class_external_defaults/java").rglob("*.java"))
            projected = []
            for source in inputs:
                if source.name == "View.java":
                    replacement = work / "owned-java/fixture/View.java"
                    replacement.parent.mkdir(parents=True)
                    content = source.read_text().replace("implements Leaf<T>, Root<T>",
                                                        "extends DefaultHolder<T,String> implements Leaf<T>, Root<T>")
                    if args.plain_default_override:
                        content = content.replace("    public View() {}",
                            "    public View() {}\n"
                            "    public int fallback(int input) { return super.fallback(input); }")
                    replacement.write_text(content)
                    source = replacement
                projected.append(source)
            inputs = projected
        if args.class_interface_branches:
            inputs += sorted((FIXTURES / "class_interface_default_branches/java").rglob("*.java"))
            projected = []
            for source in inputs:
                if source.name in ("View.java", "DefaultHolder.java"):
                    replacement = work / "branch-java/fixture" / source.name
                    replacement.parent.mkdir(parents=True, exist_ok=True)
                    content = source.read_text()
                    content = content.replace("implements Leaf<T>, Root<T>", "implements ConsumerBranch<T>, Root<T>")
                    content = content.replace("implements Leaf<A>", "implements AncestorBranch<A>")
                    replacement.write_text(content)
                    source = replacement
                projected.append(source)
            inputs = projected
        originals, harness_dir = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        declarations = {"Lfixture/Root;", "Lfixture/Middle;", "Lfixture/Leaf;", "Lfixture/Marker;", "Lfixture/Trace;"}
        if args.class_ancestor:
            declarations.add("Lfixture/DefaultParent;")
        classes = all_classes - declarations
        methods = {identity: row for identity, row in all_methods.items()
                   if identity.split("->", 1)[0] in classes}
        dependency = work / "owned-contract.jar"
        with zipfile.ZipFile(dependency, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(originals.rglob("*.class")):
                relative = path.relative_to(originals).as_posix()
                if "L" + relative[:-6] + ";" in declarations:
                    archive.writestr(relative, path.read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        harness_source = FIXTURES / "harness/ExternalDefaultChainHarness.java"
        if args.class_ancestor:
            replacement = work / "owned-harness" / harness_source.name
            replacement.parent.mkdir()
            content = harness_source.read_text().replace("        int externalBridges = 0;",
                '        check(View.class.getGenericSuperclass().getTypeName().equals("fixture.DefaultHolder<T, java.lang.String>"));\n'
                '        check(DefaultHolder.class.getGenericSuperclass().getTypeName().equals("fixture.DefaultParent<A, B>"));\n'
                '        check(DefaultHolder.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Leaf<A>"));\n'
                '        check(DefaultParent.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Leaf<X>"));\n'
                '        check(DefaultParent.class.getDeclaredMethod("value",Object.class).getGenericReturnType().getTypeName().equals("X"));\n'
                '        for (Method method : DefaultHolder.class.getDeclaredMethods()) check(!method.getName().equals("reverse"));\n'
                '        int externalBridges = 0;')
            replacement.write_text(content)
            harness_source = replacement
        if args.plain_default_override:
            replacement = work / "override-harness" / harness_source.name
            replacement.parent.mkdir()
            content = harness_source.read_text().replace(
                'View.class.getMethod("fallback", int.class).getDeclaringClass() == Middle.class',
                'View.class.getMethod("fallback", int.class).getDeclaringClass() == View.class')
            content = content.replace("check(publicMethods == 2)", "check(publicMethods == 3)")
            replacement.write_text(content)
            harness_source = replacement
        if args.class_interface_branches:
            replacement = work / "branch-harness" / harness_source.name
            replacement.parent.mkdir()
            content = harness_source.read_text().replace('"fixture.Leaf<A>"', '"fixture.AncestorBranch<A>"')
            content = content.replace('"fixture.Leaf<T>"', '"fixture.ConsumerBranch<T>"')
            content = content.replace("direct.getRawType() == Leaf.class", "direct.getRawType() == ConsumerBranch.class")
            content = content.replace('        int externalBridges = 0;',
                '        check(AncestorBranch.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Leaf<A>"));\n'
                '        check(ConsumerBranch.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Leaf<B>"));\n'
                '        check(View.class.getGenericInterfaces()[0].getTypeName().equals("fixture.ConsumerBranch<T>"));\n'
                '        int externalBridges = 0;')
            replacement.write_text(content)
            harness_source = replacement
        verify.compile_local([harness_source], harness_dir,
                             classpath=originals)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               "ExternalDefaultChainHarness"], "baseline")
        if baseline.splitlines() != ["identities=43,failures=16,bridges=0",
                                     "external-default-chain=retained,nearest-default-owner=matched,dependency-effects=matched"]:
            raise RuntimeError("Owned generic inheritance baseline did not exercise every expected case")
        def owned_dex(directory: Path, output: Path) -> Path:
            output.mkdir(parents=True)
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath",
                        args.android_jar.resolve(), "--classpath", dependency, "--output", output,
                        *[path for path in sorted(directory.rglob("*.class"))
                          if "L" + path.relative_to(directory).as_posix()[:-6] + ";" in classes]], "d8-" + output.name)
            dex = output / "classes.dex"
            if not dex.is_file() or list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned default-chain fixture did not produce exactly one DEX")
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
        if actual_classes != classes or not set(methods).issubset(actual_methods):
            raise RuntimeError("javac did not regenerate every original generic member and bridge descriptor")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir, dependency))),
                             "ExternalDefaultChainHarness"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Owned source changed reflection, inherited dispatch or ordered effects")
        if args.class_interface_branches:
            negative = work / "negative-class-branch"
            shutil.copytree(originals, negative)
            target = negative / "fixture/DefaultHolder.class"
            before = target.read_bytes()
            receipt = change_signature(target,
                b"<A:Ljava/lang/Object;B:Ljava/lang/Object;>Lfixture/DefaultParent<TA;TB;>;Lfixture/AncestorBranch<TA;>;",
                b"<A:Ljava/lang/Object;B:Ljava/lang/Object;>Lfixture/DefaultParent<TA;TB;>;Lfixture/AncestorBranch<TB;>;")
            if method_code(before) != method_code(target.read_bytes()):
                raise RuntimeError("Class-interface branch mutation changed original Code")
            (work / "negative-class-branch-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = owned_dex(negative, work / "negative-class-branch-dex")
            rejected = work / "rejected-class-branch"
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                    str(args.android_jar.resolve()), "--classpath", str(dependency),
                                    "-o", str(rejected)], text=True, capture_output=True, timeout=300,
                                    env=verify.environment)
            (work / "negative-class-branch-recovery.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Class-interface branches ignored complete argument drift")
        negative_api = work / "negative-api"
        shutil.copytree(originals, negative_api)
        receipt = change_signature(negative_api / "fixture/Middle.class",
                                   b"()Lfixture/Middle<TM;>;",
                                   b"()Lfixture/Middle<Ljava/lang/String;>;")
        (work / "negative-covariant-api-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dependency = work / "negative-owned-contract.jar"
        with zipfile.ZipFile(negative_dependency, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(negative_api.rglob("*.class")):
                relative = path.relative_to(negative_api).as_posix()
                if "L" + relative[:-6] + ";" in declarations:
                    archive.writestr(relative, path.read_bytes())
        rejected_api = work / "rejected-api"
        api_result = subprocess.run([str(args.neverd.resolve()), "mobile", str(dex), "--classpath",
                                     str(args.android_jar.resolve()), "--classpath", str(negative_dependency),
                                     "-o", str(rejected_api)], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
        (work / "negative-covariant-api-recovery.log").write_text(api_result.stdout + api_result.stderr)
        if api_result.returncode == 0 or rejected_api.exists() or "generic inheritance" not in api_result.stderr:
            raise RuntimeError("Covariant dependency default lost complete generic argument checking")
        negative = work / "negative-originals"
        shutil.copytree(originals, negative)
        target = negative / "fixture/View.class"
        before = target.read_bytes()
        signature = b"(TT;)TT;"
        changed_signature = b"<T:Ljava/lang/Object;>(TT;)TT;"
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
        if (result.returncode == 0 or rejected.exists()
                or "generic inheritance" not in result.stderr):
            raise RuntimeError("Erasure hid reordered owned method formal arguments")
        negative_sources = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "View.java":
                declaration = "public T value(T next)"
                if content.count(declaration) != 1:
                    raise RuntimeError("Missing owned source override declaration")
                content = content.replace(declaration, "@Override\n    " + declaration.replace("public T value", "public <T> T value"), 1)
            destination = negative_sources / "fixture" / source.name
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
            raise RuntimeError("Owned external default declarations changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "generic_class_inherited_external_defaults": args.class_ancestor,
                    "complete_class_interface_branches": args.class_interface_branches,
                    "complete_plain_default_override": args.plain_default_override,
                    "bridge_count": 0, "identity_cases": 43, "ordered_failure_cases": 16,
                    "external_covariant_default_and_existing_bridge": "complete generic proof, unchanged dependency body",
                    "abstract_ancestor_contract": "implemented by most specific external default",
                    "same_erasure_covariant_api_counterexample": "rejected with unchanged Code, descriptors and input DEX",
                    "redundant_source_parent_interfaces": "complete scoped arguments matched",
                    "external_interface_diamond": "unique most specific declaration retained",
                    "inherited_generic_default_identity_and_effects": "matched",
                    "external_default_chain": "retained", "nearest_default_owner": "matched", "generic_reflection": "unchanged",
                    "unchanged_dependency_and_mutable_state": "matched",
                    "original_descriptors": "retained", "scope_counterexample": "rejected-by-neverd-and-javac",
                    "original_class_sha256": hashes, "coverage": "recovered", "jvm_output": "matched"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
