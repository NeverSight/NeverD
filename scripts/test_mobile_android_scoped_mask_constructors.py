#!/usr/bin/env python3
"""Verify genuine Kotlin scoped mask constructors retain default selection and raw ABI."""
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import KotlinFixtureClassFile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--kotlin-lib", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--kotlin-java-home", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-defaults", action="store_true")
    parser.add_argument("--fixed-nominal", action="store_true")
    parser.add_argument("--package-subclass", action="store_true")
    parser.add_argument("--register-mask", action="store_true")
    parser.add_argument("--closed-parent", action="store_true")
    parser.add_argument("--star-bounded", action="store_true")
    parser.add_argument("--allocated-default", action="store_true")
    parser.add_argument("--private-target", action="store_true")
    parser.add_argument("--scoped-parent-chain", action="store_true")
    parser.add_argument("--scoped-interfaces", action="store_true")
    args = parser.parse_args()
    if args.scoped_interfaces and any((args.fixed_nominal, args.package_subclass, args.register_mask, args.closed_parent, args.star_bounded, args.allocated_default, args.private_target, args.scoped_parent_chain)):
        parser.error("--scoped-interfaces requires the original generic owner")
    if args.scoped_parent_chain and any((args.fixed_nominal,args.package_subclass,args.register_mask,args.closed_parent,args.star_bounded,args.allocated_default,args.private_target)):
        parser.error("--scoped-parent-chain requires the original generic owner")
    if args.private_target and not args.fixed_nominal:
        parser.error("--private-target requires --fixed-nominal")
    if args.allocated_default and (args.fixed_nominal or args.package_subclass or args.register_mask or args.closed_parent or args.star_bounded):
        parser.error("--allocated-default requires the original generic owner")
    if args.star_bounded and (not args.fixed_nominal or args.package_subclass):
        parser.error("--star-bounded requires --fixed-nominal and excludes --package-subclass")
    if args.closed_parent and (not args.package_subclass or args.register_mask):
        parser.error("--closed-parent requires --package-subclass and excludes --register-mask")
    if args.register_mask and not args.package_subclass:
        parser.error("--register-mask requires --package-subclass")
    if args.package_subclass and not args.fixed_nominal:
        parser.error("--package-subclass requires --fixed-nominal")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-scoped-mask-constructors-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "scoped_mask_constructors"
        if args.scoped_parent_chain:
            authored=work/"authored-fixture";shutil.copytree(fixture,authored);fixture=authored
            core=fixture/"kotlin/fixture/Core.kt";text=core.read_text();anchor="= Defaults.CB) {"
            if text.count(anchor)!=1:raise RuntimeError("Original scoped parent anchor changed")
            core.write_text(text.replace(anchor,"= Defaults.CB) : Middle<T>() {"))
            (fixture/"java/fixture/Base.java").open("x").write("package fixture;\npublic class Base<A> { public Base() {} }\n")
            (fixture/"java/fixture/Middle.java").open("x").write("package fixture;\npublic class Middle<B> extends Base<B> { public Middle() {} }\n")
        if args.scoped_interfaces:
            authored = work / "authored-fixture"
            shutil.copytree(fixture, authored)
            fixture = authored
            core = fixture / "kotlin/fixture/Core.kt"
            text = core.read_text()
            anchor = "= Defaults.CB) {"
            if text.count(anchor) != 1:
                raise RuntimeError("Original scoped interface anchor changed")
            core.write_text(text.replace(anchor, "= Defaults.CB) : Left<T>, Right<T>, java.io.Serializable {"))
            for name, parent in (("Root", ""), ("Left", " extends Root<T>"), ("Right", " extends Root<T>")):
                (fixture / "java/fixture" / (name + ".java")).open("x").write(
                    "package fixture;\npublic interface " + name + "<T>" + parent + " {}\n")
        if args.allocated_default:
            authored = work / "authored-fixture"
            shutil.copytree(fixture, authored)
            fixture = authored
            core = fixture / "kotlin/fixture/Core.kt"
            text = core.read_text()
            if text.count("= Defaults.CB") != 1:
                raise RuntimeError("Original allocating mask source anchor changed")
            core.write_text(text.replace("= Defaults.CB", "= Defaults()"))
            (fixture / "java/fixture/Defaults.java").write_text(
                "package fixture;\npublic class Defaults implements Sink<Object> {\n"
                "    static { Trace.hit(2); }\n"
                "    public Defaults() { Trace.hit(3); }\n"
                "    public void accept(Object value) { Trace.seen = value; Trace.hit(5); }\n}\n")
        if args.fixed_nominal:
            authored = work / "authored-fixture"
            shutil.copytree(fixture, authored)
            fixture = authored
            core = fixture / "kotlin/fixture/Core.kt"
            text = core.read_text()
            if text.count("class Core<T>") != 1 or text.count("Sink<in T>") != 1:
                raise RuntimeError("Original fixed nominal constructor source anchors changed")
            core.write_text(text.replace("class Core<T>", "class Core").replace("Sink<in T>", "Sink<in Trace>"))
            if args.package_subclass:
                text = core.read_text()
                marker = "= Defaults.CB) {"
                if text.count(marker) != 1:
                    raise RuntimeError("Original plain subclass source anchor changed")
                core.write_text(text.replace(marker, "= Defaults.CB) : Parent() {"))
                (fixture / "java/fixture/Parent.java").write_text("package fixture;\npublic class Parent implements Root, ParentAPI { public Parent() { Trace.hit(6); } }\n")
                (fixture / "java/fixture/Root.java").write_text("package fixture;\npublic interface Root {}\n")
                (fixture / "java/fixture/ParentAPI.java").write_text("package fixture;\npublic interface ParentAPI extends Root {}\n")
                if args.register_mask:
                    text = core.read_text().replace("val count: Int", "val count: Boolean = true")
                    text = text.replace(": Parent() {", ": Parent(), ParentAPI {")
                    text = text.replace("capacity: Int = 7", "capacity: Int")
                    text = text.replace("Sink<in Trace> = Defaults.CB", "Sink<in Trace>")
                    core.write_text(text)


        if args.closed_parent:
            core.write_text(core.read_text().replace(": Parent() {", ": Parent<Trace>() {"))
            parent = fixture / "java/fixture/Parent.java"
            parent.write_text(parent.read_text().replace("class Parent implements", "class Parent<T extends Trace> implements"))

        if args.star_bounded:
            core.write_text(core.read_text().replace("Sink<in Trace>", "Sink<*>"))
            for name in ("Sink.java", "Consumer.java", "Defaults.java"):
                source = fixture / "java/fixture" / name
                text = source.read_text().replace("Sink<T>", "Sink<T extends Trace>")
                text = text.replace("Sink<Object>", "Sink<Trace>").replace("accept(Object value)", "accept(Trace value)")
                source.write_text(text)

        if args.private_target:
            text = core.read_text()
            if text.count("class Core(") != 1:
                raise RuntimeError("Original private constructor source anchor changed")
            core.write_text(text.replace("class Core(", "class Core private constructor("))

        original_java, originals, harness = work / "original-java", work / "originals", work / "harness"
        kotlin = args.kotlin_lib.resolve()
        compiler_jars = sorted(kotlin.glob("*.jar"))
        if not (kotlin / "kotlin-compiler.jar").is_file() or not (kotlin / "kotlin-stdlib.jar").is_file():
            raise RuntimeError("The witness requires the unmodified Kotlin compiler distribution")
        inputs = [args.android_jar.resolve(), *compiler_jars]
        input_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        stdlib = kotlin / "kotlin-stdlib.jar"
        dependencies = []
        for jar in (stdlib, kotlin / "annotations-13.0.jar"):
            output = work / jar.name
            with zipfile.ZipFile(jar) as source, zipfile.ZipFile(output, "x") as dest:
                for entry in source.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):
                            raise RuntimeError("Unsafe compiler dependency entry")
                        dest.writestr(entry.filename, source.read(entry))
            dependencies.append(output)
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), original_java, release=8)
        originals.mkdir()
        compiler = [choose_jdk(args.kotlin_java_home) / "bin/java", "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (stdlib, original_java))),
                    "-jvm-target", "1.8"]
        verify.run([*compiler, "-d", originals, *sorted((fixture / "kotlin").rglob("*.kt"))], "kotlinc-original")
        for path in sorted(original_java.rglob("*.class")):
            destination = originals / path.relative_to(original_java)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
        if args.package_subclass:
            # Genuine K2 mask Code and every descriptor stay byte-identical.
            # Only remove the class public bit to model a JVM package class.
            core_class = originals / "fixture/Core.class"
            before = core_class.read_bytes()
            data = bytearray(before)
            at, count = 10, int.from_bytes(data[8:10], "big")
            index = 1
            while index < count:
                tag = data[at]; at += 1
                if tag == 1:
                    size = int.from_bytes(data[at:at+2], "big"); at += 2 + size
                elif tag in (3, 4): at += 4
                elif tag in (5, 6): at += 8; index += 1
                elif tag in (7, 8, 16, 19, 20): at += 2
                elif tag in (9, 10, 11, 12, 17, 18): at += 4
                elif tag == 15: at += 3
                else: raise RuntimeError("Unexpected original K2 constant-pool tag")
                index += 1
            flags = int.from_bytes(data[at:at+2], "big")
            if not flags & 1: raise RuntimeError("Original K2 class visibility changed")
            data[at:at+2] = (flags & ~1).to_bytes(2, "big")
            if before[:at] != data[:at] or before[at+2:] != data[at+2:]:
                raise RuntimeError("Package class patch changed Code, descriptors or metadata")
            core_class.write_bytes(data)
            (work / "class-visibility-witness.json").write_text(json.dumps({
                "original_sha256": hashlib.sha256(before).hexdigest(),
                "patched_sha256": hashlib.sha256(data).hexdigest(),
                "only_public_class_flag_removed": True, "all_other_bytes_unchanged": True}, indent=2) + "\n")

        harness_sources = [FIXTURES / "harness" / (name + ".java") for name in
                           ("ScopedMaskConstructorsHarness", "ScopedMaskConstructorsCaller")]
        if args.allocated_default:
            adapted = work / "authored-harness"
            adapted.mkdir()
            changed = []
            for original in harness_sources:
                text = original.read_text()
                if original.stem == "ScopedMaskConstructorsHarness":
                    text = text.replace("        for (int attempt", "        Sink<?> previous = null;\n        for (int attempt")
                    text = text.replace("if (defaultCallback && attempt == 0) events = Trace.fail == 2 ? 2 : 23;",
                        "if (defaultCallback && attempt == 0) events = Trace.fail == 2 ? 2 : 23;\n"
                        "            else if (defaultCallback && Trace.fail != 2) events = 3;")
                    text = text.replace("partial.callback == Defaults.CB", "partial.callback instanceof Defaults && partial.callback != incoming && partial.callback != previous")
                    text = text.replace("            if (initFailure) {", "            if (Trace.partial != null) previous = ((Core<?>)Trace.partial).callback;\n            if (initFailure) {")
                    text = text.replace("if (attempt == 0) require(failure instanceof ExceptionInInitializerError",
                        "if (Trace.fail == 3) require(failure == Trace.failure);\n"
                        "                else if (attempt == 0) require(failure instanceof ExceptionInInitializerError")
                output = adapted / original.name
                output.write_text(text)
                changed.append(output)
            harness_sources = changed
        if args.fixed_nominal:
            adapted = work / "authored-harness"
            adapted.mkdir()
            changed = []
            for original in harness_sources:
                text = original.read_text().replace("Core<?>", "Core")
                if original.stem == "ScopedMaskConstructorsHarness":
                    text = text.replace("Object token = new Object();", "Trace token = new Trace();")
                    text = text.replace("((Sink<Object>)result.callback).accept(token);", "result.callback.accept(token);")
                    marker = "        Sink<Object> incoming = new Consumer();"
                    if text.count(marker) != 1:
                        raise RuntimeError("Original constructor reflection observer anchor changed")
                    text = text.replace(marker,
                        "        require(Core.class.getTypeParameters().length == 0);\n"
                        "        Type parameter = Core.class.getDeclaredConstructor(int.class, int.class, Sink.class).getGenericParameterTypes()[2];\n"
                        "        require(parameter instanceof ParameterizedType && ((ParameterizedType)parameter).getRawType() == Sink.class);\n"
                        "        Type argument = ((ParameterizedType)parameter).getActualTypeArguments()[0];\n"
                        "        require(argument instanceof WildcardType && ((WildcardType)argument).getLowerBounds().length == 1 && ((WildcardType)argument).getLowerBounds()[0] == Trace.class);\n" + marker)
                if args.star_bounded:
                    text = text.replace("Sink<Object>", "Sink<Trace>")
                    text = text.replace("result.callback.accept(token);", "((Sink)result.callback).accept(token);")
                    text = text.replace("((WildcardType)argument).getLowerBounds().length == 1 && ((WildcardType)argument).getLowerBounds()[0] == Trace.class",
                                        "((WildcardType)argument).getLowerBounds().length == 0 && Sink.class.getTypeParameters()[0].getBounds()[0] == Trace.class")
                if args.package_subclass or args.private_target:
                    text = "package fixture;\n" + text
                    text = text.replace("MethodHandles.publicLookup()", "MethodHandles.lookup()")
                if args.private_target and original.stem == "ScopedMaskConstructorsHarness":
                    incoming = "        Sink<" + ("Trace" if args.star_bounded else "Object") + "> incoming = new Consumer();"
                    if text.count(incoming) != 1:
                        raise RuntimeError("Original private mask observer anchor changed")
                    text = text.replace(incoming,
                        "        require(Modifier.isPrivate(Core.class.getDeclaredConstructor(int.class, int.class, Sink.class).getModifiers()));\n"
                        "        require((ctor.getModifiers() & (Modifier.PUBLIC | Modifier.PROTECTED | Modifier.PRIVATE)) == 0);\n"
                        + incoming.replace("new Consumer()", "mode >= 32 ? null : new Consumer()"))
                    invocation = "((Sink)result.callback).accept(token);" if args.star_bounded else "result.callback.accept(token);"
                    observer = ("                " + invocation + "\n"
                                "                events = events * 10 + 5;\n"
                                "                require(Trace.seen == token && Trace.events == events);")
                    if text.count(observer) != 1:
                        raise RuntimeError("Original private null callback observer changed")
                    text = text.replace(observer,
                        "                if (result.callback == null) {\n"
                        "                    boolean failed = false;\n"
                        "                    try { " + invocation + " } catch (NullPointerException expected) { failed = true; }\n"
                        "                    require(failed && Trace.seen == null && Trace.events == events);\n"
                        "                } else {\n" + observer + "\n                }")
                if args.package_subclass:
                    if original.stem == "ScopedMaskConstructorsHarness":
                        text = text.replace("events = events * 10 + 4;", "events = events * 100 + 64;")
                        text = text.replace("int mask = mode % 8;", "int mask = mode % 8;")
                if args.register_mask:
                    text = text.replace("int count, int capacity", "boolean count, int capacity")
                    text = text.replace("int.class, int.class, Sink.class", "boolean.class, int.class, Sink.class")
                    text = text.replace(".invoke(11, 19, callback", ".invoke(false, 19, callback")
                    text = text.replace("make(route, 11, 19, incoming", "make(route, false, 19, incoming")
                    text = text.replace("partial.count == 11", "partial.count == ((mask & 1) != 0)")
                    text = text.replace("boolean defaultCallback = (mask & 4) != 0;", "boolean defaultCallback = false;")
                    text = text.replace("((mask & 2) != 0 ? 7 : 19)", "19")
                output = adapted / original.name
                output.write_text(text)
                changed.append(output)
            harness_sources = changed
        verify.compile_local(harness_sources, harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        providers = {"Lfixture/Defaults;", "Lfixture/Consumer;"} if args.dependency_defaults else set()
        classes = all_classes - providers
        if providers:
            frozen = work / "owned-defaults.jar"
            with zipfile.ZipFile(frozen, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                for owner in sorted(providers):
                    name = owner[1:-1] + ".class"
                    archive.writestr(name, (originals / name).read_bytes())
            dependencies.append(frozen)
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}

        def facts(root):
            return {row["name"]: row for path in sorted(root.rglob("*.class"))
                    if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes
                    for row in [KotlinFixtureClassFile(path.read_bytes()).facts()]}

        records, exceptions = facts(originals), exception_inventory(originals)
        synthetic = {identity: row for owner, record in records.items()
                     for identity, row in record["methods"].items() if row["access"] & 0x1000}
        if (len(all_classes) != (8 if args.scoped_interfaces else 7 if args.scoped_parent_chain else 8 if args.package_subclass else 5) or len(all_methods) != (11 if args.scoped_parent_chain else 10 if args.package_subclass or args.allocated_default else 9) or len(synthetic) != 1
                or any(row["name"] != "<init>" or row["signature"] is not None for row in synthetic.values())):
            raise RuntimeError("Kotlin did not produce the original raw synthetic mask constructor")
        if args.private_target and any(row["access"] & 7 for row in synthetic.values()):
            raise RuntimeError("Kotlin private target did not retain its package mask entry")
        dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        classpath = ["--classpath", args.android_jar.resolve()]
        for path in dependencies:
            classpath += ["--classpath", path]

        def execute(runtime, synthetic_flag, label):
            outputs = []
            for route in range(4):
                for mode in range(40 if args.private_target else 32):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                        ("fixture." if args.package_subclass or args.private_target else "") + "ScopedMaskConstructorsHarness", str(route), str(mode), str(synthetic_flag).lower()],
                        f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Original mask constructor behavior matrix changed: " + output)
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), True, "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline) + "\n")

        def dex(root, destination, included=classes, extra=()):
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in included]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, *extra,
                        "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Owned mangled interface changed DEX inventory")
            return result

        text = (fixture / "kotlin/fixture/Core.kt").read_text()
        delayed_fields = """package fixture
class Core<T>(count: Int, capacity: Int = 7, callback: Sink<in T> = Defaults.CB) {
    init { Trace.partial = this; Trace.hit(4) }
    @JvmField val count: Int = count
    @JvmField val capacity: Int = capacity
    @JvmField val callback: Sink<in T> = callback
}
"""
        if args.scoped_parent_chain:
            delayed_fields=delayed_fields.replace("= Defaults.CB) {","= Defaults.CB) : Middle<T>() {")
        if args.scoped_interfaces:
            delayed_fields = delayed_fields.replace("= Defaults.CB) {", "= Defaults.CB) : Left<T>, Right<T>, java.io.Serializable {")
        if args.fixed_nominal:
            delayed_fields = delayed_fields.replace("class Core<T>", "class Core").replace("Sink<in T>", "Sink<in Trace>")
            if args.package_subclass:
                delayed_fields = delayed_fields.replace("= Defaults.CB) {", "= Defaults.CB) : Parent() {")
        if args.star_bounded:
            delayed_fields = delayed_fields.replace("Sink<in Trace>", "Sink<*>")
        if args.closed_parent:
            delayed_fields = delayed_fields.replace(": Parent() {", ": Parent<Trace>() {")
        if args.register_mask:
            delayed_fields = delayed_fields.replace("count: Int", "count: Boolean = true")
            delayed_fields = delayed_fields.replace("val count: Boolean = true = count", "val count: Boolean = count")
            delayed_fields = delayed_fields.replace(": Parent() {", ": Parent(), ParentAPI {")
            delayed_fields = delayed_fields.replace("capacity: Int = 7", "capacity: Int")
            delayed_fields = delayed_fields.replace("Sink<in Trace> = Defaults.CB", "Sink<in Trace>")
        if args.private_target:
            delayed_fields = delayed_fields.replace("class Core(", "class Core private constructor(")
        mutations = (
            ("wrong-default", text.replace("capacity: Int = 7", "capacity: Int = 8"), "2"),
            ("new-callback", text.replace("= Defaults.CB", "= Consumer()"), "4"),
            ("eager-callback", text.replace("init {", "init { val ignored = Defaults.CB;"), "0"),
            ("late-fields", delayed_fields, "24"),
        )
        if args.register_mask:
            mutations = (("wrong-boolean-default", text.replace("Boolean = true", "Boolean = false"), "1"),
                         ("unexpected-consumer", text.replace("init {", "init { Consumer();"), "0"),
                         ("eager-callback", text.replace("init {", "init { val ignored = Defaults.CB;"), "0"),
                         ("late-fields", delayed_fields, "24"))
        if args.allocated_default:
            mutations = (
                ("wrong-default", text.replace("capacity: Int = 7", "capacity: Int = 8"), "2"),
                ("skipped-allocation", text.replace("= Defaults()", "= Consumer()"), "4"),
                ("duplicate-allocation", text.replace("= Defaults()", "= run { Defaults(); Defaults() }"), "4"),
                ("eager-allocation", text.replace("init {", "init { val ignored = Defaults();"), "0"),
                ("late-fields", delayed_fields.replace("= Defaults.CB", "= Defaults()"), "24"),
            )
        for label, source, mode in mutations:
            if source == text:
                raise RuntimeError("Original mask counterexample anchor changed")
            changed = work / label / "Core.kt"
            changed.parent.mkdir()
            changed.write_text(source)
            wrong = work / (label + "-classes")
            wrong.mkdir()
            mutation_compiler = list(compiler)
            if args.package_subclass:
                cp_index = mutation_compiler.index("-classpath") + 1
                mutation_compiler[cp_index] += os.pathsep + str(originals)
            verify.run([*mutation_compiler, "-d", wrong, changed], "kotlinc-" + label)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness, *dependencies))),
                                     ("fixture." if args.package_subclass or args.private_target else "") + "ScopedMaskConstructorsHarness", "1", mode, "true"], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "scoped mask constructor contract" not in result.stderr:
                raise RuntimeError("Independent observer missed mask counterexample: " + label)

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original mangled interface runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "sdk-constructor-code")
        wrapper = next(iter(synthetic.values()))["prototype"]
        target = "(ZILfixture/Sink;)V" if args.register_mask else "(IILfixture/Sink;)V"
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            descriptor = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if block[1] == "Lfixture/Core;" and name and name[1] == "<init>" and descriptor and descriptor[1] == wrapper:
                ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
                calls = [tail for op, tail in ops if op.startswith("invoke-direct") and "Lfixture/Core;.<init>:" + target in tail]
                if len(calls) != 1 or sum(op.startswith("if-") for op, tail in ops) != (1 if args.register_mask else 2) or (not args.register_mask and not args.allocated_default and not any(op == "sget-object" for op, tail in ops)):
                    raise RuntimeError("D8 changed the original generic mask constructor body")
                if args.allocated_default:
                    registers = re.search(r"(?m)^\s+registers\s+:\s+(\d+)", block[2])
                    incoming = re.search(r"(?m)^\s+ins\s+:\s+(\d+)", block[2])
                    allocations = [re.match(r"\s+v(\d+),\s+Lfixture/Defaults;", tail) for op, tail in ops if op == "new-instance"]
                    if (not registers or not incoming or len(allocations) != 1 or not allocations[0]
                            or int(allocations[0][1]) != int(registers[1]) - int(incoming[1]) + 3):
                        raise RuntimeError("D8 did not overwrite the original callback parameter with its default allocation")
                    (work / "allocation-register-witness.json").write_text(json.dumps({
                        "constructor": next(iter(synthetic)), "registers": int(registers[1]),
                        "incoming_words": int(incoming[1]), "allocation_register": int(allocations[0][1]),
                        "overwritten_reference_parameter": 2, "original_kotlin_and_d8_code": "unchanged"}, indent=2) + "\n")
                if args.register_mask and not any(op=="and-int/2addr" for op, tail in ops):
                    raise RuntimeError("Original D8 default Boolean mask lacks its register operation")
                witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original generic mask constructor witness")
        (work / "input-method-witness.json").open("x").write(json.dumps({"dex_sha256": hashlib.sha256(source_dex.read_bytes()).hexdigest(), "constructor": next(iter(synthetic)), "original_kotlin_and_d8_code": "unchanged"}, indent=2) + "\n")
        annotations_dump = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        visible = re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations_dump)
        if "VISIBILITY_BUILD" in annotations_dump or visible != ["Lkotlin/Metadata;"]:
            raise RuntimeError("D8's independently read annotation retention changed")
        # Compare the DEX contract only after independently confirming every
        # CLASS-retained annotation was removed by D8. Keep their full original
        # payloads as evidence; all runtime compiler annotations stay exact.
        dex_records = copy.deepcopy(records)
        dropped = {}
        for owner in classes:
            record = dex_records[owner]
            for identity, row in [(owner, record), *record["fields"].items(), *record["methods"].items()]:
                for key in ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations"):
                    if row.get(key):
                        dropped.setdefault(identity, {})[key] = row[key]
                    row[key] = []
        jvm_field = {"type": "Lkotlin/jvm/JvmField;", "elements": {}}
        not_null = {"type": "Lorg/jetbrains/annotations/NotNull;", "elements": {}}
        expected_dropped = {
            "Lfixture/Core;->count:I": {"runtime_invisible_annotations": [jvm_field]},
            "Lfixture/Core;->capacity:I": {"runtime_invisible_annotations": [jvm_field]},
            "Lfixture/Core;->callback:Lfixture/Sink;": {"runtime_invisible_annotations": [jvm_field, not_null]},
            "Lfixture/Core;-><init>(IILfixture/Sink;)V": {"runtime_invisible_parameter_annotations": [[], [], [not_null]]},
        }
        if args.private_target:
            # K2 does not attach parameter NotNull to the private constructor.
            # Its original field annotations and all other metadata stay exact.
            del expected_dropped["Lfixture/Core;-><init>(IILfixture/Sink;)V"]
        if args.register_mask:
            expected_dropped = {key.replace("->count:I", "->count:Z").replace("<init>(II", "<init>(ZI"): value
                                for key, value in expected_dropped.items()}
        if dropped != expected_dropped:
            raise RuntimeError("Original exact Kotlin CLASS-retained annotation witnesses changed")
        (work / "d8-annotation-differences.json").write_text(json.dumps(dropped, indent=2) + "\n")
        (work/"baseline-cli-sha256.txt").open("x").write(hashlib.sha256(args.neverd.read_bytes()).hexdigest()+"\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["projected_method_count"] != 1
                or projected != set(synthetic) or coverage["status"] != "partial"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Mask constructor recovery changed original inventory or unrelated contracts")
        for identity in projected:
            row = rows[identity]
            if (row.get("projection_kind") != "synthetic-constructor" or row.get("source_identity") != identity
                    or len(row.get("metadata_differences", [])) != 1):
                raise RuntimeError("Original mask constructor lacks its disclosed flag difference")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=os.pathsep.join(map(str, dependencies)))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if any(kind == "body" for identity, kind in all_methods.items()
                                                        if identity.split("->", 1)[0] == owner)}
        if (helpers != expected_helpers or actual_classes != classes
                or set(actual_methods) != methods | helpers):
            raise RuntimeError("Source compiler invented or omitted a runtime entry")
        after, after_exceptions = facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            original, actual = dex_records[owner], after[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if original.get(key) != actual.get(key):
                    raise RuntimeError("Original class/field metadata changed: " + owner + ":" + key)
            for identity, metadata in original["methods"].items():
                target, wanted = identity, copy.deepcopy(metadata)
                if identity in synthetic:
                    wanted["access"] &= ~0x1000
                if actual["methods"].get(target) != wanted or after_exceptions.get(target) != exceptions[identity]:
                    raise RuntimeError("Original method metadata changed beyond the disclosed name: " + identity)
        if execute((rebuilt, harness), False, "rebuilt") != baseline:
            raise RuntimeError("Mask constructor changed defaults, provider effects or failures")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != value for p, value in input_hashes.items())
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != value for p, value in dependency_hashes.items())):
            raise RuntimeError("Original compiler, SDK, inputs or immutable callers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_defaults": args.dependency_defaults,
                    "fixed_nominal_owner": args.fixed_nominal,
                    "private_target_and_package_mask_visibility": args.private_target,
                    "package_subclass": args.package_subclass,
                    "register_mask": args.register_mask,
                    "closed_parent": args.closed_parent,
                    "scoped_parent_chain": args.scoped_parent_chain,
                    "scoped_interfaces": args.scoped_interfaces,
                    "star_bounded": args.star_bounded,
                    "allocated_default_overwrites_reference_parameter": args.allocated_default,
                    "compiler_generated_raw_mask_constructor": "unchanged input",
                    "mask_selected_same_owner_target_and_full_contract": "matched",
                    "default_identity_lazy_initialization_partial_fields_and_exception_identity": "matched",
                    "original_metadata_and_exceptions": "unchanged except disclosed synthetic flag",
                    "d8_class_retained_annotation_differences": "independently verified and recorded",
                    "independent_compiler_counterexamples": len(mutations),
                    "original_inputs_and_oracles": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
