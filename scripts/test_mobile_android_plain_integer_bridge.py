#!/usr/bin/env python3
"""Verify original final scalar and guarded reference forwarding bridges."""
from __future__ import annotations

import argparse
import copy
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def bridge_flag(path: Path, method_name: str = "size", prototype: str = "()I",
                original_flags: int = 0x11, input_flags: int = 0x51) -> dict:
    """Add bridge/synthetic flags only; keep every descriptor and Code byte."""
    if input_flags & ~0x1040 != original_flags or not input_flags & 0x40:
        raise RuntimeError("Owned flag edit must only add bridge/synthetic flags")
    original = path.read_bytes()
    shape = class_records(original)
    strings = shape["strings"]
    offset = shape["pool_end"] + 6
    offset += 2 + 2 * int.from_bytes(original[offset:offset + 2], "big")

    def attributes(position: int, size: int) -> tuple:
        names = []
        for _ in range(size):
            name, length = struct.unpack_from(">HI", original, position)
            names.append(strings[name])
            position += 6 + length
        return position, names

    fields = int.from_bytes(original[offset:offset + 2], "big")
    offset += 2
    for _ in range(fields):
        offset, _ = attributes(offset + 8, int.from_bytes(original[offset + 6:offset + 8], "big"))
    methods = int.from_bytes(original[offset:offset + 2], "big")
    offset += 2
    targets = []
    for _ in range(methods):
        start = offset
        flags, name, descriptor, size = struct.unpack_from(">HHHH", original, start)
        offset, names = attributes(start + 8, size)
        if strings[name] == method_name.encode() and strings[descriptor] == prototype.encode():
            if flags != original_flags or names.count(b"Code") != 1:
                raise RuntimeError("Owned forwarding declaration changed ABI or access")
            targets.append(start)
    if len(targets) != 1:
        raise RuntimeError("Missing unique owned plain bridge")
    position = targets[0]
    changed = original[:position] + struct.pack(">H", input_flags) + original[position + 2:]
    if method_code(original) != method_code(changed):
        raise RuntimeError("Bridge flag edit changed Code bytes or descriptors")
    path.write_bytes(changed)
    return {"original_flags": original_flags, "input_flags": input_flags, "code_and_descriptors": "unchanged",
            "before_sha256": hashlib.sha256(original).hexdigest(),
            "after_sha256": hashlib.sha256(changed).hexdigest()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--guarded-reference", action="store_true")
    parser.add_argument("--guarded-integer", action="store_true")
    parser.add_argument("--generic-reference-getter", action="store_true")
    parser.add_argument("--predicate-guard", action="store_true")
    parser.add_argument("--indexed-reference-getter", action="store_true")
    parser.add_argument("--indexed-abstract-ancestor", action="store_true")
    parser.add_argument("--class-ancestor", action="store_true")
    parser.add_argument("--inherited-guarded", action="store_true")
    parser.add_argument("--dependency-root", action="store_true")
    parser.add_argument("--captured-member", action="store_true")
    args = parser.parse_args()
    args.indexed_reference_getter |= args.indexed_abstract_ancestor
    args.class_ancestor |= args.dependency_root or args.inherited_guarded
    args.guarded_reference |= args.class_ancestor
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-plain-integer-bridge-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        if sum([args.guarded_reference, args.guarded_integer, args.generic_reference_getter, args.predicate_guard, args.indexed_reference_getter]) > 1:
            raise RuntimeError("Select one forwarding bridge profile")
        if args.captured_member and any((args.generic_reference_getter, args.predicate_guard,
                                        args.indexed_reference_getter, args.class_ancestor)):
            raise RuntimeError("Captured members require a scalar forwarding profile")
        guarded_reference = args.guarded_reference or args.guarded_integer or args.predicate_guard
        generic_reference = args.generic_reference_getter or args.indexed_reference_getter
        fixture_name = ("guarded_integer_bridge" if args.guarded_integer else "indexed_reference_bridge" if args.indexed_reference_getter else "predicate_reference_bridge" if args.predicate_guard else "generic_reference_getter" if args.generic_reference_getter else
                        "guarded_reference_bridge" if guarded_reference else "plain_integer_bridge")
        fixture = FIXTURES / (fixture_name + "/java")
        harness = ("GuardedIntegerBridgeHarness" if args.guarded_integer else "IndexedReferenceBridgeHarness" if args.indexed_reference_getter else "PredicateReferenceBridgeHarness" if args.predicate_guard else "GenericReferenceGetterHarness" if args.generic_reference_getter else
                   "GuardedReferenceBridgeHarness" if guarded_reference else "PlainIntegerBridgeHarness")
        bridge_name = "indexOf" if args.guarded_integer else "remove" if args.indexed_reference_getter else "items" if args.generic_reference_getter else "contains" if guarded_reference else "size"
        prototype = "(Ljava/lang/Object;)I" if args.guarded_integer else "(I)Ljava/lang/Object;" if args.indexed_reference_getter else "()Ljava/util/Set;" if args.generic_reference_getter else "(Ljava/lang/Object;)Z" if guarded_reference else "()I"
        identity = "Lfixture/Counter;->" + bridge_name + prototype
        target_identity = ("Lfixture/Counter;->indexOf(Lfixture/Element;)I" if args.guarded_integer else "Lfixture/Counter;->removeAt(I)Ljava/lang/Object;" if args.indexed_reference_getter else "Lfixture/Counter;->getItems()Ljava/util/Set;" if args.generic_reference_getter else
                          "Lfixture/Counter;->contains(Lfixture/Element;)Z" if guarded_reference
                           else "Lfixture/Counter;->getCount()I")
        kind = ("generic-reference-interface-forwarding-bridge" if generic_reference else
                "guarded-interface-forwarding-bridge" if guarded_reference else "plain-interface-forwarding-bridge")
        inputs = sorted(fixture.rglob("*.java"))
        declarations = {"Lfixture/CountView;"}
        if args.class_ancestor:
            kind = "guarded-class-forwarding-bridge"
            authored = work / "owned-java/fixture/Counter.java"
            authored.parent.mkdir(parents=True)
            authored.write_text((fixture / "fixture/Counter.java").read_text().replace(
                "implements CountView<T>", "extends GuardMiddle<T,String> implements CountView<T>"))
            inputs = [p for p in inputs if p.name != "Counter.java"]
            inputs += [authored, *sorted((FIXTURES / "guarded_class_contracts/java").rglob("*.java"))]
        if args.inherited_guarded:
            inputs += sorted((FIXTURES / "inherited_guarded_class_contracts/java").rglob("*.java"))
        if args.dependency_root:
            declarations.add("Lfixture/GuardRoot;")
        bridge_owner = "AbstractCounter" if args.indexed_abstract_ancestor else "Counter"
        bridge_path = "fixture/" + bridge_owner + ".class"
        if args.captured_member:
            owned = work / "captured-java/fixture/Outer.java"
            owned.parent.mkdir(parents=True)
            members = []
            for source in inputs:
                if source.name != "Counter.java":
                    continue
                content = source.read_text()
                if content.count("package fixture;") != 1:
                    raise RuntimeError("Missing exact owned captured integer member")
                content = content.replace("package fixture;", "", 1)
                name = source.stem
                declaration = "public class " + name + "<"
                if content.count(declaration) != 1:
                    raise RuntimeError("Captured integer member changed its full generic declaration")
                method = "owner" if name == "Counter" else "ownerChild"
                content = content.replace("    public " + name + "() {}",
                    "    public " + name + "() {}\n    public Outer " + method + "() { return Outer.this; }", 1)
                members.append(content)
            if len(members) != 1:
                raise RuntimeError("Missing owned captured integer declaration")
            owned.write_text("package fixture;\npublic class Outer {\n" + "\n".join(members) + "}\n")
            inputs = [p for p in inputs if p.name not in ("Counter.java", "Child.java")] + [owned]
            identity = identity.replace("Lfixture/Counter;", "Lfixture/Outer$Counter;")
            target_identity = target_identity.replace("Lfixture/Counter;", "Lfixture/Outer$Counter;")
            bridge_path = "fixture/Outer$Counter.class"
        if args.indexed_abstract_ancestor:
            identity = identity.replace("Lfixture/Counter;", "Lfixture/AbstractCounter;")
            target_identity = target_identity.replace("Lfixture/Counter;", "Lfixture/AbstractCounter;")
            inputs += sorted((FIXTURES / "indexed_abstract_reference/java").rglob("*.java"))
            projected = []
            for source in inputs:
                if source.name == "Counter.java":
                    replacement = work / "owned-java/fixture/Counter.java"
                    replacement.parent.mkdir(parents=True)
                    content = source.read_text().replace("implements CountView<T>",
                                                        "extends AbstractCounter<T> implements CountView<T>")
                    content = content.replace("    public final T remove(int index) { return removeAt(index); }\n", "")
                    replacement.write_text(content)
                    source = replacement
                projected.append(source)
            inputs = projected
        originals, harness_dir = work / "originals", work / "harness"
        verify.compile_local(inputs, originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - declarations
        methods = {key: value for key, value in all_methods.items() if key.split("->", 1)[0] in classes}
        harness_source = FIXTURES / ("harness/" + harness + ".java")
        if args.captured_member:
            replacement = work / "owned-harness" / harness_source.name
            replacement.parent.mkdir()
            content = harness_source.read_text()
            content = content.replace("import fixture.*;", "import fixture.*;\nimport fixture.Outer.Counter;", 1)
            creation = "Counter<Object> counter = new Counter<>();" if guarded_reference else "Counter<String> counter = new Counter<>();"
            if content.count(creation) != 1:
                raise RuntimeError("Missing exact owned captured scalar creation")
            content = content.replace(creation, "Outer owner = new Outer(); " + creation.replace("new Counter<>();", "owner.new Counter<>();"), 1)
            if guarded_reference:
                declaration = "public static class Child extends Counter<Object> {"
                if content.count(declaration) != 1 or content.count("Child child = new Child();") != 1:
                    raise RuntimeError("Missing original independently compiled scalar override")
                content = content.replace(declaration, declaration + "\n        public Child(Outer owner) { owner.super(); }", 1)
                content = content.replace("Child child = new Child();", "Child child = new Child(owner);", 1)
            else:
                content = content.replace("Child<String> child = new Child<>();", "Child<String> child = new Child<>(owner);", 1)
            content = content.replace('        System.out.println("values="',
                '        check(counter.owner() == owner && child.owner() == owner);\n'
                '        check(Modifier.isStatic(Counter.class.getModifiers()) == args[0].equals("recovered"));\n'
                '        Field capture = Counter.class.getDeclaredField("this$0"); capture.setAccessible(true);\n'
                '        check(capture.get(counter) == owner && capture.get(child) == owner);\n'
                '        System.out.println("values="', 1)
            if not guarded_reference:
                child = next(p for p in sorted(fixture.rglob("*.java")) if p.name == "Child.java").read_text()
                child = child.replace("package fixture;", "", 1)
                child = child.replace("public class Child<E>", "public static class Child<E>", 1)
                child = child.replace("public Child() {}", "public Child(Outer owner) { owner.super(); }", 1)
                at = content.rfind("}")
                content = content[:at] + child + content[at:]
            replacement.write_text(content)
            harness_source = replacement
        if args.indexed_abstract_ancestor:
            replacement = work / "owned-harness" / harness_source.name
            replacement.parent.mkdir()
            content = harness_source.read_text().replace('Counter.class.getDeclaredMethod("remove", int.class)',
                                                         'AbstractCounter.class.getDeclaredMethod("remove", int.class)')
            content = content.replace('Counter.class.getDeclaredMethod("removeAt", int.class)',
                                      'AbstractCounter.class.getDeclaredMethod("removeAt", int.class)')
            content = content.replace('bridge.getGenericReturnType().equals(Counter.class.getTypeParameters()[0])',
                                      'bridge.getGenericReturnType().equals(AbstractCounter.class.getTypeParameters()[0])')
            content = content.replace('getDeclaringClass() == Counter.class', 'getDeclaringClass() == AbstractCounter.class')
            content = content.replace('        System.out.println("values="',
                '        check(Modifier.isAbstract(target.getModifiers()));\n'
                '        check(AbstractCounter.class.getGenericSuperclass().getTypeName().equals("fixture.ReferenceRoot<P>"));\n'
                '        check(Counter.class.getGenericSuperclass().getTypeName().equals("fixture.AbstractCounter<T>"));\n'
                '        for (Method method : Counter.class.getDeclaredMethods()) check(!method.getName().equals("remove"));\n'
                '        System.out.println("values="')
            replacement.write_text(content)
            harness_source = replacement
        if args.class_ancestor:
            replacement = work / "owned-harness" / harness_source.name
            replacement.parent.mkdir(parents=True, exist_ok=True)
            content = harness_source.read_text().replace(
                '        System.out.println("values="',
                '        check(Counter.class.getGenericSuperclass().getTypeName().equals("fixture.GuardMiddle<T, java.lang.String>"));\n'
                '        check(GuardMiddle.class.getGenericSuperclass().getTypeName().equals("fixture.GuardRoot<M, N>"));\n'
                '        check(GuardRoot.class.getGenericInterfaces()[0].getTypeName().equals("fixture.CountView<R>"));\n'
                '        GuardRoot<Object,String> ancestor = counter;\n'
                '        GuardMiddle<Object,String> middle = counter;\n'
                '        value(ancestor.contains(null), false);\n'
                '        value(middle.contains("wrong"), false);\n'
                '        fails(() -> ancestor.contains(hit), sentinel); same(counter.last, hit);\n'
                '        check(counter.calls == 13 && counter.rootCalls == 0 && child.rootCalls == 0);\n'
                '        System.out.println("values="')
            if args.inherited_guarded:
                content = content.replace("Child extends Counter<Object>", "Child extends InheritedGuard<Object>")
                content = content.replace("Counter<Object> counter = new Counter<>();",
                                          "InheritedGuard<Object> counter = new InheritedGuard<>();")
                content = content.replace('        System.out.println("values="',
                    '        check(InheritedGuard.class.getGenericSuperclass().getTypeName().equals("fixture.Counter<U>"));\n'
                    '        check(InheritedGuard.class.getGenericInterfaces()[0].getTypeName().equals("fixture.CountView<U>"));\n'
                    '        check(InheritedGuard.class.getMethod("contains", Object.class).getDeclaringClass() == Counter.class);\n'
                    '        for (Method method : InheritedGuard.class.getDeclaredMethods()) check(!method.getName().equals("contains"));\n'
                    '        System.out.println("values="')
            replacement.write_text(content)
            harness_source = replacement
        verify.compile_local([harness_source], harness_dir, classpath=originals)
        harness_hashes = {p.relative_to(harness_dir).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sorted(harness_dir.rglob("*.class"))}
        dependency = work / "owned-contract.jar"
        with zipfile.ZipFile(dependency, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for descriptor in sorted(declarations):
                path = descriptor[1:-1] + ".class"
                archive.writestr(path, (originals / path).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        receipt = bridge_flag(originals / bridge_path, bridge_name, prototype)
        (work / "input-flag-change.json").write_text(json.dumps(receipt, indent=2) + "\n")
        original_records = class_identity.compiler_classes(originals)
        hashes = {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(originals.rglob("*.class"))}
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               harness, "original"], "baseline")
        expected = (["values=35,identities=31,failures=5",
                     "guarded-integer-bridge=matched,virtual-override=retained,flags=reported"]
                    if args.guarded_integer else ["values=16,identities=9,failures=10",
                     "indexed-reference-bridge=matched,index-and-virtual-override=retained,flags=reported"]
                    if args.indexed_reference_getter else ["values=37,identities=16,failures=5",
                     "predicate-guard=matched,call-effects-and-failures=retained,flags=reported"]
                    if args.predicate_guard else ["values=10,identities=15,failures=4",
                     "generic-reference-getter=matched,virtual-override=retained,flags=reported"]
                    if args.generic_reference_getter else ["values=20,identities=16,failures=5",
                     "guarded-reference-bridge=matched,virtual-override=retained,flags=reported"]
                    if guarded_reference else ["values=12,failures=4",
                    "plain-integer-bridge=matched,virtual-override=retained,flags=reported"])
        if args.class_ancestor:
            expected[0] = "values=22,identities=17,failures=6"
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned forwarding baseline missed expected cases")

        def owned_dex(directory: Path, output: Path) -> Path:
            output.mkdir(parents=True)
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath",
                        args.android_jar.resolve(), "--classpath", dependency, "--output", output,
                        *[path for path in sorted(directory.rglob("*.class"))
                          if "L" + path.relative_to(directory).as_posix()[:-6] + ";" in classes]],
                       "d8-" + output.name)
            dex = output / "classes.dex"
            if not dex.is_file() or list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned bridge did not produce exactly one DEX")
            return dex

        dex = owned_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned forwarding inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "--classpath", dependency, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        projected = [row for row in coverage["methods"]
                     if row.get("projection_kind") == kind]
        projected_count = sum(key.split("->", 1)[0] in ("Lfixture/Outer$Counter;",)
                              for key in methods) if args.captured_member else 1
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["projected_method_count"] != projected_count
                or coverage["unrecovered_method_count"] != 0 or coverage["status"] != "partial"
                or coverage.get("generic_signature_projections") or len(projected) != 1
                or projected[0]["identity"] != identity
                or projected[0]["source_identity"] != projected[0]["identity"]
                or projected[0]["projection_kind"] != kind
                or projected[0]["bridge_target"] != target_identity
                or (args.class_ancestor and projected[0].get("inherited_class_contract")
                    != "Lfixture/GuardRoot;->contains(Ljava/lang/Object;)Z")
                or (args.predicate_guard and projected[0].get("guard_predicate") != "Lfixture/Probe;->accept(Ljava/lang/Object;)Z")
                or not projected[0].get("metadata_differences")):
            raise RuntimeError("Original forwarding bridge lost its body, descriptor or flag receipt")
        if args.captured_member:
            bindings = coverage.get("class_source_bindings", [])
            if (len(bindings) != 1 or coverage["minimum_java_release"] != 25
                    or any(r.get("binding_kind") != "inner-to-static-member"
                           or not r.get("metadata_differences") for r in bindings)):
                raise RuntimeError("Captured integer forwarding lost its proven outer source binding")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency,
                             release=25 if args.captured_member else 8)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("javac changed the complete original/helper inventory")
        if args.inherited_guarded or args.captured_member:
            expected_records = copy.deepcopy(original_records)
            if args.captured_member:
                for owner in ("Lfixture/Outer$Counter;",):
                    expected_records[owner]["inner_class"]["access"] |= 8
                    expected_records[owner]["fields"][owner + "->this$0:Lfixture/Outer;"]["access"] &= ~0x1000
            actual_records = class_identity.compiler_classes(rebuilt)
            for owner in classes:
                expected, actual = expected_records[owner], actual_records[owner]
                for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                    if expected[key] != actual[key]:
                        raise RuntimeError("Original inherited class metadata changed: " + owner + ":" + key)
                for method_id, expected_method in expected["methods"].items():
                    expected_method = dict(expected_method)
                    if method_id == identity:
                        expected_method["access"] &= ~0x40
                    if expected_method != actual["methods"][method_id]:
                        raise RuntimeError("Original inherited method metadata changed beyond reported bridge flag: " + method_id)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir, dependency))),
                             harness, "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Recovered bridge changed virtual dispatch or ordered effects")
        negative_sources = work / "negative-java"
        negative_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == bridge_owner + ".java" or (args.captured_member and source.name == "Outer.java"):
                original = "return -1;" if args.guarded_integer else "return removeAt(index);" if args.indexed_reference_getter else "return getItems();" if args.generic_reference_getter else "return false;" if guarded_reference else "return getCount();"
                changed = "{ calls++; return -1; }" if args.guarded_integer else "return removeAt(index + 1);" if args.indexed_abstract_ancestor else "calls++; return removeAt(index);" if args.indexed_reference_getter else "calls++; return getItems();" if args.generic_reference_getter else "return true;" if guarded_reference else "count++; return getCount();"
                if content.count(original) != 1:
                    raise RuntimeError("Missing independently authored forwarding body")
                content = content.replace(original, changed)
            destination = negative_sources / "fixture" / source.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            negative_inputs.append(destination)
        negative = work / "negative-originals"
        verify.compile_local(negative_inputs, negative)
        negative_receipt = bridge_flag(negative / bridge_path, bridge_name, prototype)
        (work / "negative-flag-change.json").write_text(json.dumps(negative_receipt, indent=2) + "\n")
        negative_dex = owned_dex(negative, work / "negative-dex")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "--classpath", str(dependency), "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic inheritance" not in result.stderr:
            raise RuntimeError("An unproven bridge was approximated as original forwarding")
        if hashes != {p.relative_to(originals).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted(originals.rglob("*.class"))}:
            raise RuntimeError("Original owned class bytes changed")
        if hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash:
            raise RuntimeError("Owned dependency declaration changed")
        if harness_hashes != {p.relative_to(harness_dir).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in sorted(harness_dir.rglob("*.class"))}:
            raise RuntimeError("Original independent harness changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "original_bridge_count": 1, "rebuilt_bridge_count": 0, "value_cases": 20 if guarded_reference else 12,
                    "identity_cases": 16 if guarded_reference else 0,
                    "ordered_failure_cases": 5 if guarded_reference else 4, "virtual_override": "matched", "mutable_state": "matched",
                    "generic_reflection": "unchanged", "bridge_flag_difference": "reported",
                    "coverage": "partial", "original_descriptors_and_forwarding_body": "retained",
                    "forwarding_counterexample": "rejected", "projection_kind": kind, "original_class_sha256": hashes,
                    "jvm_output": "matched"}
        if args.captured_member:
            evidence.update(captured_member=True, outer_identity_cases=4,
                            inherited_final_bridge="original captured parent declaration only",
                            child_getter_override="matched", java_release=25,
                            original_metadata_and_complete_inventory="unchanged except reported member bindings, capture-field flags, bridge flag and helpers")
        if args.class_ancestor:
            evidence.update(value_cases=22, identity_cases=17, ordered_failure_cases=6,
                            complete_generic_class_ancestors=True,
                            unchanged_dependency_root=args.dependency_root,
                            class_slot_dispatch_and_zero_root_effects="matched",
                            complete_rebuilt_inventory="original methods and reported helpers only",
                            inherited_original_guarded_body=args.inherited_guarded)
        if args.generic_reference_getter:
            evidence.update(value_cases=10, identity_cases=15, ordered_failure_cases=4)
        if args.guarded_integer:
            evidence.update(value_cases=35, identity_cases=31, ordered_failure_cases=5,
                            original_integer_sentinel="retained", integer_extremes="matched")
        if args.indexed_reference_getter:
            evidence.update(value_cases=16, identity_cases=9, ordered_failure_cases=10,
                            original_index_argument_and_mutable_removal="retained",
                            indexed_abstract_class_ancestor=args.indexed_abstract_ancestor)
        if args.predicate_guard:
            evidence.update(value_cases=37, identity_cases=16, ordered_failure_cases=5,
                            guard_predicate_effects_and_failures="retained", guard_call_and_target_order="matched")
            evidence.pop("virtual_override")
        if "Lfixture/Child;" in classes:
            evidence.update(inherited_final_bridge="parent declaration only",
                            child_getter_override="matched", inherited_generic_reflection="unchanged")
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
