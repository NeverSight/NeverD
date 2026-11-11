#!/usr/bin/env python3
"""Verify independently authored deferred abstract interface requirements."""
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
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code

SIGNATURE = (b"<S:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;"
             b"Lfixture/Contract<TS;>;Lfixture/Mirror<TS;>;")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--source-interfaces", action="store_true")
    parser.add_argument("--fixed-throws", action="store_true")
    parser.add_argument("--class-ancestor", action="store_true")
    parser.add_argument("--dependency-ancestor", action="store_true")
    args = parser.parse_args()
    args.class_ancestor |= args.dependency_ancestor
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-deferred-abstract-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "deferred_abstract_interfaces/java"
        inputs = sorted(fixture.rglob("*.java"))
        signature = SIGNATURE
        if args.class_ancestor:
            inputs += sorted((FIXTURES / "deferred_class_ancestors/java").rglob("*.java"))
            source = next(p for p in inputs if p.name == "Holder.java")
            replacement = work / "owned-java/fixture/Holder.java"
            replacement.parent.mkdir(parents=True)
            content = source.read_text().replace("implements Contract<S>", "extends Parent<V,S> implements Contract<S>")
            content = content.replace("    protected S state;", "    public int current(String ignored) { return 19; }\n    protected S state;")
            replacement.write_text(content)
            inputs = [replacement if p == source else p for p in inputs]
            signature = SIGNATURE.replace(b">Ljava/lang/Object;Lfixture/Contract", b">Lfixture/Parent<TV;TS;>;Lfixture/Contract")
        if args.fixed_throws:
            changed_inputs = []
            for source in inputs:
                content = source.read_text()
                replacements = {
                    "Contract.java": (("void insert(int index, E value);", "void insert(int index, E value) throws Exception;"),
                                      ("E current();", "E current() throws Exception;"),
                                      ("E tail();", "E tail() throws Exception;")),
                    "Mirror.java": (("P current();", "P current() throws Exception;"),
                                    ("P tail();", "P tail() throws Exception;")),
                    "Receiver.java": (("public RuntimeException failure;", "public RuntimeException failure;\n    public Exception checkedFailure;"),
                                      ("void insert(int index, S value) {", "void insert(int index, S value) throws Exception {"),
                                      ("        state = value;", "        if (checkedFailure != null) throw checkedFailure;\n        state = value;"),
                                      ("S current() {", "S current() throws Exception {"),
                                      ("S tail() {", "S tail() throws Exception {")),
                    "Holder.java": (("    protected S state;", "    public V[] params;\n    protected S state;"),),
                    "Parent.java": (("abstract R current();", "abstract R current() throws Exception;"),),
                }.get(source.name, ())
                for before, after in replacements:
                    if content.count(before) != 1:
                        raise RuntimeError("Missing exact owned checked-exception declaration")
                    content = content.replace(before, after, 1)
                destination = work / "fixed-throws-java/fixture" / source.name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                changed_inputs.append(destination)
            inputs = changed_inputs
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        records = class_identity.compiler_classes(originals)
        all_classes, methods = compiler_inventory(verify, originals)
        declarations = set() if args.source_interfaces else {"Lfixture/Contract;", "Lfixture/Mirror;"}
        if args.dependency_ancestor:
            declarations.add("Lfixture/Parent;")
        classes = all_classes - declarations
        identities = {key for key in methods if key.split("->", 1)[0] in classes}
        dependency = work / "owned-contracts.jar"
        with zipfile.ZipFile(dependency, "x", zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(originals.rglob("*.class")):
                relative = path.relative_to(originals).as_posix()
                if "L" + relative[:-6] + ";" in declarations:
                    archive.writestr(relative, path.read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in originals.rglob("*.class")}
        harness_source = FIXTURES / "harness/DeferredAbstractInterfaceHarness.java"
        if args.class_ancestor:
            replacement = work / "owned-harness" / harness_source.name
            replacement.parent.mkdir()
            content = harness_source.read_text().replace('        check(Modifier.isAbstract(Holder.class.getModifiers()));',
                '        check(Holder.class.getGenericSuperclass().getTypeName().equals("fixture.Parent<V, S>"));\n'
                '        check(fixture.Parent.class.getDeclaredMethod("current").getGenericReturnType().getTypeName().equals("R"));\n'
                '        check(Modifier.isAbstract(Holder.class.getModifiers()));')
            content = content.replace("check(holderMethods == 1)", "check(holderMethods == 2)")
            content = content.replace('        System.out.println("values="',
                '        Receiver<Object,Object> overloaded = new Receiver<>();\n'
                '        check(overloaded.current(5) == 17); values++;\n'
                '        check(overloaded.current("tag") == 19); values++;\n'
                '        fixture.Parent<Object,Object> inherited = overloaded;\n'
                '        same(inherited.current(),overloaded.current());\n'
                '        System.out.println("values="')
            replacement.write_text(content)
            harness_source = replacement
        if args.fixed_throws:
            content = harness_source.read_text()
            before = "public void insert(int index, Object value) {"
            if content.count(before) != 1:
                raise RuntimeError("Missing immutable checked-exception subclass override")
            content = content.replace(before, "public void insert(int index, Object value) throws Exception {", 1)
            marker = "        TypeVariable<?> own = Receiver.class.getTypeParameters()[0];"
            if content.count(marker) != 1:
                raise RuntimeError("Missing immutable deferred harness reflection anchor")
            content = content.replace(marker, """
        Object[] params = new Object[] {first, second, null};
        for (Receiver<Object,Object> target : new Receiver[] {receiver, child}) {
            target.params = params;
            Exception checked = new Exception("owned checked failure");
            target.checkedFailure = checked;
            for (Object item : params) {
                Object previous = target.current();
                try {
                    ((Contract<Object>) target).insert(7, item);
                    throw new AssertionError();
                } catch (Exception actual) { same(actual, checked); failures++; }
                same(target.current(), previous);
                check(target.lastIndex == (target == child ? 7 ^ 0x5a5a5a5a : 7));
            }
            same(target.params, params);
            target.checkedFailure = null;
        }
        check(((java.lang.reflect.GenericArrayType) Holder.class.getDeclaredField("params").getGenericType())
                .getGenericComponentType().equals(Holder.class.getTypeParameters()[1]));
        for (Class<?> owner : new Class<?>[] {Contract.class, Mirror.class, Receiver.class}) {
            for (java.lang.reflect.Method method : owner.getDeclaredMethods()) {
                if (!(method.getName().equals("insert") || method.getName().equals("current") || method.getName().equals("tail"))) continue;
                check(method.getExceptionTypes().length == 1 && method.getExceptionTypes()[0] == Exception.class
                        && method.getGenericExceptionTypes().length == 1 && method.getGenericExceptionTypes()[0] == Exception.class);
            }
        }
""" + marker, 1)
            replacement = work / "fixed-throws-harness" / harness_source.name
            replacement.parent.mkdir()
            replacement.write_text(content)
            harness_source = replacement
        verify.compile_local([harness_source], harness, classpath=originals)
        harness_hashes = {p.relative_to(harness).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sorted(harness.rglob("*.class"))}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "DeferredAbstractInterfaceHarness", "original"], "baseline")
        if baseline.splitlines() != [
                "values=" + str(32 if args.class_ancestor else 30) + ",identities="
                + str((217 if args.class_ancestor else 216) + (14 if args.fixed_throws else 0))
                + ",failures=" + str(36 if args.fixed_throws else 30),
                "inherited-abstract-slots=retained,method-inventory=unchanged,generic-reflection=unchanged"]:
            raise RuntimeError("Owned baseline did not exercise every expected case: " + baseline)

        def dex(source: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [p for p in sorted(source.rglob("*.class"))
                     if "L" + p.relative_to(source).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath",
                        args.android_jar.resolve(), "--classpath", dependency,
                        "--output", destination, *paths], "d8-" + destination.name)
            if list(destination.glob("classes*.dex")) != [destination / "classes.dex"]:
                raise RuntimeError("Owned partition did not produce exactly one DEX")
            return destination / "classes.dex"

        classpath = ["--classpath", args.android_jar.resolve()]
        if declarations:
            classpath += ["--classpath", dependency]
        original_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", original_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, original_dex.name, dex_header(original_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes or
                {row["identity"] for row in inventory["methods"]} != identities):
            raise RuntimeError("D8 changed the independently authored class or method inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", original_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != identities or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(identities) or
                coverage["unrecovered_method_count"] != 0 or coverage.get("projected_method_count", 0) != 0 or
                coverage.get("generic_signature_projections")):
            raise RuntimeError("Recovery invented a declaration or changed a complete source formal")
        holder = output / "sources/fixture/Holder.java"
        if (" insert(" in holder.read_text() or " tail(" in holder.read_text() or
                (not args.class_ancestor and " current(" in holder.read_text())):
            raise RuntimeError("Abstract holder acquired an invented method")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helpers = coverage.get("generated_source_helpers", [])
        helper_identities = {row["class"] + "->" + row["name"] + row["prototype"] for row in helpers}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in ("Lfixture/Holder;", "Lfixture/Receiver;")}
        if args.class_ancestor and not args.dependency_ancestor:
            expected_helpers.add("Lfixture/Parent;->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;")
        if helper_identities != expected_helpers or len(helpers) != len(expected_helpers) or any(
                row["kind"] != "throw-helper" or not row["static"] for row in helpers):
            raise RuntimeError("Source exception helper inventory was not reported exactly")
        if rebuilt_classes != classes or set(rebuilt_methods) != identities | helper_identities:
            raise RuntimeError("javac did not preserve the exact original method inventory")
        if args.fixed_throws:
            actual_records = class_identity.compiler_classes(rebuilt)
            for owner in classes:
                wanted, after = records[owner], actual_records[owner]
                for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                            "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                    if after.get(key) != wanted.get(key):
                        raise RuntimeError("Original checked-exception class metadata changed: " + owner + ":" + key)
                for identity, metadata in wanted["methods"].items():
                    if after["methods"][identity] != metadata:
                        raise RuntimeError("Original checked-exception method metadata changed: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, dependency))),
                             "DeferredAbstractInterfaceHarness", "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Owned source changed interface dispatch, identity, failures or reflection")

        negative = work / "negative"
        shutil.copytree(originals, negative)
        path = negative / "fixture/Holder.class"
        before = path.read_bytes()
        receipt = change_signature(path, signature, signature.replace(b"Mirror<TS;>", b"Mirror<TV;>"))
        if method_code(before) != method_code(path.read_bytes()):
            raise RuntimeError("Signature counterexample changed Code or descriptors")
        (work / "negative-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(dex(negative, work / "negative-dex")),
                                 *map(str, classpath), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        expected_error = ("no exact specialized implementation" if args.class_ancestor else
                          "conflicting complete substitutions")
        if result.returncode == 0 or rejected.exists() or expected_error not in result.stderr:
            raise RuntimeError("Equal erasures hid incompatible inherited result scopes")
        negative_java = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            if source.name == "Receiver.java":
                continue
            content = source.read_text()
            if source.name == "Holder.java":
                content = content.replace("Mirror<S>", "Mirror<V>")
            destination = negative_java / "fixture" / source.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs],
                                  text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0 or "incompatible" not in compiler.stderr:
            raise RuntimeError("Independent javac did not reject incompatible abstract requirements")
        if args.fixed_throws:
            invalid_throws = work / "invalid-throws-originals"
            shutil.copytree(originals, invalid_throws)
            receipt = change_signature(invalid_throws / "fixture/Contract.class", b"java/lang/Exception", b"java/lang/String")
            (work / "invalid-throws-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
            invalid_classpath = list(classpath)
            if declarations:
                invalid_dependency = work / "invalid-throws-contracts.jar"
                with zipfile.ZipFile(invalid_dependency, "x", zipfile.ZIP_DEFLATED) as archive:
                    for path in sorted(invalid_throws.rglob("*.class")):
                        relative = path.relative_to(invalid_throws).as_posix()
                        if "L" + relative[:-6] + ";" in declarations:
                            archive.writestr(relative, path.read_bytes())
                invalid_classpath[-1] = invalid_dependency
            invalid_output = work / "invalid-throws-rejected"
            result = subprocess.run([str(args.neverd.resolve()), "mobile",
                                     str(dex(invalid_throws, work / "invalid-throws-dex")),
                                     *map(str, invalid_classpath), "-o", str(invalid_output)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "invalid-throws-recovery.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or invalid_output.exists() or "throws" not in result.stderr:
                raise RuntimeError("Deferred exception proof accepted a non-Throwable declaration")
        if harness_hashes != {p.relative_to(harness).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in sorted(harness.rglob("*.class"))}:
            raise RuntimeError("Immutable deferred harness bytes changed")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in originals.rglob("*.class")} or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash:
            raise RuntimeError("Original owned classes or declaration JAR changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(identities),
                    "source_interfaces": args.source_interfaces, "original_code_and_descriptors": "unchanged",
                    "fixed_throws": args.fixed_throws,
                    "full_fixed_exception_metadata": "unchanged" if args.fixed_throws else "not applicable",
                    "non_throwable_counterexample": "rejected with original Code" if args.fixed_throws else "not applicable",
                    "class_ancestor": args.class_ancestor, "dependency_ancestor": args.dependency_ancestor,
                    "invented_interface_methods_and_bridges": 0, "values": 32 if args.class_ancestor else 30,
                    "identity_cases": (217 if args.class_ancestor else 216) + (14 if args.fixed_throws else 0),
                    "generated_exception_helpers": sorted(helper_identities),
                    "ordered_failure_cases": 36 if args.fixed_throws else 30, "generic_reflection": "unchanged",
                    "deferred_scope_conflict": "rejected-by-neverd-and-javac", "jvm_output": "matched",
                    "coverage": coverage["status"], "original_class_sha256": hashes,
                    "declaration_jar_sha256": dependency_hash}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
