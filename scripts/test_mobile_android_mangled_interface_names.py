#!/usr/bin/env python3
"""Verify genuine Kotlin value-return interface/final names and shared aliases."""
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
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
    from test_mobile_android_protected_array_callbacks import change_method_flags


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--kotlin-lib", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--kotlin-java-home", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--enclosing", action="store_true")
    parser.add_argument("--default-root", action="store_true")
    parser.add_argument("--synthetic-implementation", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-mangled-interface-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "mangled_interface_names"
        if args.default_root:
            authored = work / "default-fixture"
            shutil.copytree(fixture, authored)
            fixture = authored
            names = fixture / "kotlin/fixture/Names.kt"
            text = names.read_text()
            marker = "interface Api<T> { fun send(value: T): Token }"
            if text.count(marker) != 1:
                raise RuntimeError("Original default interface root source anchor changed")
            names.write_text(text.replace(marker, "interface Api<T> { fun send(value: T): Token { Trace.hit(6); throw Trace.failure } }"))
        if args.enclosing:
            authored = work / "authored-fixture"
            shutil.copytree(fixture, authored)
            fixture = authored
            trace = fixture / "java/fixture/Trace.java"
            trace.write_text(trace.read_text().replace("public final class Trace {", "public final class Trace {\n    public static Object marker;"))
            names = fixture / "kotlin/fixture/Names.kt"
            text = names.read_text()
            if text.count("        Trace.hit(2)") != 1:
                raise RuntimeError("Original family enclosure source anchor changed")
            names.write_text(text.replace("        Trace.hit(2)", "        Trace.marker = object {}\n        Trace.hit(2)"))
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
        if args.default_root:
            compiler += ["-Xjvm-default=all"]
        verify.run([*compiler, "-d", originals, *sorted((fixture / "kotlin").rglob("*.kt"))], "kotlinc-original")
        for path in sorted(original_java.rglob("*.class")):
            destination = originals / path.relative_to(original_java)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
        def mark_implementation(root):
            path = root / "fixture/Relay.class"
            record = KotlinFixtureClassFile(path.read_bytes()).facts()
            members = [m for m in record["methods"].values() if m["name"].startswith("send-")]
            if len(members) != 1 or members[0]["access"] != 0x1:
                raise RuntimeError("Missing original synthetic implementation encoding anchor")
            row = members[0]
            # Independently authored access-only variant: the shared encoder
            # checks every original Code byte and descriptor before/after.
            return change_method_flags(path, row["prototype"].encode(), 0x1, 0x1001, name=row["name"].encode())
        if args.synthetic_implementation:
            receipt = mark_implementation(originals)
            (work / "synthetic-implementation-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        token = work / "owned-value.jar"
        with zipfile.ZipFile(token, "x") as jar:
            jar.writestr("fixture/Token.class", (originals / "fixture/Token.class").read_bytes())
        dependencies.append(token)
        harness_sources = [FIXTURES / "harness" / (name + ".java") for name in
                           ("MangledInterfaceHarness", "MangledInterfaceCaller", "MangledFinalHarness", "MangledFinalCaller")]
        if args.enclosing:
            observer = work / "authored-harness/MangledInterfaceHarness.java"
            observer.parent.mkdir()
            text = harness_sources[0].read_text()
            before = "            Trace.events = 0; int oldCalls=calls[0], oldValue=input[0];"
            after = "            Trace.events = 0; Trace.marker = null; int oldCalls=calls[0], oldValue=input[0];"
            marker = "            require(Trace.events == expected && calls[0] == oldCalls + (callback ? 1 : 0));"
            if text.count(before) != 1 or text.count(marker) != 1:
                raise RuntimeError("Original enclosing-method observer anchors changed")
            check = ("            boolean reachedRelay = mode != 6 && !(variant == 1 && mode == 1);\n"
                     "            require(reachedRelay ? Trace.marker != null && Trace.marker.getClass().getEnclosingMethod().equals(Relay.class.getMethod(name, Object.class)) : Trace.marker == null);\n")
            observer.write_text(text.replace(before, after).replace(marker, check + marker))
            harness_sources[0] = observer
        if args.synthetic_implementation:
            if args.default_root:
                raise RuntimeError("Use the separate default-root control without synthetic encoding")
            observer = work / "synthetic-observer/MangledInterfaceHarness.java"
            observer.parent.mkdir()
            text = harness_sources[0].read_text()
            marker = "        require(!Api.class.getMethod(name,Object.class).isSynthetic());"
            if text.count(marker) != 1:
                raise RuntimeError("Synthetic implementation observer anchor changed")
            observer.write_text(text.replace(marker, marker + "\n        require(Relay.class.getDeclaredMethod(name,Object.class).isSynthetic() == name.contains(\"-\"));"))
            harness_sources[0] = observer
        if args.default_root:
            harness_sources.append(FIXTURES / "harness/DefaultRootFamilyHarness.java")
        verify.compile_local(harness_sources, harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        all_classes, all_methods = compiler_inventory(verify, originals)
        classes = all_classes - {"Lfixture/Token;"}
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}

        def facts(root, included=classes):
            return {row["name"]: row for path in sorted(root.rglob("*.class"))
                    if "L" + path.relative_to(root).as_posix()[:-6] + ";" in included
                    for row in [KotlinFixtureClassFile(path.read_bytes()).facts()]}

        records, exceptions = facts(originals), exception_inventory(originals)
        family = {identity: row for owner, record in records.items()
                  for identity, row in record["methods"].items() if row["name"].startswith("send-")}
        original_names = {row["name"] for row in family.values()}
        if (len(classes) != (8 if args.enclosing else 7) or len(methods) != (17 if args.enclosing else 16) or len(family) != 4 or len(original_names) != 1
                or {identity.split("->", 1)[0] for identity in family} != {"Lfixture/Api;", "Lfixture/Relay;", "Lfixture/Child;", "Lfixture/Terminal;"}
                or any(row["access"] & 0x1008 != (0x1000 if args.synthetic_implementation and identity.startswith("Lfixture/Relay;->") else 0) for identity, row in family.items())):
            raise RuntimeError("Kotlin did not produce the original public virtual mangled family")
        original_name = next(iter(original_names))
        nested = "Lfixture/Relay$send$1;"
        nested_constructor = nested + "-><init>()V"
        if args.enclosing:
            original_nested = records[nested]
            if (original_nested["access"] != 0x31 or original_nested["fields"]
                    or original_nested["inner_class"] != {"inner": nested, "outer": None, "name": None, "access": 0x19}
                    or original_nested["enclosing_method"] != {"owner": "Lfixture/Relay;", "name": original_name,
                                                               "prototype": "(Ljava/lang/Object;)Ljava/lang/Object;"}
                    or set(original_nested["methods"]) != {nested_constructor}):
                raise RuntimeError("Kotlin anonymous enclosing-method witness changed")
        dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        classpath = ["--classpath", args.android_jar.resolve()]
        for path in dependencies:
            classpath += ["--classpath", path]

        def execute(runtime, name, label):
            outputs = []
            for variant in range(2):
                for route in range(5):
                    for mode in range(7):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                            "DefaultRootFamilyHarness" if args.default_root else "MangledInterfaceHarness",
                            str(variant), str(route), str(mode), name], f"{label}-{variant}-{route}-{mode}")
                        expected = f"variant={variant},route={route},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Original virtual method behavior matrix changed: " + output)
                        outputs.append(output)
            return outputs

        baseline = execute((originals, harness), original_name, "baseline")

        def execute_final(runtime, name, label):
            outputs = []
            for route in range(4):
                for mode in range(7):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                        "MangledFinalHarness", str(route), str(mode), name], f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Original final method behavior matrix changed: " + output)
                    outputs.append(output)
            return outputs

        final_baseline = execute_final((originals, harness), original_name, "final-baseline")

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

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original mangled interface runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        annotations_dump = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        visible = re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations_dump)
        if "VISIBILITY_BUILD" in annotations_dump or visible != ["Lkotlin/Metadata;"] * (7 if args.enclosing else 6):
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
        if len(dropped) != 15:
            raise RuntimeError("Original Kotlin CLASS-retained annotation witness changed")
        (work / "d8-annotation-differences.json").write_text(json.dumps(dropped, indent=2) + "\n")
        output = work / "recovered"
        # The complete Android/Kotlin declaration set and archive accounting
        # exceed the default work allowance even for the abstract-root control.
        # Match the existing default-access oracle's explicit analysis window.
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["projected_method_count"] != (5 if args.enclosing else 4)
                or projected != set(family) | ({nested_constructor} if args.enclosing else set()) or coverage["status"] != "partial"
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Mangled method recovery changed original inventory or unrelated contracts")
        mappings, names = {}, set()
        declaration = "Lfixture/Api;->" + original_name + "(Ljava/lang/Object;)Ljava/lang/Object;"
        for identity in family:
            row, original = rows[identity], family[identity]
            target = identity.split("->", 1)[0] + "->" + row["source_name"] + original["prototype"]
            required_declaration = identity if identity.startswith("Lfixture/Terminal;->") else declaration
            if (row.get("projection_kind") != "virtual-method-identifier" or row.get("source_identity") != target
                    or row.get("source_name_declaration") != required_declaration or row.get("conflicting_inherited_contracts") != []
                    or set(row.get("source_name_family", [])) != set(family)
                    or len(row.get("metadata_differences", [])) != 3 or target in methods
                    or row["source_name"] == "__neverdVirtualMethod0"):
                raise RuntimeError("Virtual family lacks its complete disclosed shared source mapping")
            if args.synthetic_implementation and identity.startswith("Lfixture/Relay;->") and not any(
                    "cannot reproduce the original synthetic method flag" in text for text in row["metadata_differences"]):
                raise RuntimeError("Missing explicit synthetic implementation flag receipt")
            mappings[identity] = target
            names.add(row["source_name"])
        if len(names) != 1:
            raise RuntimeError("One virtual family acquired multiple source names")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=os.pathsep.join(map(str, dependencies)))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        bindings = coverage.get("class_source_bindings", [])
        class_names = {}
        if len(bindings) != int(args.enclosing):
            raise RuntimeError("Unexpected local class name mappings")
        if args.enclosing:
            binding = bindings[0]
            candidates = [c for c in actual_classes if c.endswith(binding["source_name"] + ";")]
            original_enclosing = "Lfixture/Relay;->" + original_name + "(Ljava/lang/Object;)Ljava/lang/Object;"
            expected_enclosing = {"owner": "Lfixture/Relay;", "name": original_name,
                                  "prototype": "(Ljava/lang/Object;)Ljava/lang/Object;",
                                  "parameters": ["Ljava/lang/Object;"], "returns": "Ljava/lang/Object;",
                                  "identity": original_enclosing}
            expected_source_enclosing = dict(expected_enclosing, name=next(iter(names)), identity=mappings[original_enclosing])
            if (len(candidates) != 1 or binding["class"] != nested
                    or binding["binding_kind"] != "anonymous-to-named-local"
                    or binding.get("enclosing_method") != expected_enclosing
                    or binding.get("source_enclosing_method") != expected_source_enclosing
                    or not any("constructor parameters may differ" in text for text in binding["metadata_differences"])
                    or not any("InnerClass flags" in text for text in binding["metadata_differences"])
                    or rows[nested_constructor]["projection_kind"] != "anonymous-to-named-local"):
                raise RuntimeError("Missing exact original and source enclosing-method receipt")
            class_names[nested] = candidates[0]

        def mapped(identity):
            identity = mappings.get(identity, identity)
            if args.enclosing and identity == nested_constructor:
                identity = nested + "-><init>(Lfixture/Relay;)V"
            for old, new in class_names.items():
                identity = identity.replace(old, new)
            return identity

        helpers = {mapped(row["class"] + "->" + row["name"] + row["prototype"]) for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {mapped(owner) + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes - ({"Lfixture/Branch;"} if args.default_root else {"Lfixture/Api;", "Lfixture/Branch;"})}
        expected_helpers.add("Lfixture/NamesKt;-><init>()V")
        if (helpers != expected_helpers or actual_classes != {mapped(c) for c in classes}
                or set(actual_methods) != {mapped(m) for m in methods} | helpers):
            raise RuntimeError("Source compiler invented or omitted a runtime entry")
        after, after_exceptions = facts(rebuilt, actual_classes), exception_inventory(rebuilt)
        for owner in classes:
            original, actual = dex_records[owner], after[mapped(owner)]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                expected = copy.deepcopy(original.get(key))
                if args.enclosing and owner == nested:
                    if key == "access":
                        expected &= ~0x1  # A named local class cannot retain public access.
                    elif key == "inner_class":
                        expected.update(inner=mapped(nested), name=bindings[0]["source_name"], access=0x10)
                    elif key == "enclosing_method":
                        expected["name"] = next(iter(names))
                if expected != actual.get(key):
                    raise RuntimeError("Original class/field metadata changed: " + owner + ":" + key)
            for identity, metadata in original["methods"].items():
                target, wanted = mapped(identity), copy.deepcopy(metadata)
                if identity in mappings:
                    wanted["name"] = rows[identity]["source_name"]
                if args.synthetic_implementation and identity in family and owner == "Lfixture/Relay;":
                    wanted["access"] &= ~0x1000
                if args.enclosing and identity == nested_constructor:
                    wanted["prototype"] = "(Lfixture/Relay;)V"
                if actual["methods"].get(target) != wanted or after_exceptions.get(target) != exceptions[identity]:
                    raise RuntimeError("Original method metadata changed beyond the disclosed name: " + identity)
        if execute((rebuilt, harness), next(iter(names)), "rebuilt") != baseline:
            raise RuntimeError("Shared virtual name changed dispatch, aliases or original failures")
        if execute_final((rebuilt, harness), next(iter(names)), "final-rebuilt") != final_baseline:
            raise RuntimeError("Final class name changed dispatch, aliases or original failures")
        external = work / "external-api.jar"
        with zipfile.ZipFile(external, "x") as jar:
            jar.writestr("fixture/Api.class", (originals / "fixture/Api.class").read_bytes())
        extra = ["--classpath", external]
        negative_dex = dex(originals, work / "external-dex", classes - {"Lfixture/Api;"}, extra)
        rejected = work / "external-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, (*classpath, *extra)),
                                 "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "external-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists():
            raise RuntimeError("An external mangled interface was silently renamed")
        changed = work / "wrong-super/Names.kt"
        changed.parent.mkdir()
        text = (fixture / "kotlin/fixture/Names.kt").read_text()
        if text.count("return super.send(value)") != 1:
            raise RuntimeError("Original virtual super witness changed")
        changed.write_text(text.replace("return super.send(value)", "return target.send(value)"))
        wrong = work / "wrong-super-classes"
        wrong.mkdir()
        verify.run([*compiler, "-d", wrong, changed], "kotlinc-wrong-super")
        if args.synthetic_implementation:
            mark_implementation(wrong)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness, *dependencies))),
                                 "MangledInterfaceHarness", "1", "1", "0", original_name], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-super-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "mangled virtual dispatch changed" not in result.stderr:
            raise RuntimeError("Independent observer did not detect skipped superclass effects")
        changed_final = work / "wrong-final/Names.kt"
        changed_final.parent.mkdir()
        if text.count("Trace.hit(4)") != 1:
            raise RuntimeError("Original final dispatch witness changed")
        changed_final.write_text(text.replace("Trace.hit(4)", "Trace.hit(4)\n        Trace.hit(4)"))
        wrong_final = work / "wrong-final-classes"
        wrong_final.mkdir()
        verify.run([*compiler, "-d", wrong_final, changed_final], "kotlinc-wrong-final")
        if args.synthetic_implementation:
            mark_implementation(wrong_final)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong_final, originals, harness, *dependencies))),
                                 "MangledFinalHarness", "1", "0", original_name], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-final-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "mangled final dispatch changed" not in result.stderr:
            raise RuntimeError("Independent observer did not detect repeated final method effects")
        if args.default_root:
            changed_default = work / "wrong-default/Names.kt"
            changed_default.parent.mkdir()
            if text.count("Trace.hit(6)") != 1:
                raise RuntimeError("Original default interface body counterexample anchor changed")
            changed_default.write_text(text.replace("Trace.hit(6)", "Trace.hit(5)"))
            wrong_default = work / "wrong-default-classes"
            wrong_default.mkdir()
            verify.run([*compiler, "-d", wrong_default, changed_default], "kotlinc-wrong-default")
            if args.synthetic_implementation:
                mark_implementation(wrong_default)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong_default, originals, harness, *dependencies))),
                                     "DefaultRootFamilyHarness", "0", "1", "0", original_name], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / "wrong-default-counterexample.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "mangled virtual dispatch changed" not in result.stderr:
                raise RuntimeError("Independent observer did not detect changed interface default effects")
        if args.enclosing:
            wrong_enclosing = work / "wrong-enclosing/Names.kt"
            wrong_enclosing.parent.mkdir()
            marker = "Trace.marker = object {}"
            if text.count(marker) != 1:
                raise RuntimeError("Original enclosing-method counterexample anchor changed")
            wrong_enclosing.write_text(text.replace(marker, "Trace.marker = misplacedMarker()")
                                       + "\nfun misplacedMarker(): Any = object {}\n")
            wrong_enclosing_classes = work / "wrong-enclosing-classes"
            wrong_enclosing_classes.mkdir()
            verify.run([*compiler, "-d", wrong_enclosing_classes, wrong_enclosing], "kotlinc-wrong-enclosing")
            if args.synthetic_implementation:
                mark_implementation(wrong_enclosing_classes)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str,
                                     (wrong_enclosing_classes, originals, harness, *dependencies))),
                                     "MangledInterfaceHarness", "0", "3", "0", original_name], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / "wrong-enclosing-counterexample.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "mangled virtual dispatch changed" not in result.stderr:
                raise RuntimeError("Independent observer missed an incorrect enclosing method")
        if args.synthetic_implementation:
            variant = work / "lost-synthetic-classes"
            path = variant / "fixture/Relay.class"
            path.parent.mkdir(parents=True)
            shutil.copy2(originals / "fixture/Relay.class", path)
            receipt = change_method_flags(path, b"(Ljava/lang/Object;)Ljava/lang/Object;", 0x1001, 0x1, name=original_name.encode())
            (work / "lost-synthetic-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (variant, originals, harness, *dependencies))),
                                     "MangledInterfaceHarness", "0", "3", "0", original_name], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / "lost-synthetic-counterexample.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "mangled virtual dispatch changed" not in result.stderr:
                raise RuntimeError("Frozen observer missed synthetic reflection drift")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != value for p, value in input_hashes.items())
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != value for p, value in dependency_hashes.items())):
            raise RuntimeError("Original compiler, SDK, inputs or immutable callers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline) + len(final_baseline), "attempts_per_case": 2,
                    "compiler_generated_mangled_names": "unchanged input", "virtual_interface_and_super_dispatch": "matched",
                    "full_metadata_and_original_exceptions": ("unchanged except disclosed names, local access and capture constructor"
                                                              if args.enclosing else "unchanged except disclosed names"),
                    "renamed_enclosing_method": args.enclosing,
                    "default_interface_root": args.default_root,
                    "d8_class_retained_annotation_differences": "independently verified and recorded",
                    "external_name_contract": "rejected", "independent_skipped_super_counterexample": "detected",
                    "final_class_and_interface_names": "one shared mapping with exact roots",
                    "independent_repeated_final_effect_counterexample": "detected",
                    "original_inputs_and_oracles": "unchanged", "binary_abi_name_changes": "reported"}
        if args.synthetic_implementation:
            evidence["synthetic_implementation"] = "access-only encoding, original Code unchanged"
            evidence["synthetic_method_flag_loss"] = "explicitly reported and independently observed"
            evidence["independent_lost_synthetic_counterexample"] = "detected"
            evidence["full_metadata_and_original_exceptions"] += ", and reported synthetic method flag"
        if args.enclosing:
            evidence["independent_incorrect_enclosing_method_counterexample"] = "detected"
        if args.default_root:
            evidence["inherited_default_body_dispatch_and_failure_identity"] = "matched"
            evidence["independent_changed_default_body_counterexample"] = "detected"
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
