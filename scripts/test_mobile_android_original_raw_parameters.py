#!/usr/bin/env python3
"""Verify original raw parameter subsignatures, captured forwarding and inherited providers."""
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
    from scripts.test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_final_getter_return_slots import rename_owned_entry
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge
    from scripts.test_mobile_android_empty_original_bridge_throws import exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_final_getter_return_slots import rename_owned_entry
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge
    from test_mobile_android_empty_original_bridge_throws import exception_inventory


def hashes(directory: Path) -> dict[str, str]:
    return {p.relative_to(directory).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(directory.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-interface", action="store_true")
    parser.add_argument("--interface-fields", action="store_true")
    parser.add_argument("--dependency-parent-chain", action="store_true")
    parser.add_argument("--abstract-fixed-prefix", action="store_true")
    parser.add_argument("--abstract-dependency-parent", action="store_true")
    parser.add_argument("--plain-class-covariance", action="store_true")
    parser.add_argument("--covariant-siblings", action="store_true")
    parser.add_argument("--deferred-abstract-child", action="store_true")
    parser.add_argument("--dependency-method-formals", action="store_true")
    parser.add_argument("--raw-dependency-defaults", action="store_true")
    parser.add_argument("--scoped-nominal-parameters", action="store_true")
    args = parser.parse_args()
    if args.scoped_nominal_parameters and any((args.interface_fields, args.dependency_parent_chain,
            args.abstract_dependency_parent, args.plain_class_covariance, args.covariant_siblings,
            args.deferred_abstract_child, args.dependency_method_formals, args.raw_dependency_defaults)):
        parser.error("scoped nominal parameters use their separate source or dependency-interface profile")
    if args.raw_dependency_defaults and not args.dependency_method_formals:
        parser.error("raw SDK defaults require the method-owned dependency profile")
    if args.dependency_method_formals and (not args.dependency_interface
            or args.interface_fields or args.dependency_parent_chain or args.abstract_dependency_parent):
        parser.error("method-owned dependency formals require their separate unchanged dependency profile")
    if args.abstract_fixed_prefix and not args.dependency_parent_chain:
        parser.error("abstract fixed prefixes require the fixed dependency parent chain")
    if args.dependency_parent_chain and not args.dependency_interface:
        parser.error("dependency parent chains require the unchanged dependency profile")
    if args.covariant_siblings and not args.plain_class_covariance:
        parser.error("covariant siblings require the plain class covariance profile")
    if args.plain_class_covariance and not args.abstract_dependency_parent:
        parser.error("plain class covariance requires the abstract dependency parent profile")
    if args.deferred_abstract_child and (not args.abstract_dependency_parent or args.plain_class_covariance):
        parser.error("deferred abstract children require the separate abstract dependency parent profile")
    if args.abstract_dependency_parent and not args.dependency_interface:
        parser.error("abstract dependency parents require the unchanged dependency profile")
    if sum((args.interface_fields, args.dependency_parent_chain, args.abstract_dependency_parent)) > 1:
        parser.error("dependency fields, fixed prefixes and abstract parents use separate fixture profiles")
    if args.interface_fields and not args.dependency_interface:
        parser.error("interface fields require the unchanged dependency profile")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-original-raw-parameters-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "original_raw_parameters/java"
        originals, harness_dir = work / "originals", work / "harness"
        inputs = sorted(fixture.rglob("*.java"))
        if (args.interface_fields or args.dependency_parent_chain or args.scoped_nominal_parameters
                or args.abstract_dependency_parent or args.dependency_method_formals):
            authored = fixture
            fixture = work / "dependency-java"
            for source in inputs:
                content = source.read_text()
                if args.scoped_nominal_parameters:
                    edits = {
                        "Api.java": [("public interface Api {", "public interface Api<A> {"),
                                     ("accept(RawBox<?> box", "accept(RawBox<? super A> box")],
                        "Forwarder.java": [("public final class Forwarder implements Api",
                                            "public final class Forwarder<C,D> implements Api<RawBox<? extends C>>")],
                        "Leaf.java": [("public class Leaf extends Parent implements Api",
                                       "public class Leaf<C,D> extends Parent implements Api<RawBox<? extends C>>")],
                    }
                    for old, new in edits.get(source.name, []):
                        if content.count(old) != 1:
                            raise RuntimeError("Missing owned scoped nominal declaration")
                        content = content.replace(old, new, 1)
                if source.name == "Api.java" and args.interface_fields:
                    content = content.replace("public interface Api {", "public interface Api {\n"
                                              "    int FLAGS = 7;\n    String LABEL = \"owned-sdk\";\n"
                                              "    State STATE = new State();", 1)
                if source.name == "Forwarder.java" and (args.dependency_parent_chain or args.abstract_dependency_parent):
                    declaration = "public final class Forwarder implements Api"
                    if content.count(declaration) != 1 or not content.endswith("\n}\n"):
                        raise RuntimeError("Missing original raw forwarder class declaration")
                    parent = "AbstractWidget" if args.abstract_dependency_parent else "Widget"
                    content = content.replace(declaration,
                        "public final class Forwarder extends " + parent + " implements Api", 1)
                    if args.abstract_dependency_parent:
                        providers = "    protected void touch(Object marker) {\n" \
                            + "        touches++; seen = marker;\n" \
                            + "        if (failure != null) throw failure;\n    }\n" \
                            + "    public Object item() { return seen; }\n" \
                            + "    public <N> N identity(N value) { touches++; return value; }\n"
                    else:
                        providers = "    protected void touch(Object marker) { super.touch(marker); }\n"
                    content = content[:-3] + "\n" + providers \
                        + "    public void ping(Object marker) { touch(marker); }\n}\n"
                    if args.plain_class_covariance:
                        content = content.replace("public final class Forwarder extends", "public class Forwarder extends", 1)
                        constructor = "public Forwarder(Sink sink) { this.sink = sink; }"
                        if content.count(constructor) != 1:
                            raise RuntimeError("Missing owned covariant provider constructor")
                        content = content.replace(constructor,
                            "public Forwarder(Sink sink) { this.sink = sink; value = new Narrow(sink); }", 1)
                        content = content[:-3] + "\n    public final Narrow value;\n" \
                            + "    public Narrow makeValue(Object marker) {\n" \
                            + "        touches++; seen = marker;\n" \
                            + "        if (failure != null) throw failure;\n        return value;\n    }\n" \
                            + "    protected Narrow defaultValue() {\n" \
                            + "        touches++; if (failure != null) throw failure; return value;\n    }\n" \
                            + "    public Narrow defaultResult() { return defaultValue(); }\n}\n"
                if args.dependency_method_formals and source.name in {"Forwarder.java", "Parent.java"}:
                    if not content.endswith("\n}\n"):
                        raise RuntimeError("Missing owned method-formal provider anchor")
                    receiver = "sink." if source.name == "Forwarder.java" else ""
                    event = "4" if source.name == "Forwarder.java" else "7"
                    content = content[:-3] + "\n    public <N extends Mark> N select(RawBox<N> box) {\n" \
                        + "        Trace.events = Trace.events * 10 + " + event + ";\n" \
                        + "        " + receiver + "calls++; " + receiver + "seen = box;\n" \
                        + "        if (" + receiver + "failure != null) throw " + receiver + "failure;\n" \
                        + "        return box == null ? null : box.value;\n    }\n}\n"
                destination = fixture / source.relative_to(authored)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
            if args.interface_fields:
                (fixture / "fixture/State.java").write_text(
                    "package fixture;\npublic final class State {\n"
                    "    public static int starts;\n    public int marker;\n"
                    "    public State() { starts++; marker = 7; }\n}\n")
            if args.dependency_parent_chain:
                for source in sorted((FIXTURES / "fixed_dependency_class_prefix/java/fixture").glob("*.java")):
                    shutil.copyfile(source, fixture / "fixture" / source.name)
                if args.abstract_fixed_prefix:
                    for source in sorted((FIXTURES / "abstract_fixed_dependency_prefix/java/fixture").glob("*.java")):
                        shutil.copyfile(source, fixture / "fixture" / source.name)
            if args.abstract_dependency_parent:
                shutil.copyfile(FIXTURES / "fixed_dependency_class_prefix/java/fixture/Root.java",
                                fixture / "fixture/Root.java")
                shutil.copyfile(FIXTURES / "abstract_dependency_parent/java/fixture/AbstractWidget.java",
                                fixture / "fixture/AbstractWidget.java")
                if args.plain_class_covariance:
                    for source in sorted((FIXTURES / "plain_class_covariance/java/fixture").glob("*.java")):
                        shutil.copyfile(source, fixture / "fixture" / source.name)
                    parent = fixture / "fixture/AbstractWidget.java"
                    content = parent.read_text()
                    if not content.endswith("\n}\n"):
                        raise RuntimeError("Missing owned covariant SDK parent anchor")
                    parent.write_text(content[:-3] + "\n    public abstract Value makeValue(Object marker);\n"
                        + "    protected Value defaultValue() { touches += 2; return null; }\n}\n")
            if args.covariant_siblings:
                shutil.copyfile(FIXTURES / "covariant_class_siblings/java/fixture/Broad.java",
                                fixture / "fixture/Broad.java")
                value = fixture / "fixture/Value.java"
                content = value.read_text()
                assert content.count("public class Value {") == 1
                value.write_text(content.replace("public class Value {", "public class Value extends Broad {"))
                root = fixture / "fixture/Root.java"
                content = root.read_text()
                assert content.endswith("\n}\n")
                root.write_text(content[:-3] + "\n    public Broad makeValue(Object marker) { return null; }\n"
                                + "    protected Broad defaultValue() { return null; }\n}\n")
            if args.deferred_abstract_child:
                for source in sorted((FIXTURES / "plain_deferred_abstract/java/fixture").glob("*.java")):
                    shutil.copyfile(source, fixture / "fixture" / source.name)
            if args.dependency_method_formals:
                for source in sorted((FIXTURES / "dependency_method_formals/java/fixture").glob("*.java")):
                    shutil.copyfile(source, fixture / "fixture" / source.name)
                if args.raw_dependency_defaults:
                    api = fixture / "fixture/Api.java"
                    content = api.read_text()
                    content = content.replace("public interface Api {", "public interface Api<C> {", 1)
                    content = content.replace("    <M extends Mark> M select(RawBox<M> box);\n", "", 1)
                    content = content.replace("return select(box);", "return box == null ? null : box.value;", 1)
                    api.write_text(content)
            inputs = sorted(fixture.rglob("*.java"))
        verify.compile_local(inputs, originals)
        if args.covariant_siblings:
            # Model an abstract middle declaration without its own broader
            # compiler slots. Keep both original concrete child bridges.
            edits = [omit_owned_interface_bridge(originals / "fixture/AbstractWidget.class", identity,
                                                  original_flags=flags)
                     for identity, flags in ((b"makeValue(Ljava/lang/Object;)Lfixture/Broad;", 0x1041),
                                             (b"defaultValue()Lfixture/Broad;", 0x1044))]
            (work / "middle-bridge-encoding.json").write_text(json.dumps(edits, indent=2) + "\n")
        original_hashes = hashes(originals)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        original_exceptions = exception_inventory(originals)
        records = class_identity.compiler_classes(originals)
        classes, methods = compiler_inventory(verify, originals)
        dependency = work / "owned-raw-parameter-contract.jar"
        declarations = {"Lfixture/Api;"}
        if args.dependency_method_formals:
            declarations.add("Lfixture/Mark;")
        if args.interface_fields:
            declarations.add("Lfixture/State;")
        if args.dependency_parent_chain:
            declarations.update({"Lfixture/Root;", "Lfixture/Base;", "Lfixture/Fixed;", "Lfixture/Widget;"})
            if args.abstract_fixed_prefix:
                declarations.add("Lfixture/ConcreteWidget;")
        if args.abstract_dependency_parent:
            declarations.update({"Lfixture/Root;", "Lfixture/AbstractWidget;"})
        if args.plain_class_covariance:
            declarations.add("Lfixture/Value;")
        if args.covariant_siblings:
            declarations.add("Lfixture/Broad;")
        with zipfile.ZipFile(dependency, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for owner in sorted(declarations):
                name = owner[1:-1] + ".class"
                archive.write(originals / name, name)
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        if args.dependency_interface:
            classes -= declarations
            methods = {key: row for key, row in methods.items() if key.split("->", 1)[0] in classes}
        classpath = ["--classpath", dependency] if args.dependency_interface else []
        harness = ("ScopedRawParametersHarness" if args.scoped_nominal_parameters else
                   "RawDependencyDefaultsHarness" if args.raw_dependency_defaults else
                   "DependencyMethodFormalsHarness" if args.dependency_method_formals else
                   "PlainDeferredAbstractHarness" if args.deferred_abstract_child else
                   "CovariantClassSiblingsHarness" if args.covariant_siblings else
                   "PlainClassCovarianceHarness" if args.plain_class_covariance else
                   "AbstractDependencyParentHarness" if args.abstract_dependency_parent else
                   "AbstractFixedDependencyPrefixHarness" if args.abstract_fixed_prefix else
                   "FixedDependencyClassPrefixHarness" if args.dependency_parent_chain else
                   "SdkInterfaceFieldsHarness" if args.interface_fields else "OriginalRawParametersHarness")
        harness_inputs = [FIXTURES / "harness/OriginalRawParametersHarness.java"]
        if args.scoped_nominal_parameters:
            harness_inputs.append(FIXTURES / "harness/ScopedRawParametersHarness.java")
        if args.interface_fields:
            harness_inputs.append(FIXTURES / "harness/SdkInterfaceFieldsHarness.java")
        if args.dependency_parent_chain:
            harness_inputs.append(FIXTURES / "harness" / ("AbstractFixedDependencyPrefixHarness.java"
                if args.abstract_fixed_prefix else "FixedDependencyClassPrefixHarness.java"))
        if args.abstract_dependency_parent and not args.plain_class_covariance:
            harness_inputs.append(FIXTURES / "harness/AbstractDependencyParentHarness.java")
        if args.plain_class_covariance:
            harness_inputs.append(FIXTURES / "harness/PlainClassCovarianceHarness.java")
        if args.covariant_siblings:
            harness_inputs.append(FIXTURES / "harness/CovariantClassSiblingsHarness.java")
        if args.deferred_abstract_child:
            harness_inputs.append(FIXTURES / "harness/PlainDeferredAbstractHarness.java")
        if args.dependency_method_formals:
            harness_inputs.append(FIXTURES / "harness" / ("RawDependencyDefaultsHarness.java"
                if args.raw_dependency_defaults else "DependencyMethodFormalsHarness.java"))
        verify.compile_local(harness_inputs, harness_dir, classpath=originals)
        harness_hashes = hashes(harness_dir)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness_dir))),
                               harness], "baseline")
        expected = ["values=36,identities=444,states=156,failures=20,bridges=0",
                    "original-raw-parameters-and-default-free-ownership=retained,capture-and-inherited-dispatch=matched"]
        if args.interface_fields:
            expected.append("fields=3,constant-values=2,initializations=1,mutable-sdk-state=matched")
        if args.dependency_parent_chain:
            expected.extend(["sdk-identities=17,sdk-states=7,sdk-failures=3,original-sdk-bridges=2,source-bridges=0",
                             "fixed-sdk-class-edges=retained,protected-source-and-sdk-virtual-dispatch=matched"])
        if args.abstract_dependency_parent and not args.plain_class_covariance:
            expected.extend(["abstract-sdk-identities=18,abstract-sdk-states=9,abstract-sdk-failures=3,source-bridges=0",
                             "abstract-sdk-requirements-and-original-provider-ownership=retained,method-formals-and-virtual-dispatch=matched"])
        if args.plain_class_covariance:
            expected.extend(["plain-class-covariant-values=48,identities=112,states=64,failures=24,original-source-bridges="
                             + ("4" if args.covariant_siblings else "2"),
                             "plain-class-covariance-and-full-metadata=retained,public-protected-and-subclass-dispatch=matched"])
        if args.covariant_siblings:
            expected.extend(["sibling-class-covariant-values=48,identities=112,states=64,failures=24",
                             "original-sibling-bridges-and-full-metadata=retained,root-and-subclass-dispatch=matched"])
        if args.deferred_abstract_child:
            expected.extend(["plain-deferred-abstract-values=18,identities=128,states=80,failures=16,bridges=0",
                             "nearest-abstract-declarations-and-full-method-scopes=retained,independent-concrete-subclass-dispatch=matched"])
        if args.raw_dependency_defaults:
            expected.extend(["raw-sdk-default-identities=24,states=24,failures=6,source-bridges=0",
                             "raw-default-view-and-original-sdk-metadata=retained,default-ownership-and-failure-order=matched"])
        elif args.dependency_method_formals:
            expected.extend(["dependency-method-formal-identities=252,states=90,failures=30,bridges=0",
                             "complete-sdk-method-scopes-and-default-ownership=retained,virtual-dispatch-and-mutable-aliases=matched"])
        if args.scoped_nominal_parameters:
            expected.extend(["scoped-values=24,identities=114,states=66,failures=12,bridges=0",
                             "complete-scoped-nominal-parameters-and-original-raw-methods=retained"])
        if baseline.splitlines() != expected:
            raise RuntimeError("Original raw parameter baseline missed expected identities, effects or failures")

        def make_dex(directory: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath",
                        args.android_jar.resolve(), *classpath, "--output", destination,
                        *[p for p in sorted(directory.rglob("*.class"))
                          if "L" + p.relative_to(directory).as_posix()[:-6] + ";" in classes]],
                       "d8-" + destination.name)
            dex = destination / "classes.dex"
            if not dex.is_file() or list(destination.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Original raw parameter fixture did not retain one original DEX")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed original original raw parameter fixture ownership")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        projected = 4 if args.covariant_siblings else 2 if args.plain_class_covariance else 0
        expected_status = "partial" if args.plain_class_covariance else "recovered"
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage.get("projected_method_count", 0) != projected or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != expected_status or coverage.get("generic_signature_projections")):
            raise RuntimeError("Original raw parameter recovery copied a method or projected generic metadata")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_interface else None,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("Recompile changed original original raw parameter runtime entries")
        after = class_identity.compiler_classes(rebuilt)
        rebuilt_exceptions = exception_inventory(rebuilt)
        for method in methods:
            if rebuilt_exceptions.get(method) != original_exceptions[method]:
                raise RuntimeError("Original raw parameter Exceptions metadata changed")
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                if records[owner].get(key) != after[owner].get(key):
                    raise RuntimeError("Original raw parameter class/field metadata changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                if after[owner]["methods"][identity] != metadata:
                    raise RuntimeError("Complete original raw parameter method metadata changed: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness_dir, dependency))),
                             harness], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Original raw parameter methods changed reference identity, state or failure order")
        cases = [
            ("direct-typed", "Forwarder", 17, "RawBox<String>"),
            ("inherited-typed", "Parent", 1, "RawBox<Object>"),
        ]
        for name, owner, flags, typed in cases:
            negative = work / (name + "-originals")
            shutil.copytree(originals, negative)
            argument = "Ljava/lang/String;" if owner == "Forwarder" else "Ljava/lang/Object;"
            signature = "(Lfixture/RawBox<" + argument + ">;Ljava/lang/Object;)V"
            receipt = add_parameter_signature(negative / ("fixture/" + owner + ".class"), signature,
                                              method_name="accept", prototype="(Lfixture/RawBox;Ljava/lang/Object;)V",
                                              original_flags=flags)
            (work / (name + "-same-code.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = make_dex(negative, work / (name + "-dex"))
            rejected = work / (name + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                     str(args.android_jar.resolve()), *map(str, classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (name + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Equal erased descriptors hid different complete raw parameter scopes: " + name)
            compiler_inputs = []
            for source in inputs:
                content = source.read_text()
                if source.name == owner + ".java":
                    declaration = "accept(RawBox box, Object tag)"
                    if content.count(declaration) != 1:
                        raise RuntimeError("Missing independent raw parameter counterexample")
                    content = content.replace(declaration, "accept(" + typed + " box, Object tag)", 1)
                destination = work / (name + "-java") / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                compiler_inputs.append(str(destination))
            compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                       str(work / (name + "-compiler")), *compiler_inputs],
                                      text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (name + "-compiler.log")).write_text(compiler.stdout + compiler.stderr)
            if compiler.returncode == 0 or "does not override" not in compiler.stderr:
                raise RuntimeError("Independent javac accepted different raw parameter scope: " + name)
        if args.dependency_method_formals and not args.raw_dependency_defaults:
            negative = work / "method-scope-originals"
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Forwarder.class",
                b"<N::Lfixture/Mark;>(Lfixture/RawBox<TN;>;)TN;",
                b"<N::Lfixture/Mark;>(Lfixture/RawBox<Lfixture/Mark;>;)TN;")
            (work / "method-scope-same-code.json").write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = make_dex(negative, work / "method-scope-dex")
            rejected = work / "method-scope-rejected"
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                str(args.android_jar.resolve()), *map(str, classpath), "-o", str(rejected)],
                text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "method-scope-recovery.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Same-erasure SDK parameter lost its original method scope")
            compiler_inputs = []
            for source in inputs:
                content = source.read_text()
                if source.name == "Forwarder.java":
                    declaration = "N select(RawBox<N> box)"
                    if content.count(declaration) != 1:
                        raise RuntimeError("Missing independently authored SDK method scope")
                    content = content.replace(declaration, "N select(RawBox<Mark> box)", 1)
                destination = work / "method-scope-java" / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                compiler_inputs.append(str(destination))
            compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                str(work / "method-scope-compiler"), *compiler_inputs], text=True, capture_output=True,
                timeout=300, env=verify.environment)
            (work / "method-scope-compiler.log").write_text(compiler.stdout + compiler.stderr)
            if compiler.returncode == 0 or "does not override" not in compiler.stderr:
                raise RuntimeError("Independent javac accepted different SDK method parameter scope")
        if args.raw_dependency_defaults:
            negative = work / "raw-default-override-originals"
            shutil.copytree(originals, negative)
            receipt = rename_owned_entry(negative / "fixture/Forwarder.class", b"select", b"inherited")
            (work / "raw-default-override-same-code.json").write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = make_dex(negative, work / "raw-default-override-dex")
            rejected = work / "raw-default-override-rejected"
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                str(args.android_jar.resolve()), *map(str, classpath), "-o", str(rejected)],
                text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "raw-default-override-recovery.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Generic override silently acquired a raw SDK default slot")
            compiler_inputs = []
            for source in inputs:
                content = source.read_text()
                if source.name == "Forwarder.java":
                    declaration = "N select(RawBox<N> box)"
                    if content.count(declaration) != 1:
                        raise RuntimeError("Missing independently authored raw SDK override")
                    content = content.replace(declaration, "N inherited(RawBox<N> box)", 1)
                destination = work / "raw-default-override-java" / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                compiler_inputs.append(str(destination))
            compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                str(work / "raw-default-override-compiler"), *compiler_inputs], text=True, capture_output=True,
                timeout=300, env=verify.environment)
            (work / "raw-default-override-compiler.log").write_text(compiler.stdout + compiler.stderr)
            if compiler.returncode == 0 or "name clash" not in compiler.stderr:
                raise RuntimeError("Independent javac accepted a scoped override of a raw SDK default")
        if args.dependency_parent_chain:
            negative = work / "ancestor-arity-originals"
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Fixed.class",
                b"Lfixture/Base<Ljava/lang/String;>;",
                b"Lfixture/Base<Ljava/lang/String;Ljava/lang/String;>;")
            (work / "ancestor-arity-same-code.json").write_text(json.dumps(receipt, indent=2) + "\n")
            changed_dependency = work / "ancestor-arity-dependency.jar"
            with zipfile.ZipFile(dependency) as original, zipfile.ZipFile(
                    changed_dependency, "x", compression=zipfile.ZIP_DEFLATED) as altered:
                for entry in original.namelist():
                    altered.writestr(entry, (negative / entry).read_bytes()
                        if entry == "fixture/Fixed.class" else original.read(entry))
            rejected = work / "ancestor-arity-rejected"
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(dex), "--classpath",
                str(args.android_jar.resolve()), "--classpath", str(changed_dependency), "-o", str(rejected)],
                text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "ancestor-arity-recovery.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Incomplete fixed dependency class argument escaped the common validator")
            compiler_inputs = []
            for source in inputs:
                content = source.read_text()
                if source.name == "Fixed.java":
                    if content.count("extends Base<String>") != 1:
                        raise RuntimeError("Missing independently authored fixed class argument")
                    content = content.replace("extends Base<String>", "extends Base<String, String>", 1)
                destination = work / "ancestor-arity-java" / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                compiler_inputs.append(str(destination))
            compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                str(work / "ancestor-arity-compiler"), *compiler_inputs], text=True, capture_output=True,
                timeout=300, env=verify.environment)
            (work / "ancestor-arity-compiler.log").write_text(compiler.stdout + compiler.stderr)
            if compiler.returncode == 0 or "wrong number of type arguments" not in compiler.stderr:
                raise RuntimeError("Independent javac accepted malformed fixed SDK class arity")
        if args.abstract_dependency_parent:
            negative = work / "abstract-provider-formal-originals"
            shutil.copytree(originals, negative)
            receipt = add_parameter_signature(negative / "fixture/Forwarder.class",
                "<M:Ljava/lang/Object;>(TM;)V", method_name="touch",
                prototype="(Ljava/lang/Object;)V", original_flags=4)
            (work / "abstract-provider-formal-same-code.json").write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = make_dex(negative, work / "abstract-provider-formal-dex")
            rejected = work / "abstract-provider-formal-rejected"
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                str(args.android_jar.resolve()), *map(str, classpath), "-o", str(rejected)],
                text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "abstract-provider-formal-recovery.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Changed complete abstract provider formal escaped inheritance validation")
            compiler_inputs = []
            for source in inputs:
                content = source.read_text()
                if source.name == "Forwarder.java":
                    declaration = "protected void touch(Object marker)"
                    if content.count(declaration) != 1:
                        raise RuntimeError("Missing independently authored abstract callback provider")
                    content = content.replace(declaration, "protected <M> void touch(M marker)", 1)
                destination = work / "abstract-provider-formal-java" / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                compiler_inputs.append(str(destination))
            compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                str(work / "abstract-provider-formal-compiler"), *compiler_inputs], text=True, capture_output=True,
                timeout=300, env=verify.environment)
            (work / "abstract-provider-formal-compiler.log").write_text(compiler.stdout + compiler.stderr)
            if compiler.returncode == 0 or not any(message in compiler.stderr
                    for message in ("does not override", "name clash")):
                raise RuntimeError("Independent javac accepted incompatible abstract provider formal")
        if args.deferred_abstract_child:
            negative = work / "deferred-abstract-formal-originals"
            shutil.copytree(originals, negative)
            receipt = add_parameter_signature(negative / "fixture/Redeclared.class",
                "<M:Ljava/lang/Object;>(TM;)V", method_name="touch",
                prototype="(Ljava/lang/Object;)V", original_flags=0x404, code_count=0)
            (work / "deferred-abstract-formal-same-code.json").write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = make_dex(negative, work / "deferred-abstract-formal-dex")
            rejected = work / "deferred-abstract-formal-rejected"
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                str(args.android_jar.resolve()), *map(str, classpath), "-o", str(rejected)],
                text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "deferred-abstract-formal-recovery.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Changed complete abstract redeclaration formal escaped inheritance validation")
            compiler_inputs = []
            for source in inputs:
                content = source.read_text()
                if source.name == "Redeclared.java":
                    declaration = "protected abstract void touch(Object marker)"
                    if content.count(declaration) != 1:
                        raise RuntimeError("Missing original abstract redeclaration")
                    content = content.replace(declaration, "protected abstract <M> void touch(M marker)", 1)
                destination = work / "deferred-abstract-formal-java" / source.relative_to(fixture)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                compiler_inputs.append(str(destination))
            compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                str(work / "deferred-abstract-formal-compiler"), *compiler_inputs], text=True, capture_output=True,
                timeout=300, env=verify.environment)
            (work / "deferred-abstract-formal-compiler.log").write_text(compiler.stdout + compiler.stderr)
            if compiler.returncode == 0 or not any(message in compiler.stderr
                    for message in ("does not override", "name clash")):
                raise RuntimeError("Independent javac accepted incompatible abstract redeclaration formal")
        if args.covariant_siblings:
            for name, flags in (("ordinary-sibling", 0x1), ("private-sibling", 0x1042)):
                negative = work / (name + "-originals")
                shutil.copytree(originals, negative)
                receipt = change_method_flags(negative / "fixture/Forwarder.class",
                    b"(Ljava/lang/Object;)Lfixture/Broad;", 0x1041, flags, name=b"makeValue")
                (work / (name + "-same-code.json")).write_text(json.dumps(receipt, indent=2) + "\n")
                negative_dex = make_dex(negative, work / (name + "-dex"))
                rejected = work / (name + "-rejected")
                result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                    "--classpath", str(args.android_jar.resolve()), *map(str, classpath), "-o", str(rejected)],
                    capture_output=True, text=True, timeout=300, env=verify.environment)
                (work / (name + "-recovery.log")).write_text(result.stdout + result.stderr)
                if result.returncode == 0 or rejected.exists():
                    raise RuntimeError("Sibling return slot lost its original public bridge proof: " + name)
        if args.abstract_fixed_prefix:
            for label, declaration in (("new-abstract", "public abstract void missing();"),
                                       ("reabstracted", "public abstract String item();")):
                altered_source = work / (label + "-sdk-source/fixture/Widget.java")
                altered_source.parent.mkdir(parents=True)
                altered_source.write_text("package fixture;\npublic abstract class Widget extends ConcreteWidget { "
                                          + declaration + " }\n")
                altered_classes = work / (label + "-sdk-classes")
                verify.compile_local([altered_source], altered_classes, classpath=originals)
                altered_jar = work / (label + "-sdk.jar")
                with zipfile.ZipFile(dependency) as original, zipfile.ZipFile(altered_jar, "x") as altered:
                    for entry in original.namelist():
                        altered.writestr(entry, (altered_classes / entry).read_bytes()
                            if entry == "fixture/Widget.class" else original.read(entry))
                rejected = work / (label + "-rejected")
                result = subprocess.run([str(args.neverd.resolve()), "mobile", str(dex), "--classpath",
                    str(args.android_jar.resolve()), "--classpath", str(altered_jar), "-o", str(rejected)],
                    text=True, capture_output=True, timeout=300, env=verify.environment)
                (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
                if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                    raise RuntimeError("Abstract fixed prefix hid a new SDK obligation: " + label)
                compiler_inputs = [str(altered_source) if source.name == "Widget.java" else str(source)
                                   for source in inputs]
                compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                    str(work / (label + "-compiler")), *compiler_inputs], text=True, capture_output=True,
                    timeout=300, env=verify.environment)
                (work / (label + "-compiler.log")).write_text(compiler.stdout + compiler.stderr)
                if compiler.returncode == 0 or "does not override" not in compiler.stderr:
                    raise RuntimeError("Independent javac accepted the missing SDK implementation: " + label)
        if (hashes(originals) != original_hashes or hashes(harness_dir) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original raw parameter input, immutable JVM harness or dependency changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 36, "identity_cases": 444, "state_observations": 156,
                    "ordered_failure_cases": 20, "bridge_count": 0, "coverage": "recovered",
                    "original_raw_parameter_signatures": "unchanged", "generic_metadata": "unchanged",
                    "capture_forwarding_and_inherited_dispatch": "matched", "jvm_output": "matched",
                    "typed_counterexamples": "same Code and descriptors; rejected by NeverD and javac",
                    "declaration_profile": "unchanged JAR" if args.dependency_interface else "input interface"}
        if args.scoped_nominal_parameters:
            evidence["scoped_nominal_parameters"] = "original class variables inside complete nested wildcard interface arguments; unchanged raw methods and zero bridges"
            evidence["scoped_dispatch"] = "24 additional values, 114 identities, 66 states and 12 failures through typed callers, reflection and MethodHandles"
        if args.interface_fields:
            evidence["dependency_fields"] = "three original public static final fields; no source copies"
            evidence["constant_values_and_initialization"] = "two values, one initialization and mutable alias identity matched"
        if args.raw_dependency_defaults:
            evidence["raw_dependency_defaults"] = "original raw default view; unchanged complete SDK class/method scopes"
            evidence["raw_default_dispatch"] = "24 identities, 24 state observations and six ordered failures; zero source bridges"
            evidence["raw_default_override_counterexample"] = "same Code and descriptors; rejected by NeverD and independent javac"
        elif args.dependency_method_formals:
            evidence["dependency_method_formals"] = "complete original abstract/default method scopes and owner metadata retained in unchanged JAR"
            evidence["method_formal_dispatch"] = "252 identities, 90 state observations and 30 ordered failures; zero bridges"
            evidence["method_scope_counterexample"] = "same Code and descriptors; rejected by NeverD and independent javac"
        if args.dependency_parent_chain:
            evidence["fixed_dependency_class_prefix"] = ("five" if args.abstract_fixed_prefix else "four") + " original SDK classes and all member metadata retained in unchanged JAR"
            if args.abstract_fixed_prefix:
                evidence["abstract_fixed_prefix"] = "original abstract class introduces no obligation above its concrete generic provider"
                evidence["abstract_obligation_counterexamples"] = "unchanged input DEX rejects new and reabstracted SDK members; independent javac agrees"
            evidence["sdk_dispatch"] = "17 identities, seven state observations, three ordered failures, two original SDK bridges and zero source bridges"
            evidence["ancestor_arity_counterexample"] = "same Code and descriptors; rejected by NeverD and javac"
        if args.abstract_dependency_parent and not args.plain_class_covariance:
            evidence["abstract_dependency_parent"] = "two SDK classes remain unchanged; nearest original source providers retained"
            evidence["abstract_sdk_dispatch"] = "18 identities, nine state observations, three ordered failures and zero source bridges"
            evidence["abstract_provider_formal_counterexample"] = "same Code and descriptors; rejected by NeverD and javac"
        if args.plain_class_covariance:
            evidence["coverage"] = expected_status
            evidence["bridge_count"] = projected
            evidence["plain_class_covariance"] = str(projected) + " original public/protected bridges regenerated with unchanged complete metadata"
            evidence["covariant_dispatch"] = "48 values, 112 identities, 64 state observations and 24 ordered failures matched"
            evidence["bridge_reporting"] = "original runtime entries retained; source projection remains disclosed as partial"
        if args.covariant_siblings:
            evidence["covariant_siblings"] = "four original public/protected bridges to two ancestor return slots; exact original runtime inventory and metadata"
            evidence["sibling_dispatch"] = "48 additional values, 112 identities, 64 state observations and 24 ordered failures; original subclass, reflection and method handles"
        if args.deferred_abstract_child:
            evidence["plain_deferred_abstract"] = "nearest inherited and redeclared abstract members retained with exact complete original metadata"
            evidence["deferred_abstract_dispatch"] = "18 values, 128 identities, 80 state observations and 16 ordered failures matched"
            evidence["abstract_redeclaration_formal_counterexample"] = "unchanged absent Code and descriptors; rejected by NeverD and independent javac"
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
