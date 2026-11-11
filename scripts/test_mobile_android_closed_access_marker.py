#!/usr/bin/env python3
"""Verify closed access constructors retain raw forwarding and disclosed marker metadata."""
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
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-inputs", action="store_true")
    parser.add_argument("--stateful-marker", action="store_true")
    parser.add_argument("--checked-throws", action="store_true")
    parser.add_argument("--nested-owner", action="store_true")
    parser.add_argument("--package-element", action="store_true")
    args = parser.parse_args()
    if args.package_element and (args.stateful_marker or args.nested_owner or args.checked_throws):
        parser.error("--package-element is a separate owned scope profile")
    if args.nested_owner and not args.stateful_marker:
        parser.error("--nested-owner requires --stateful-marker")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-closed-access-marker-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        fixture = FIXTURES / "closed_access_marker/java"
        if args.stateful_marker:
            staged = work / "stateful-inputs"
            shutil.copytree(fixture, staged)
            variant = FIXTURES / "closed_access_marker/variants/stateful/java"
            for source in variant.rglob("*.java"):
                shutil.copy2(source, staged / source.relative_to(variant))
            fixture = staged
        if args.nested_owner:
            variant = FIXTURES / "closed_access_marker/variants/nested/java"
            for source in variant.rglob("*.java"):
                shutil.copy2(source, fixture / source.relative_to(variant))
        if args.checked_throws:
            staged = work / "checked-inputs"
            shutil.copytree(fixture, staged)
            replacements = {
                "Core": ("Probe probe) {", "Probe probe) throws java.io.IOException {", 2),
                "Context": ("RuntimeException failure", "java.io.IOException failure", 1),
                "Probe": ("Context context);", "Context context) throws java.io.IOException;", 1)}
            for owner, (before, after, count) in replacements.items():
                path = staged / ("fixture/" + owner + ".java")
                text = path.read_text()
                if text.count(before) != count:
                    raise RuntimeError("Original checked constructor fixture anchor changed: " + owner)
                path.write_text(text.replace(before, after))
            path = staged / "fixture/Core.java"
            text = path.read_text()
            if args.nested_owner:
                before = "public Object observe(Object self, Object values, Context context) {"
                if text.count(before) != 1:
                    raise RuntimeError("Original nested callback anchor changed")
                text = text.replace(before, before[:-1] + "throws java.io.IOException {")
            else:
                if text.count("public final class Core {") != 1:
                    raise RuntimeError("Original checked constructor owner anchor changed")
                text = text.replace("public final class Core {",
                    "public final class Core implements Probe {\n"
                    "    public Object observe(Core self, Object values, Context context) throws java.io.IOException { return values; }")
            path.write_text(text)
            fixture = staged
        if args.package_element:
            staged = work / "package-element-inputs"
            shutil.copytree(fixture, staged)
            variant = FIXTURES / "closed_access_marker/variants/package_element/java"
            for source in variant.rglob("*.java"):
                shutil.copy2(source, staged / source.relative_to(variant))
            fixture = staged
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        harness_name = "StatefulAccessMarkerHarness" if args.stateful_marker else "ClosedAccessMarkerHarness"
        if args.nested_owner:
            harness_name = "NestedAccessMarkerHarness"
        if args.package_element:
            harness_name = "PackageElementAccessMarkerHarness"
        harness_source = FIXTURES / "harness" / (harness_name + ".java")
        if args.checked_throws:
            text = harness_source.read_text()
            replacements = {
                "RuntimeException callbackFailure = new RuntimeException(), targetFailure = new RuntimeException();":
                    "java.io.IOException callbackFailure = new java.io.IOException(), targetFailure = new java.io.IOException();",
                "public Object observe(Core self, Object actualValues, Context actualContext) {":
                    "public Object observe(Core self, Object actualValues, Context actualContext) throws java.io.IOException {",
                "access.setAccessible(true);": "access.setAccessible(true);\n"
                    "        Class<?>[] checked = {java.io.IOException.class};\n"
                    "        require(Arrays.equals(access.getExceptionTypes(), checked));\n"
                    "        require(Probe.class.isAssignableFrom(Core.class));\n"
                    "        require(Arrays.equals(Core.class.getDeclaredConstructor(Set.class, Context.class, Probe.class).getExceptionTypes(), checked));\n"
                    "        require(Arrays.equals(Core.Builder.class.getMethod(\"create\", Set.class, Context.class, Probe.class).getExceptionTypes(), checked));\n"
                    "        require(Arrays.equals(Probe.class.getMethod(\"observe\", Core.class, Object.class, Context.class).getExceptionTypes(), checked));"}
            if args.nested_owner:
                replacements = {before.replace("Core self", "Object self"): after
                    .replace("Core self", "Object self")
                    .replace("Core.class", "entryType")
                    .replace("\"observe\", entryType", "\"observe\", Object.class")
                    for before, after in replacements.items()}
            for before, after in replacements.items():
                if text.count(before) != 1:
                    raise RuntimeError("Original checked constructor observer anchor changed")
                text = text.replace(before, after)
            harness_source = work / "checked-harness" / (harness_name + ".java")
            harness_source.parent.mkdir()
            harness_source.open("x").write(text)
        verify.compile_local([harness_source], harness,
                             classpath=originals, release=8)
        if args.package_element:
            harness_name = "fixture." + harness_name
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        (work / "baseline-cli-sha256.txt").open("x").write(sha(args.neverd.resolve()) + "\n")
        records = class_identity.compiler_classes(originals)
        exceptions = exception_inventory(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        expected_classes, expected_methods = (6, 14) if args.stateful_marker else (5, 6)
        if args.package_element:
            expected_classes += 1
            expected_methods += 2
        if args.nested_owner:
            expected_classes += 1
            expected_methods += 2
        elif args.checked_throws:
            expected_methods += 1
        if (len(all_classes), len(all_methods)) != (expected_classes, expected_methods):
            raise RuntimeError("Owned closed access marker inventory changed")
        excluded = {"Lfixture/Context;", "Lfixture/Probe;"} if args.dependency_inputs else set()
        if args.stateful_marker:
            excluded.add("Lfixture/Kind;")
            marker = records["Lfixture/Core$1;"]
            if (len(marker["fields"]) != 1 or len(marker["methods"]) != 1
                    or marker["methods"]["Lfixture/Core$1;-><clinit>()V"]["code"] is not True):
                raise RuntimeError("Original stateful marker lacks its static initialization body")
        classes = all_classes - excluded
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        sdk = work / "sdk"
        sdk.mkdir()
        dependencies = []
        if excluded:
            jar = work / "owned-inputs.jar"
            with zipfile.ZipFile(jar, "x") as archive:
                for owner in sorted(excluded):
                    name = owner[1:-1] + ".class"
                    path = sdk / name
                    path.parent.mkdir(exist_ok=True)
                    shutil.copyfile(originals / name, path)
                    archive.writestr(name, path.read_bytes())
            dependencies.append(jar)
        input_hashes = {str(p): sha(p) for p in [args.android_jar.resolve(), *dependencies]}
        classpath = [part for p in [args.android_jar.resolve(), *dependencies]
                     for part in ("--classpath", str(p))]

        def execute(runtime, label, marker_name="fixture.Core$1", rebuilt=False):
            outputs = []
            for route in range(4):
                for mode in range(18 if args.stateful_marker else 17 if args.package_element else 16):
                    result = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                         harness_name, str(route), str(mode), marker_name, str(rebuilt).lower()], f"{label}-{route}-{mode}")
                    stage = 1 if mode in (3, 4, 5, 14) or (args.package_element and mode == 16) else 2 if mode in (6, 7) else 3
                    expected = [f"route={route},mode={mode},attempt={attempt},stage={stage}" for attempt in range(2)]
                    if result.splitlines() != expected:
                        raise RuntimeError("Independent final-field/effect oracle changed: " + result)
                    outputs.append(result)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline) + "\n")
        paths = [p.relative_to(originals) for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]

        def dex(root, name, declarations=None):
            destination = work / name
            destination.mkdir()
            declared = classpath if declarations is None else declarations
            verify.run([*verify.d8, "--debug", "--min-api", "24", *declared, "--output", destination,
                        *[root / p for p in paths]], name)
            return destination / "classes.dex"

        source_dex = dex(originals, "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original closed access marker entry")
        (work / "inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage["status"] != "partial" or coverage["projected_method_count"] != (4 if args.stateful_marker else 2)
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Original closed access marker entries or projection accounting changed")
        expected_projected = {
            "Lfixture/Core;-><init>(Ljava/util/Set;Lfixture/Context;Lfixture/Probe;Lfixture/Core$1;)V",
            "Lfixture/Core$Builder;->create(Ljava/util/Set;Lfixture/Context;Lfixture/Probe;)Lfixture/Core;"}
        if args.nested_owner:
            expected_projected = {
                "Lfixture/Core$Entry;-><init>(Ljava/util/Set;Lfixture/Context;Lfixture/Probe;Lfixture/Core$1;)V",
                "Lfixture/Core$Builder;->create(Ljava/util/Set;Lfixture/Context;Lfixture/Probe;)Ljava/lang/Object;"}
        if args.stateful_marker:
            expected_projected |= {"Lfixture/Core;->choose(I)I", "Lfixture/Core$1;-><clinit>()V"}
        if ({identity for identity, row in rows.items() if row["status"] == "source-projected"} != expected_projected
                or any(rows[identity].get("projection_kind") != "anonymous-to-named-member" for identity in expected_projected)):
            raise RuntimeError("Anonymous marker mapping changed an unrelated method role")
        bindings = coverage["class_source_bindings"]
        if (len(bindings) != 1 or bindings[0]["class"] != "Lfixture/Core$1;"
                or bindings[0]["binding_kind"] != "anonymous-to-named-member"
                or bindings[0]["binary_name_status"] != "changed"
                or len(bindings[0]["metadata_differences"]) < 5):
            raise RuntimeError("Missing complete constructorless marker source mapping")
        binding = bindings[0]
        replacement = binding["source_enclosing_class"][:-1] + "$" + binding["source_name"] + ";"
        marker_name = replacement[1:-1].replace("/", ".")
        def mapped(value):
            if isinstance(value, str): return value.replace(binding["class"], replacement)
            if isinstance(value, list): return [mapped(v) for v in value]
            if isinstance(value, dict): return {mapped(k): mapped(v) for k, v in value.items()}
            return value
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=sdk if excluded else None, release=8)
        if args.stateful_marker:
            recipe = coverage.get("java_rebuild", {})
            fields = coverage.get("field_source_bindings", [])
            original_fields = records["Lfixture/Core$1;"]["fields"]
            original_field = next(iter(original_fields))
            field = original_fields[original_field]
            if (not coverage.get("requires_java_rebuild_postprocessing")
                    or recipe.get("application_code_execution") is not False
                    or recipe.get("utility_source") != "rebuild/RestoreFieldFlags.java"
                    or recipe.get("field_manifest") != "rebuild/runtime-fields.tsv"
                    or len(fields) != 1 or fields[0].get("class") != binding["class"]
                    or fields[0].get("binding_kind") != "static-synthetic-field"
                    or fields[0].get("original_identity") != original_field
                    or fields[0].get("source_identity") != mapped(original_field)
                    or fields[0].get("required_postcompile_flags") != "final,synthetic"
                    or not fields[0].get("metadata_differences") or field["access"] != 0x1018):
                raise RuntimeError("Missing the exact static synthetic field restoration contract")
            manifest = output / recipe["field_manifest"]
            row = replacement[1:-1] + "\t" + field["name"] + "\t" + field["descriptor"] + "\tfinal,synthetic\n"
            if manifest.read_text() != row:
                raise RuntimeError("Field manifest lost the actual named compiler identity")
            before_bytes = {p.relative_to(rebuilt).as_posix(): p.read_bytes() for p in rebuilt.rglob("*.class")}
            missing = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness, sdk))),
                harness_name, "0", "0", marker_name, "true"], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "missing-restoration.log").open("x").write(missing.stdout + missing.stderr)
            if missing.returncode == 0 or "owned closed access marker" not in missing.stderr:
                raise RuntimeError("Frozen observer did not require the original synthetic field flag")
            tool = work / "field-metadata-tool"
            verify.compile_local([output / recipe["utility_source"]], tool, release=8)
            command = [verify.java, "-cp", tool, "RestoreFieldFlags", rebuilt, manifest]
            verify.run(command, "restore-static-field")
            after_bytes = {p.relative_to(rebuilt).as_posix(): p.read_bytes() for p in rebuilt.rglob("*.class")}
            changed_class = replacement[1:-1] + ".class"
            if (set(before_bytes) != set(after_bytes)
                    or {k for k in before_bytes if before_bytes[k] != after_bytes[k]} != {changed_class}
                    or any(method_code(before_bytes[k]) != method_code(after_bytes[k]) for k in before_bytes)
                    or len(before_bytes[changed_class]) != len(after_bytes[changed_class])
                    or sum(a != b for a, b in zip(before_bytes[changed_class], after_bytes[changed_class])) != 1):
                raise RuntimeError("Field restoration changed Code, layout or another definition")
            verify.run(command, "restore-static-field-idempotent")
            after_hashes = hashes(rebuilt)
            if after_bytes != {p.relative_to(rebuilt).as_posix(): p.read_bytes() for p in rebuilt.rglob("*.class")}:
                raise RuntimeError("Static field restoration is not idempotent")
            bad_rows = (row.replace("final,synthetic", "final"), row + row,
                        row.replace(field["name"], "missing"), row.replace("\t[I\t", "\t[J\t"))
            for index, bad in enumerate(bad_rows):
                bad_manifest = work / ("bad-field-manifest-" + str(index) + ".tsv")
                bad_manifest.open("x").write(bad)
                rejected = subprocess.run([verify.java, "-cp", tool, "RestoreFieldFlags", rebuilt, bad_manifest],
                    text=True, capture_output=True, timeout=300, env=verify.environment)
                (work / ("bad-field-manifest-" + str(index) + ".log")).open("x").write(rejected.stdout + rejected.stderr)
                if rejected.returncode == 0 or hashes(rebuilt) != after_hashes:
                    raise RuntimeError("Invalid static field recipe changed an application class")
            (work / "field-restoration.json").open("x").write(json.dumps({
                "status": "verified", "original_field": original_field, "source_field": mapped(original_field),
                "all_Code_and_other_bytes": "unchanged", "idempotent": True,
                "missing_restoration": "rejected_by_frozen_observer", "invalid_recipes_rejected": len(bad_rows),
                "application_code_execution_by_restoration": False}, indent=2) + "\n")
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helper_rows = coverage["generated_source_helpers"]
        helpers = {mapped(r["class"] + "->" + r["name"] + r["prototype"]) for r in helper_rows}
        if (actual_classes != {mapped(c) for c in classes}
                or set(actual_methods) != {mapped(m) for m in methods} | helpers):
            raise RuntimeError("Closed access marker original definitions or helper roles changed")
        defaults = [r for r in helper_rows if r["kind"] == "default-constructor"]
        if len(defaults) != 1 or defaults[0]["class"] != binding["class"]:
            raise RuntimeError("Marker's sole added private constructor is not recorded")
        if any(r["kind"] not in ("throw-helper", "default-constructor")
               or r["kind"] == "throw-helper" and r.get("static") is not True for r in helper_rows):
            raise RuntimeError("Unexpected closed access marker generated helper")
        after = class_identity.compiler_classes(rebuilt)
        after_exceptions = exception_inventory(rebuilt)
        differences = {}
        for owner in classes:
            before_record = mapped(records[owner]); after_record = after[mapped(owner)]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before_record.get(key) != after_record.get(key):
                    if owner != binding["class"] or key not in ("access", "inner_class", "enclosing_method"):
                        raise RuntimeError("Undisclosed original class metadata changed: " + owner + ":" + key)
                    if key == "enclosing_method" and (before_record[key] != {
                            "owner": binding["source_enclosing_class"], "name": None, "prototype": None}
                            or after_record.get(key) is not None):
                        raise RuntimeError("Unexpected anonymous-to-member enclosure metadata")
                    differences[owner + ":" + key] = [before_record.get(key), after_record.get(key)]
            for identity, metadata in records[owner]["methods"].items():
                expected = mapped(metadata)
                if metadata["access"] & 0x1000:
                    expected["access"] &= ~0x1000
                    if (not rows[identity].get("metadata_differences")
                            or not any("synthetic" in d for d in rows[identity]["metadata_differences"])):
                        raise RuntimeError("Synthetic constructor flag change lacks a report receipt")
                    differences[identity + ":access"] = [metadata["access"], expected["access"]]
                if (after_record["methods"].get(mapped(identity)) != expected
                        or after_exceptions[mapped(identity)] != mapped(exceptions[identity])):
                    raise RuntimeError("Undisclosed original method metadata or Exceptions changed: " + identity)
        marker = after[replacement]
        if (marker["access"] & 0x1000 or marker["inner_class"]["name"] != binding["source_name"]
                or marker["inner_class"]["outer"] != binding["source_enclosing_class"]):
            raise RuntimeError("Rebuilt marker differs from its exact named binding")
        (work / "disclosed-metadata-differences.json").open("x").write(json.dumps(differences, indent=2) + "\n")
        if execute((rebuilt, harness, *([sdk] if excluded else [])), "recovered", marker_name, True) != baseline:
            raise RuntimeError("Original raw forwarding, partial state or exact failure changed")
        controls = []
        wrongs = [('lost-values', 'Core', 'this.values = values;', 'this.values = null;', 0, 0), ('lost-context', 'Core', 'this.context = context;', 'this.context = null;', 0, 0), ('lost-probe', 'Core', 'this.probe = probe;', 'this.probe = null;', 0, 0), ('lost-marker', 'Core', 'marker = probe.observe(this, values, context);', 'probe.observe(this, values, context); marker = null;', 0, 0), ('duplicate-probe', 'Core', 'marker = probe.observe(this, values, context);', 'probe.observe(this, values, context); marker = probe.observe(this, values, context);', 0, 0), ('early-failure', 'Core', 'marker = probe.observe(this, values, context);', 'if (context != null && context.failure != null) throw context.failure; marker = probe.observe(this, values, context);', 6, 0), ('lost-completion', 'Core', 'stage = 3;', 'stage = 0;', 0, 0), ('wrong-factory-values', 'Core', 'return new Core(values, context, probe);', 'return new Core(null, context, probe);', 0, 0)]
        if args.stateful_marker:
            wrongs += [
                ("early-marker-initialization", "Core", "latest = this;", "choose(0); latest = this;", 0, 0),
                ("lost-marker-initialization", "Context", "initializations++;", ";", 0, 0),
                ("repeated-marker-initialization", "Core", "switch (Kind.values()[index])", "Context.initialize(); switch (Kind.values()[index])", 0, 0),
                ("wrong-switch-value", "Core", "case FIRST: return 11;", "case FIRST: return 22;", 0, 0),
                ("lost-initializer-failure", "Context", "initializer.run();", ";", 16, 0)]
        if args.checked_throws:
            wrongs += [
                ("wrapped-checked-failure", "Core", "throw context.failure;",
                 "throw new IllegalStateException(context.failure);", 6, 0),
                ("caught-callback-failure", "Core", "marker = probe.observe(this, values, context);",
                 "try { marker = probe.observe(this, values, context); } catch (java.io.IOException failure) { marker = null; }", 4, 0)]
        if args.nested_owner:
            wrongs = [(name, owner, before.replace("new Core(", "new Entry("),
                       changed.replace("new Core(", "new Entry("), mode, route)
                      for name, owner, before, changed, mode, route in wrongs]
            wrongs.append(("lost-private-member", "Core", "private static class Entry",
                           "public static class Entry", 0, 0))
        if args.package_element:
            wrongs += [
                ("public-element", "Core", "static final class Value", "public static final class Value", 0, 0),
                ("wrong-element", "Core", "return number;", "return number + 1;", 0, 0),
                ("wrapped-opaque-checked", "Core", "marker = probe.observe(this, values, context);",
                 "try { marker = probe.observe(this, values, context); } catch (Throwable failure) { throw new IllegalStateException(failure); }", 16, 0)]
        for name, owner, before, changed, mode, route in wrongs:
            altered = work / (name + "-wrong-source")
            shutil.copytree(fixture, altered)
            path = altered / ("fixture/" + owner + ".java")
            text = path.read_text()
            assert text.count(before) == 1
            path.write_text(text.replace(before, changed))
            wrong = work / (name + "-wrong-classes")
            verify.compile_local(sorted(altered.rglob("*.java")), wrong, release=8)
            result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (wrong, harness))),
                                     harness_name, str(route), str(mode), "fixture.Core$1", "false"], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (name + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned closed access marker" not in result.stderr:
                raise RuntimeError("Frozen caller missed independently compiled wrong final-field flow: " + name)
        if args.package_element:
            signature = b"(Ljava/util/Set<Lfixture/Core$Value;>;Lfixture/Context;Lfixture/Probe;)V"
            changes = (
                ("unknown-element", signature.replace(b"Core$Value", b"Core$Missing")),
                ("extra-slot", signature.replace(b"Core$Value;>", b"Core$Value;Ljava/lang/Object;>")),
                ("foreign-formal", signature.replace(b"Lfixture/Core$Value;", b"TUnknown;")),
                ("changed-erasure", signature.replace(b"java/util/Set", b"java/util/List")),
            )
            for label, changed in changes:
                altered = work / (label + "-originals")
                shutil.copytree(originals, altered)
                target = altered / "fixture/Core.class"
                code = method_code(target.read_bytes())
                receipt = change_signature(target, signature, changed)
                if method_code(target.read_bytes()) != code:
                    raise RuntimeError("Owned member Signature control changed Code")
                negative_dex = dex(altered, label + "-dex")
                rejected = work / (label + "-rejected")
                result = subprocess.run([args.neverd.resolve(), "mobile", negative_dex, *classpath, "-o", rejected],
                    text=True, capture_output=True, timeout=300, env=verify.environment)
                (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
                (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
                if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                    raise RuntimeError("Unproved owned member constructor Signature accepted: " + label)
                controls.append((label, receipt))
        if ({"originals": hashes(originals), "harness": hashes(harness)} != frozen
                or any(sha(Path(p)) != value for p, value in input_hashes.items())):
            raise RuntimeError("Frozen owned factory inputs changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_inputs": args.dependency_inputs, "stateful_marker": args.stateful_marker,
                    "checked_throws": args.checked_throws, "nested_owner": args.nested_owner,
                    "package_element": args.package_element,
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "same_code_counterexamples": [row[0] for row in controls],
                    "independent_compiler_counterexamples": len(wrongs)}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
