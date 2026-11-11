#!/usr/bin/env python3
"""Compare genuine Kotlin/D8 synthetic JVM names with disclosed Java names."""
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_protected_array_callbacks import change_method_flags


class KotlinFixtureClassFile(class_identity.ClassFile):
    """Strict, fixture-scoped typed annotations; never discard unknown payloads."""
    VISIBLE_ANNOTATIONS = {"Lkotlin/Metadata;", "Lkotlin/jvm/JvmStatic;", "Lkotlin/jvm/JvmInline;"}
    SCHEMAS = {
        "Lkotlin/Metadata;": {"mv": "[I", "bv": "[I", "k": "I", "xi": "I", "d1": "[s", "d2": "[s", "xs": "s", "pn": "s"},
        "Lkotlin/jvm/JvmName;": {"name": "s"},
        "Lkotlin/jvm/JvmInline;": {},
        **{name: {} for name in ("Lkotlin/jvm/JvmStatic;", "Lkotlin/jvm/JvmField;",
                                "Lorg/jetbrains/annotations/Nullable;", "Lorg/jetbrains/annotations/NotNull;")},
    }

    def deprecation(self, attributes, context, *, allowed=True, **unused):
        result = {"deprecated_attribute": False, "runtime_visible_annotations": [],
                  "runtime_invisible_annotations": [], "runtime_invisible_parameter_annotations": []}
        seen = set()
        require = class_identity.require

        def element(reader, expected):
            tag = reader.take(1).decode("ascii")
            require(tag == expected[0], "Unexpected compiler annotation value type on " + context)
            if tag == "[":
                return {"tag": tag, "values": [element(reader, expected[1:]) for _ in range(reader.number(2))]}
            require(tag in ("I", "s"), "Unsupported owned compiler annotation tag")
            index = reader.number(2)
            value = self.text(index) if tag == "s" else int.from_bytes(self.constant(index, 3), "big", signed=True)
            return {"tag": tag, "value": value}

        def annotations(reader, kind):
            rows, types = [], set()
            for _ in range(reader.number(2)):
                owner = self.text(reader.number(2))
                require(owner in self.SCHEMAS and owner not in types, "Unsupported/duplicate compiler annotation on " + context)
                types.add(owner)
                require((kind == "RuntimeVisibleAnnotations") == (owner in self.VISIBLE_ANNOTATIONS),
                        "Original compiler annotation retention changed")
                if kind == "RuntimeInvisibleParameterAnnotations":
                    require(owner in {"Lorg/jetbrains/annotations/Nullable;", "Lorg/jetbrains/annotations/NotNull;"},
                            "Unsupported compiler parameter annotation")
                schema, values = self.SCHEMAS[owner], {}
                for _ in range(reader.number(2)):
                    name = self.text(reader.number(2))
                    require(name in schema and name not in values, "Unsupported/duplicate compiler annotation element")
                    values[name] = element(reader, schema[name])
                require(owner == "Lkotlin/Metadata;" or set(values) == set(schema), "Missing compiler annotation element")
                rows.append({"type": owner, "elements": values})
            return sorted(rows, key=lambda row: row["type"])

        for kind, data in attributes:
            if kind not in class_identity.ANNOTATION_ATTRIBUTES:
                continue
            require(allowed and kind not in seen, "Unsupported/duplicate compiler annotation attribute on " + context)
            seen.add(kind)
            require(kind in {"RuntimeVisibleAnnotations", "RuntimeInvisibleAnnotations", "RuntimeInvisibleParameterAnnotations"},
                    "Unsupported owned compiler annotation attribute: " + kind)
            reader = class_identity.Bytes(data)
            if kind == "RuntimeInvisibleParameterAnnotations":
                result["runtime_invisible_parameter_annotations"] = [annotations(reader, kind) for _ in range(reader.number(1))]
            else:
                key = "runtime_visible_annotations" if kind == "RuntimeVisibleAnnotations" else "runtime_invisible_annotations"
                result[key] = annotations(reader, kind)
            reader.finish()
        return result


