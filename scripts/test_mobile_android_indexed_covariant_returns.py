#!/usr/bin/env python3
"""Compare original indexed result bridges across full class/interface scopes."""
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
    from scripts.test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_static_defaults import method_code
    from scripts.test_mobile_android_upcast_result_bridges import result_cast
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_static_defaults import method_code
    from test_mobile_android_upcast_result_bridges import result_cast


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--omit-interface-bridge", action="store_true")
    parser.add_argument("--interface-default", action="store_true")
    parser.add_argument("--inherited-default", action="store_true")
    parser.add_argument("--reference-input", action="store_true")
    parser.add_argument("--getter-chain", action="store_true")
    parser.add_argument("--deferred-class", action="store_true")
    parser.add_argument("--inherited-entry", action="store_true")
    parser.add_argument("--inherited-bridge", action="store_true")
    parser.add_argument("--class-bridge-override", action="store_true")
    args = parser.parse_args()
    args.inherited_bridge |= args.class_bridge_override
    args.inherited_entry |= args.inherited_bridge
    args.interface_default |= args.inherited_default
    args.reference_input |= args.inherited_entry
    if args.interface_default and args.omit_interface_bridge:
        parser.error("The default profile requires its original interface bridge")
    if args.reference_input and args.interface_default:
        parser.error("The reference-input profile uses abstract interface contracts")
    if args.getter_chain and any((args.interface_default, args.reference_input, args.omit_interface_bridge)):
        parser.error("The getter-chain profile requires its two original forwarding entries")
    if args.deferred_class and (args.interface_default or args.getter_chain):
        parser.error("The deferred-class profile keeps its interface declarations abstract")
    if args.inherited_entry and any((args.interface_default, args.getter_chain, args.deferred_class, args.omit_interface_bridge)):
        parser.error("The inherited-entry profile requires the original class bridge")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-indexed-result-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = "covariant_getter_chains/java" if args.getter_chain else "indexed_covariant_returns/java"
        inputs = sorted((FIXTURES / fixture).rglob("*.java"))
        if args.inherited_entry:
            inputs = sorted((FIXTURES / "inherited_covariant_entries/java").rglob("*.java"))
            inputs += [FIXTURES / "indexed_covariant_returns/java/fixture" / name
                       for name in ("Base.java", "Leaf.java", "Trace.java")]
            if args.inherited_bridge:
                refined = FIXTURES / "indexed_covariant_returns/java/fixture/RefinedIndex.java"
                inputs = [refined if p.name == "RefinedIndex.java" else p for p in inputs]
                inputs.append(FIXTURES / "indexed_covariant_returns/java/fixture/Indexed.java")
        if args.interface_default:
            refined = FIXTURES / "indexed_covariant_defaults/java/fixture/RefinedIndex.java"
            inputs = [refined if p.name == "RefinedIndex.java" else p for p in inputs]
            receiver = next(p for p in inputs if p.name == "Receiver.java")
            replacement = work / "owned-java/fixture/Receiver.java"
            replacement.parent.mkdir(parents=True)
            content = receiver.read_text().replace("    public static Base<?> checked", "    public Leaf<A> select(int lower, int upper) { return create(lower,upper); }\n    public static Base<?> checked")
            replacement.write_text(content)
            inputs = [replacement if p == receiver else p for p in inputs]
        if args.inherited_default:
            inputs += sorted((FIXTURES / "inherited_indexed_defaults/java").rglob("*.java"))
        if args.deferred_class:
            inputs += sorted((FIXTURES / "deferred_covariant_interfaces/java").rglob("*.java"))
            projected = []
            for source in inputs:
                if source.name in ("Root.java", "Receiver.java"):
                    replacement = work / "owned-java/fixture" / source.name
                    replacement.parent.mkdir(parents=True, exist_ok=True)
                    content = source.read_text()
                    if source.name == "Root.java":
                        content = content.replace("abstract class Root", "interface Root").replace("    public Root() {}\n", "")
                    else:
                        content = content.replace("extends Root<A> implements RefinedIndex<A>",
                                                  "extends Holder<A,B> implements RefinedIndex<A>, Root<A>")
                    replacement.write_text(content)
                    source = replacement
                projected.append(source)
            inputs = projected
        if args.reference_input:
            projected = []
            for source in inputs:
                formal = {"Root.java": "R", "Indexed.java": "I", "RefinedIndex.java": "P", "Receiver.java": "A"}.get(source.name)
                if formal:
                    replacement = work / "owned-java/fixture" / source.name
                    replacement.parent.mkdir(parents=True, exist_ok=True)
                    content = source.read_text().replace("int lower", formal + " lower")
                    content = content.replace("public A lower, upper, calls;", "public A lower;\n    public int upper, calls;")
                    replacement.write_text(content)
                    source = replacement
                projected.append(source)
            inputs = projected
        if args.class_bridge_override:
            projected = []
            for source in inputs:
                if source.name == "Receiver.java":
                    replacement = work / "override-java/fixture/Receiver.java"
                    replacement.parent.mkdir(parents=True)
                    content = source.read_text().replace(" implements RefinedIndex<A>", "")
                    content = content.replace("    public Receiver() {}",
                        "    public Receiver() {}\n"
                        "    public Leaf<A> create(A lower,int upper) {\n"
                        "        Trace.events = Trace.events * 10 + 2;\n"
                        "        return super.create(lower,upper);\n    }")
                    replacement.write_text(content)
                    source = replacement
                projected.append(source)
            inputs = projected
        verify.compile_local(inputs, originals)
        harness_name = ("CovariantGetterChainHarness" if args.getter_chain else
                        "IndexedCovariantDefaultHarness" if args.interface_default else "IndexedCovariantReturnHarness")
        harness_source = FIXTURES / f"harness/{harness_name}.java"
        if args.inherited_default:
            replacement = work / "owned-harness" / harness_source.name
            replacement.parent.mkdir()
            content = harness_source.read_text()
            content = content.replace("callDefault(DefaultOnly target", "callDefault(DefaultReceiver<String,Object> target")
            content = content.replace("DefaultOnly target = new DefaultOnly();", "DefaultReceiver<String,Object> target = new DefaultReceiver<>();")
            content = content.replace("DefaultOnly.class.getMethod", "DefaultReceiver.class.getMethod")
            content = content.replace('        check(Receiver.class.getGenericSuperclass()',
                                      '        check(InheritedIndex.class.getGenericInterfaces()[0].getTypeName().equals("fixture.IndexMiddle<X, Y>"));\n'
                                      '        check(InheritedIndex.class.getGenericInterfaces()[1].getTypeName().equals("fixture.IndexSibling<Y>"));\n'
                                      '        check(DefaultReceiver.class.getGenericInterfaces()[0].getTypeName().equals("fixture.InheritedIndex<L, R>"));\n'
                                      '        check(Receiver.class.getGenericSuperclass()')
            replacement.write_text(content)
            harness_source = replacement
        if args.deferred_class:
            replacement = work / "deferred-harness" / harness_source.name
            replacement.parent.mkdir()
            content = harness_source.read_text().replace('"fixture.Root<A>"', '"fixture.Holder<A, B>"')
            content = content.replace('        System.out.println("values="',
                '        for (Method method : Holder.class.getDeclaredMethods()) check(!method.getName().equals("create"));\n'
                '        check(Holder.class.getGenericInterfaces()[0].getTypeName().equals("fixture.RefinedIndex<S>"));\n'
                '        System.out.println("values="')
            replacement.write_text(content)
            harness_source = replacement
        if args.inherited_entry:
            replacement = work / "inherited-harness" / harness_source.name
            replacement.parent.mkdir()
            content = harness_source.read_text()
            if not args.class_bridge_override:
                content = content.replace("Receiver.class.getDeclaredMethods()", "Core.class.getDeclaredMethods()")
                content = content.replace("Receiver.class.getDeclaredMethod(", "Core.class.getDeclaredMethod(")
            content = content.replace('"fixture.Root<A>"', '"fixture.Core<A, B>"')
            if not args.inherited_bridge:
                content = content.replace('check(RefinedIndex.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Indexed<P>"));',
                                          'check(RefinedIndex.class.getGenericInterfaces().length == 0);')
                content = content.replace("Indexed.class.getMethod(", "Root.class.getMethod(")
                content = content.replace("if (mode == 3) return ((Indexed<Object>)receiver).create(lower,upper);", "if (mode == 3) return ((Root<Object>)receiver).create(lower,upper);")
                content = content.replace('bridges == (args.length != 0 && args[0].equals("omitted") ? 0 : 1)', 'bridges == 0')
            declaration_check = ('' if args.class_bridge_override else
                                 '        for (Method method : Receiver.class.getDeclaredMethods()) check(!method.getName().equals("create"));\n')
            content = content.replace('        System.out.println("values="',
                                      declaration_check +
                                      '        check(Core.class.getGenericSuperclass().getTypeName().equals("fixture.Root<A>"));\n'
                                      '        System.out.println("values="')
            if args.class_bridge_override:
                content = content.replace("receiver instanceof Child ? 91 : 1", "receiver instanceof Child ? 921 : 21")
                content = content.replace("target instanceof Child ? 91 : 1", "target instanceof Child ? 921 : 21")
                content = content.replace('check(Receiver.class.getGenericInterfaces()[0].getTypeName().equals("fixture.RefinedIndex<A>"));',
                                          'check(Receiver.class.getGenericInterfaces().length == 0);')
            replacement.write_text(content)
            harness_source = replacement
        if args.reference_input:
            replacement = work / "owned-harness" / harness_source.name
            replacement.parent.mkdir()
            content = harness_source.read_text().replace("int lower", "Object lower")
            content = content.replace("check(target.lower == -3)", "check(Integer.valueOf(-3).equals(target.lower))")
            content = content.replace("for (Object lower : indices)", "for (Object lower : inputs)")
            content = content.replace("        int[] indices =", '        Object[] inputs = {null,new Object(),"input",Integer.valueOf(513),new Object[] {token}};\n        int[] indices =')
            content = content.replace('getMethod("create",int.class,int.class)', 'getMethod("create",Object.class,int.class)')
            content = content.replace('        check(Receiver.class.getGenericSuperclass()',
                                      '        check(Receiver.class.getDeclaredMethod("create",Object.class,int.class).getGenericParameterTypes()[0].getTypeName().equals("A"));\n'
                                      '        check(RefinedIndex.class.getDeclaredMethod("create",Object.class,int.class).getGenericParameterTypes()[0].getTypeName().equals("P"));\n'
                                      '        check(Receiver.class.getGenericSuperclass()')
            if args.inherited_entry and not args.class_bridge_override:
                content = content.replace("Receiver.class.getDeclaredMethod(", "Core.class.getDeclaredMethod(")
            replacement.write_text(content)
            harness_source = replacement
        verify.compile_local([harness_source], harness, classpath=originals)
        if args.getter_chain:
            receipt = bridge_flag(originals / "fixture/Receiver.class", "create", "()Lfixture/Leaf;")
            (work / "owned-final-getter-flag-edit.json").write_text(json.dumps(receipt, indent=2) + "\n")
        bridge_owner = "Core" if args.inherited_entry and not args.class_bridge_override else "Receiver"
        if not args.class_bridge_override:
            receipt = result_cast(originals / f"fixture/{bridge_owner}.class", primitive_inputs=not (args.reference_input or args.getter_chain),
                                  reference_inputs=args.reference_input)
            (work / "owned-bridge-code-edit.json").write_text(json.dumps(receipt, indent=2) + "\n")
        if args.omit_interface_bridge:
            receipt = omit_owned_interface_bridge(originals / "fixture/RefinedIndex.class",
                                                  b"create(Ljava/lang/Object;I)Lfixture/Base;" if args.reference_input else b"create(II)Lfixture/Base;")
            (work / "owned-interface-method-edit.json").write_text(json.dumps(receipt, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        def hashes(root: Path) -> dict:
            return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(root.rglob("*.class"))}
        input_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_name, "default" if args.interface_default else "omitted" if args.omit_interface_bridge else "original"], "baseline")
        counts = ("values=48,identities=32,failures=8" if args.getter_chain else
                  "values=808,identities=781,failures=17" if args.interface_default else "values=742,identities=721,failures=14")
        prefix = ("final-entry=retained,virtual-getter=matched,full-generic-reflection=retained,bridge-flags=reported" if args.getter_chain else
                  "full-indexed-result-substitution=retained,original-argument-order-and-dispatch=matched,interface-bridges=")
        inherited_typed_only = args.inherited_entry and not args.inherited_bridge
        interface_entries = int(not (args.omit_interface_bridge or inherited_typed_only))
        if baseline.splitlines() != [counts, prefix if args.getter_chain else prefix + str(interface_entries)]:
            raise RuntimeError("Owned indexed covariance baseline missed cases: " + baseline)
        classpath = ["--classpath", args.android_jar.resolve()]
        def make_dex(source: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                        "--output", destination, *sorted(source.rglob("*.class"))], "d8-" + destination.name)
            dex = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned indexed fixture did not produce one DEX")
            return dex
        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned indexed input inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] != 0 or coverage["projected_method_count"] != (3 if args.class_bridge_override else 1 if inherited_typed_only else 2) or
                coverage["status"] != "partial" or coverage.get("generic_signature_projections")):
            raise RuntimeError("Indexed result proof changed a full generic contract or original identity")
        parameters = "()" if args.getter_chain else "(Ljava/lang/Object;I)" if args.reference_input else "(II)"
        bridge = next(r for r in coverage["methods"] if r["identity"] == f"Lfixture/{bridge_owner};->create" + parameters + "Lfixture/Base;")
        if bridge.get("bridge_target") != f"Lfixture/{bridge_owner};->create" + parameters + "Lfixture/Leaf;":
            raise RuntimeError("Indexed class bridge lost its exact typed result witness")
        if args.getter_chain:
            typed = next(r for r in coverage["methods"] if r["identity"] == "Lfixture/Receiver;->create()Lfixture/Leaf;")
            if (typed.get("bridge_target") != "Lfixture/Receiver;->getCreated()Lfixture/Leaf;" or
                    typed.get("projection_kind") != "generic-reference-interface-forwarding-bridge" or
                    not typed.get("metadata_differences")):
                raise RuntimeError("Original final generic bridge lost its unchanged forwarding body or flag receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        helper_rows = coverage.get("generated_source_helpers", [])
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in helper_rows}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(methods) | helpers:
            raise RuntimeError("javac added an unreported indexed slot or changed original ownership")
        if args.omit_interface_bridge and "Lfixture/RefinedIndex;->create" + parameters + "Lfixture/Base;" not in helpers:
            raise RuntimeError("Compiler-added indexed interface bridge was not reported")
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             harness_name, "default" if args.interface_default else "recovered"], "recovered-execution")
        if actual.splitlines() != [baseline.splitlines()[0], prefix if args.getter_chain else prefix + str(int(not inherited_typed_only))]:
            raise RuntimeError("Indexed result recovery changed arguments, identity, dispatch or ordered failures")
        negative = work / "negative"
        shutil.copytree(originals, negative)
        target = negative / f"fixture/{bridge_owner}.class"
        before = target.read_bytes()
        signature = (b"()Lfixture/Leaf<TT;>;" if args.getter_chain else
                     b"(TA;I)Lfixture/Leaf<TA;>;" if args.reference_input else b"(II)Lfixture/Leaf<TA;>;")
        wrong = signature.replace(b"Leaf<TT;>", b"Leaf<TU;>") if args.getter_chain else signature.replace(b"Leaf<TA;>", b"Leaf<TB;>")
        receipt = change_signature(target, signature, wrong)
        if method_code(before) != method_code(target.read_bytes()):
            raise RuntimeError("Wrong-class-formal counterexample changed original Code")
        (work / "negative-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = make_dex(negative, work / "negative-dex")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath),
                                 "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Object erasure hid a different class-formal result scope")
        if args.reference_input:
            negative = work / "negative-parameter"
            shutil.copytree(originals, negative)
            target = negative / f"fixture/{bridge_owner}.class"
            before = target.read_bytes()
            receipt = change_signature(target, signature, signature.replace(b"(TA;I)", b"(TB;I)"))
            if method_code(before) != method_code(target.read_bytes()):
                raise RuntimeError("Wrong parameter counterexample changed original Code")
            (work / "negative-parameter-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = make_dex(negative, work / "negative-parameter-dex")
            rejected = work / "rejected-parameter"
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath),
                                    "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "negative-parameter-recovery.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Object erasure hid a different parameter formal")
        if args.inherited_bridge:
            negative = work / "negative-consumer"
            shutil.copytree(originals, negative)
            target = negative / "fixture/Receiver.class"
            before = target.read_bytes()
            signature = (b"<A:Ljava/lang/Object;B:Ljava/lang/Object;>Lfixture/Core<TA;TB;>;" if args.class_bridge_override else
                         b"<A:Ljava/lang/Object;B:Ljava/lang/Object;>Lfixture/Core<TA;TB;>;Lfixture/RefinedIndex<TA;>;")
            wrong = (signature.replace(b"Core<TA;TB;>", b"Core<TB;TA;>") if args.class_bridge_override else
                     signature.replace(b"Lfixture/RefinedIndex<TA;>;", b"Lfixture/RefinedIndex<TB;>;"))
            receipt = change_signature(target, signature, wrong)
            if method_code(before) != method_code(target.read_bytes()):
                raise RuntimeError("Wrong consumer substitution changed original Code")
            (work / "negative-consumer-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = make_dex(negative, work / "negative-consumer-dex")
            rejected = work / "rejected-consumer"
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath),
                                    "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "negative-consumer-recovery.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Inherited erased bridge reused a contract from another consumer scope")
        if args.inherited_default:
            negative = work / "negative-inherited"
            shutil.copytree(originals, negative)
            target = negative / "fixture/InheritedIndex.class"
            before = target.read_bytes()
            signature = b"<X:Ljava/lang/Object;Y:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/IndexMiddle<TX;TY;>;Lfixture/IndexSibling<TY;>;"
            receipt = change_signature(target, signature, signature.replace(b"Lfixture/IndexSibling<TY;>;", b"Lfixture/IndexSibling<TX;>;"))
            if method_code(before) != method_code(target.read_bytes()):
                raise RuntimeError("Inherited argument counterexample changed original Code")
            (work / "negative-inherited-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = make_dex(negative, work / "negative-inherited-dex")
            rejected = work / "rejected-inherited"
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath),
                                    "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "negative-inherited-recovery.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Same-erasure repeated interface arguments reused an owner-local default proof")
        if hashes(originals) != input_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original indexed fixture or concrete harness changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 48 if args.getter_chain else 808 if args.interface_default else 742,
                    "identity_cases": 32 if args.getter_chain else 781 if args.interface_default else 721,
                    "ordered_failure_cases": 8 if args.getter_chain else 17 if args.interface_default else 14,
                    "original_generic_interface_default": args.interface_default,
                    "inherited_default_graph": args.inherited_default,
                    "reference_input_scopes": args.reference_input,
                    "original_covariant_getter_chain": args.getter_chain,
                    "deferred_covariant_class": args.deferred_class,
                    "inherited_original_class_entries": args.inherited_entry,
                    "inherited_original_erased_bridge": args.inherited_bridge,
                    "original_class_bridge_override": args.class_bridge_override,
                    "full_class_and_interface_result_scopes": "retained",
                    "indexed_original_upcast_and_dispatch": "original forwarding matched" if args.class_bridge_override else "matched",
                    "original_inventory_and_concrete_harness_code": "unchanged", "wrong_class_formal": "rejected",
                    "reported_helpers_only": "verified", "omitted_interface_bridge": args.omit_interface_bridge,
                    "compiler_added_indexed_interface_entry": "not-required" if inherited_typed_only else "reported" if args.omit_interface_bridge else "original", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