def compiler_facts(root: Path) -> dict:
    result = {}
    for path in sorted(root.rglob("*.class")):
        row = KotlinFixtureClassFile(path.read_bytes()).facts()
        if row["name"] in result or path.relative_to(root).as_posix() != row["name"][1:-1] + ".class":
            raise RuntimeError("Duplicate/misplaced original compiler class")
        result[row["name"]] = row
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--kotlin-lib", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-box", action="store_true")
    parser.add_argument("--ordinary-static", action="store_true")
    parser.add_argument("--private-forwarder", action="store_true")
    parser.add_argument("--package-default", action="store_true")
    args = parser.parse_args()
    if args.package_default and (not args.private_forwarder or args.ordinary_static):
        parser.error("--package-default requires --private-forwarder and synthetic methods")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-synthetic-names-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "synthetic_method_names"
        original_java, originals, harness = work / "original-java", work / "originals", work / "harness"
        kotlin = args.kotlin_lib.resolve()
        compiler_jars = sorted(kotlin.glob("*.jar"))
        if not (kotlin / "kotlin-compiler.jar").is_file() or not (kotlin / "kotlin-stdlib.jar").is_file():
            raise RuntimeError("The original witness requires the unmodified Kotlin compiler distribution")
        inputs = [args.android_jar.resolve(), *compiler_jars]
        input_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        stdlib = kotlin / "kotlin-stdlib.jar"
        annotations = kotlin / "annotations-13.0.jar"
        dependencies = []
        # NeverD deliberately rejects multi-release jars. Select the Java 8
        # base declarations explicitly, retaining the original class bytes.
        for jar in (stdlib, annotations):
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
        kotlin_sources = sorted((fixture / "kotlin").rglob("*.kt"))
        if args.ordinary_static or args.private_forwarder:
            # Authored source profiles, compiled without bytecode changes.
            plain = work / "ordinary-static-source"
            plain.mkdir()
            authored = []
            for source in kotlin_sources:
                text = source.read_text()
                if text.count("@JvmSynthetic ") != 3:
                    raise RuntimeError("Original public static profile source changed")
                if args.private_forwarder:
                    marker = "fun wide(value: Long): Long {\n        Trace.event(4)\n        return value\n    }"
                    if text.count(marker) != 1:
                        raise RuntimeError("Original wide forwarding profile changed")
                    text = text.replace(marker, "fun wide(value: Long): Long = pack(value)\n\n"
                        "    @JvmStatic @JvmSynthetic @JvmName(\"pack-value\")\n"
                        "    private fun pack(value: Long): Long { Trace.event(4); return value }")
                    if args.package_default:
                        text = text.replace("private fun pack(value: Long): Long { Trace.event(4); return value }",
                            "private fun pack(value: Long, delta: Long = 0L): Long { Trace.event(4); return value + delta }")
                path = plain / source.name
                path.write_text(text.replace("@JvmSynthetic ", "") if args.ordinary_static else text)
                authored.append(path)
            kotlin_sources = authored
        verify.run([verify.java, "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (stdlib, original_java))),
                    "-jvm-target", "1.8", "-d", originals, *kotlin_sources], "kotlinc-original")
        for path in sorted(original_java.rglob("*.class")):
            destination = originals / path.relative_to(original_java)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("SyntheticMethodHarness", "SyntheticMethodCaller")], harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        all_classes, all_methods = compiler_inventory(verify, originals)
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        classes = all_classes - ({"Lfixture/Box;"} if args.dependency_box else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        synthetic = {identity: row for owner, facts in records.items() if owner in classes
                     for identity, row in facts["methods"].items() if "-" in row["name"]}
        original_flags = 0x19 if args.ordinary_static else 0x1019
        if (len(all_classes) != 3 or len(synthetic) != (5 if args.package_default else 4 if args.private_forwarder else 3)
                or any(row["access"] != (0x1008 if row["name"] == "pack-value$default" else ((original_flags & ~1) | 2) if row["name"] == "pack-value" else original_flags)
                       for row in synthetic.values())):
            raise RuntimeError("Original compiler did not produce the exact public/private static name/flag witnesses")
        if args.dependency_box:
            box = work / "owned-box.jar"
            with zipfile.ZipFile(box, "x") as jar:
                jar.writestr("fixture/Box.class", (originals / "fixture/Box.class").read_bytes())
            dependencies.append(box)
        dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        classpath = ["--classpath", args.android_jar.resolve()]
        for path in dependencies:
            classpath += ["--classpath", path]

        def execute(runtime, names, label):
            outputs = []
            for operation, modes in enumerate(((0, 1, 2), (0, 1, 3, 2), (0, 1, 6, 2, 5), (0, 1, 4))):
                for route in range(4):
                    for mode in modes:
                        name = names["(J)J" if operation == 3 else "(Ljava/lang/Object;)Ljava/lang/Object;"]
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                            "SyntheticMethodHarness", str(operation), str(route), str(mode), name], f"{label}-{operation}-{route}-{mode}")
                        if not re.fullmatch(f"operation={operation},route={route},mode={mode},trace=[0-9]+\\n", output):
                            raise RuntimeError("Owned method case matrix changed: " + output)
                        outputs.append(output)
            return outputs

        names = {row["prototype"]: row["name"] for row in synthetic.values() if row["name"] == "read-value"}
        baseline = execute((originals, harness), names, "baseline")

        def dex(root, destination):
            destination.mkdir()
            paths = [path for path in sorted(root.rglob("*.class"))
                     if "L" + path.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", destination, *paths], "d8-" + destination.name)
            if sorted(destination.glob("classes*.dex")) != [destination / "classes.dex"]:
                raise RuntimeError("Owned method fixture changed DEX inventory")
            return destination / "classes.dex"

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original Kotlin runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        if args.package_default:
            disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-package-default-code")
            if "Lfixture/Provider;.pack-value$default:(JJILjava/lang/Object;)J" not in disassembly:
                raise RuntimeError("D8 lost the original package default mask call")
            (work / "package-default-witness.json").write_text(json.dumps({
                "original_kotlin_and_d8_code": "unchanged", "access": ["static", "synthetic"],
                "method": "Lfixture/Provider;->pack-value$default(JJILjava/lang/Object;)J",
                "private_target": "Lfixture/Provider;->pack-value(JJ)J"}, indent=2) + "\n")
        annotations_dump = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        visible = re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations_dump)
        if ("VISIBILITY_BUILD" in annotations_dump or visible.count("Lkotlin/Metadata;") != 1
                or visible.count("Lkotlin/jvm/JvmStatic;") != (8 if args.private_forwarder else 7)
                or len(visible) != (9 if args.private_forwarder else 8)):
            raise RuntimeError("D8's independently read annotation retention changed")
        # D8 drops CLASS-retained annotations before NeverD sees the input.
        # Record every original typed payload and require the SDK witness above;
        # runtime annotations (including the entire Kotlin Metadata) stay exact.
        dex_records = copy.deepcopy(records)
        dropped = {}
        for owner in classes:
            facts = dex_records[owner]
            for identity, row in [(owner, facts), *facts["fields"].items(), *facts["methods"].items()]:
                for key in ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations"):
                    if row.get(key):
                        dropped.setdefault(identity, {})[key] = row[key]
                    row[key] = []
        if len(dropped) != (8 if args.private_forwarder else 7):
            raise RuntimeError("Original Kotlin CLASS-retained annotation witness changed")
        (work / "d8-annotation-differences.json").write_text(json.dumps(dropped, indent=2) + "\n")
        output = work / "recovered"
        # The full Android/Kotlin declaration graph shares the analysis budget.
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["projected_method_count"] != len(synthetic)
                or projected != set(synthetic) or coverage["status"] != "partial"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Synthetic method recovery changed original inventory or unrelated contracts")
        mappings = {}
        for identity in projected:
            row = rows[identity]
            original = synthetic[identity]
            target = identity.split("->", 1)[0] + "->" + row["source_name"] + original["prototype"]
            kind = "static-method-identifier" if args.ordinary_static else "synthetic-static-method-identifier"
            if (row.get("projection_kind") != kind or row.get("source_identity") != target
                    or row.get("source_name_declaration") != identity or row.get("conflicting_inherited_contracts") != []
                    or len(row.get("metadata_differences", [])) != 3 or target in methods
                    or row["source_name"] == "__neverdStaticMethod0"):
                raise RuntimeError("Synthetic method lacks its explicit unique source identity and differences")
            mappings[identity] = target
        if len(set(mappings.values())) != len(mappings):
            raise RuntimeError("Distinct original synthetic methods share a source identity")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=os.pathsep.join(map(str, dependencies)))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if (helpers != expected_helpers or actual_classes != classes
                or set(actual_methods) != {mappings.get(m, m) for m in methods} | helpers):
            raise RuntimeError("Source compiler invented or omitted a runtime entry")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            original, actual = dex_records[owner], after[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if original.get(key) != actual.get(key):
                    raise RuntimeError("Original class/field metadata changed: " + owner + ":" + key)
            for identity, metadata in original["methods"].items():
                target, wanted = mappings.get(identity, identity), copy.deepcopy(metadata)
                if identity in mappings:
                    wanted["name"] = rows[identity]["source_name"]
                    wanted["access"] &= ~0x1000
                if actual["methods"].get(target) != wanted or after_exceptions.get(target) != exceptions[identity]:
                    raise RuntimeError("Original method metadata changed beyond the disclosed name/flag: " + identity)
        renamed = {row["prototype"]: rows[identity]["source_name"] for identity, row in synthetic.items() if row["name"] == "read-value"}
        if execute((rebuilt, harness), renamed, "rebuilt") != baseline:
            raise RuntimeError("Synthetic method names changed values, aliases, initialization or failures")
        negative = work / "private-originals"
        shutil.copytree(originals, negative)
        receipt = change_method_flags(negative / "fixture/Provider.class", b"(Ljava/lang/Object;)Ljava/lang/Object;",
                                      original_flags, (original_flags & ~1) | 2, name=b"read-value")
        (work / "private-encoding.json").write_text(json.dumps(receipt, indent=2) + "\n")
        # Compile a real foreign caller while the original entry is public,
        # then change only that callee's access flags. Same-owner private calls
        # are supported; the foreign invocation must keep failing closed.
        foreign_source = work / "foreign-source/Foreign.kt"
        foreign_source.parent.mkdir()
        foreign_source.write_text("package negative\nfun call(value: Any?): Any? = fixture.Provider.read(value)\n")
        foreign = work / "foreign-classes"
        foreign.mkdir()
        verify.run([verify.java, "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (stdlib, originals))),
                    "-jvm-target", "1.8", "-d", foreign, foreign_source], "kotlinc-foreign")
        foreign_records = compiler_facts(foreign)
        foreign_method = "Lnegative/ForeignKt;->call(Ljava/lang/Object;)Ljava/lang/Object;"
        if (set(foreign_records) != {"Lnegative/ForeignKt;"}
                or set(foreign_records["Lnegative/ForeignKt;"]["methods"]) != {foreign_method}):
            raise RuntimeError("Original foreign-scope negative changed its definitions")
        private_dex = work / "private-dex"
        private_dex.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", private_dex,
                    *[p for p in sorted(negative.rglob("*.class"))
                      if "L" + p.relative_to(negative).as_posix()[:-6] + ";" in classes],
                    *sorted(foreign.rglob("*.class"))], "d8-private")
        negative_dex = private_dex / "classes.dex"
        if list(private_dex.glob("classes*.dex")) != [negative_dex]:
            raise RuntimeError("Foreign-scope negative changed its DEX partition")
        rejected = work / "private-rejected"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), *map(str, classpath), "--timeout", "600", "-o", str(rejected)],
                                text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "private-recovery.log").write_text(result.stdout + result.stderr)
        if (result.returncode == 0 or rejected.exists()
                or "private source static invocation requires its exact declaring-class scope" not in result.stderr):
            raise RuntimeError("Foreign private illegal method call was not rejected")
        changed_source = work / "wrong-effect-source/Provider.kt"
        changed_source.parent.mkdir()
        original_source = kotlin_sources[0].read_text()
        marker = "fun touch(value: Any?) { Trace.event(3) }"
        if original_source.count(marker) != 1:
            raise RuntimeError("Owned void-call witness changed")
        changed_source.write_text(original_source.replace(marker, "fun touch(value: Any?) { Trace.event(2) }"))
        wrong = work / "wrong-effect-classes"
        wrong.mkdir()
        verify.run([verify.java, "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (stdlib, original_java))),
                    "-jvm-target", "1.8", "-d", wrong, changed_source], "kotlinc-wrong-effect")
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness, *dependencies))),
                                 "SyntheticMethodHarness", "1", "1", "0", "read-value"], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-effect-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent oracle did not detect a misrouted void call")
        if args.package_default:
            marker = "delta: Long = 0L"
            if original_source.count(marker) != 1:
                raise RuntimeError("Original package default source anchor changed")
            wrong_source = work / "wrong-default-source/Provider.kt"
            wrong_source.parent.mkdir()
            wrong_source.write_text(original_source.replace(marker, "delta: Long = 1L"))
            wrong_default = work / "wrong-default-classes"
            wrong_default.mkdir()
            verify.run([verify.java, "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                        "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (stdlib, original_java))),
                        "-jvm-target", "1.8", "-d", wrong_default, wrong_source], "kotlinc-wrong-default")
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong_default, originals, harness, *dependencies))),
                                     "SyntheticMethodHarness", "3", "1", "0", "read-value"], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / "wrong-default-counterexample.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "synthetic method contract" not in result.stderr:
                raise RuntimeError("Independent oracle missed the changed wide default")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs} != input_hashes
                or {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies} != dependency_hashes):
            raise RuntimeError("Original compiler, input, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "projected_methods": len(projected),
                    "dependency_box": args.dependency_box, "ordinary_static": args.ordinary_static,
                    "private_forwarder": args.private_forwarder,
                    "package_default": args.package_default,
                    "method_names_and_synthetic_flags": "differences reported",
                    "signatures_fields_runtime_annotations_and_exceptions": "unchanged",
                    "d8_class_retained_annotation_differences": "independently verified and recorded",
                    "same_code_counterexample": "rejected", "independent_wrong_effect_counterexample": "detected",
                    "initialization_order_exception_and_object_identity": "matched", "input_and_oracle_bytes": "unchanged"}
        if args.package_default:
            evidence["independent_wrong_default_counterexample"] = "detected"
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
