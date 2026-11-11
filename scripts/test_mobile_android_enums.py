#!/usr/bin/env python3
"""Compare enum initialization, dispatch and JVM enum contracts after recovery."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import json
import hashlib
import os
import subprocess
from pathlib import Path
import shutil
import tempfile

try:
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--android-jar", type=Path)
    variants = parser.add_mutually_exclusive_group()
    variants.add_argument("--named-constant", action="store_true")
    variants.add_argument("--constant-annotations", action="store_true")
    variants.add_argument("--recursive-bounds", action="store_true")
    variants.add_argument("--inherited-static-field", action="store_true")
    args = parser.parse_args()
    if args.recursive_bounds and not args.android_jar:
        parser.error("--recursive-bounds requires --android-jar for Enum's generic declaration")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-enum-contracts-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        inputs = sorted((FIXTURES / "enums/java").rglob("*.java"))
        if args.inherited_static_field:
            source = work / "owned-source/fixture"
            source.mkdir(parents=True)
            text = inputs[0].read_text().replace(
                "public int score(int value) { return value - bias(); }",
                "public int score(int value) { return value - bias(); }\n"
                "        public Mode peer() { return BASE; }")
            at = text.rindex("}")
            text = text[:at] + "    public Mode peer() { return CUSTOM; }\n" + text[at:]
            (source / "Mode.java").write_text(text)
            inputs = sorted(source.glob("*.java"))
        if args.recursive_bounds:
            source = work / "owned-source/fixture"
            source.mkdir(parents=True)
            (source / "Mode.java").write_text(inputs[0].read_text().replace(
                "private final int bias;", "public static EntryContract<Mode> entries;\n    private final int bias;"))
            (source / "EntryContract.java").write_text('''package fixture;
public interface EntryContract<E extends Enum<E>> { E get(); }
''')
            inputs = sorted(source.glob("*.java"))
        if args.named_constant:
            # Independently authored Java behavior oracle. The owned smali
            # deliberately uses a named member implementation; its metadata
            # differences must be reported.
            source = work / "owned-source/fixture"
            source.mkdir(parents=True)
            text = inputs[0].read_text().replace(
                "public int score(int value) { return value - bias(); }",
                "public int score(int value) { return adjust(value); }\n"
                "        private int adjust(int value) { return value - bias(); }")
            (source / "Mode.java").write_text(text)
            inputs = sorted(source.glob("*.java"))
        if args.constant_annotations:
            source = work / "owned-source/fixture"
            source.mkdir(parents=True)
            (source / "Mode.java").write_text(inputs[0].read_text().replace(
                '    BASE(3),', '    @Stamp(value = "owned-note", codes = {3, 7}) BASE(3),'))
            (source / "Stamp.java").write_text('''package fixture;
@java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.RUNTIME)
@java.lang.annotation.Target(java.lang.annotation.ElementType.FIELD)
public @interface Stamp {
    String value();
    int[] codes();
}
''')
            inputs = sorted(source.glob("*.java"))
        harness = FIXTURES / "harness/EnumConstantsHarness.java"
        if args.recursive_bounds:
            modified = work / harness.name
            content = harness.read_text().replace('main(String[] args) {', 'main(String[] args) throws Exception {')
            content = content.replace('        System.out.println("enum-contract:matched");', '''        java.lang.reflect.ParameterizedType use =
                (java.lang.reflect.ParameterizedType) Mode.class.getField("entries").getGenericType();
        java.lang.reflect.TypeVariable<?> formal = fixture.EntryContract.class.getTypeParameters()[0];
        java.lang.reflect.ParameterizedType bound = (java.lang.reflect.ParameterizedType) formal.getBounds()[0];
        if (use.getRawType() != fixture.EntryContract.class || use.getActualTypeArguments()[0] != Mode.class
                || bound.getRawType() != Enum.class || bound.getActualTypeArguments()[0] != formal)
            throw new AssertionError("Recursive enum bound changed");
        System.out.println("enum-contract:matched");''')
            modified.write_text(content)
            harness = modified
        if args.constant_annotations:
            modified = work / harness.name
            text = harness.read_text().replace('main(String[] args) {', 'main(String[] args) throws Exception {')
            text = text.replace('        System.out.println("enum-contract:matched");', '''        fixture.Stamp stamp = Mode.class.getField("BASE").getAnnotation(fixture.Stamp.class);
        if ((stamp != null) != Boolean.parseBoolean(args[0]))
            throw new AssertionError("Annotation projection was not explicit");
        if (stamp != null && (!stamp.value().equals("owned-note")
                || !java.util.Arrays.equals(stamp.codes(), new int[]{3, 7})))
            throw new AssertionError("Original annotation payload changed");
        if (!java.util.Arrays.equals(fixture.Stamp.class.getAnnotation(java.lang.annotation.Target.class).value(),
                new java.lang.annotation.ElementType[]{java.lang.annotation.ElementType.TYPE}))
            throw new AssertionError("Annotation target policy changed");
        System.out.println("enum-contract:matched");
        System.out.println("annotation=" + (stamp != null));''')
            modified.write_text(text)
            harness = modified
        if args.inherited_static_field:
            modified = work / harness.name
            content = harness.read_text().replace("main(String[] args) {", "main(String[] args) throws Throwable {")
            content = content.replace("public class EnumConstantsHarness {", "public class EnumConstantsHarness {\n    private static Mode frozenPeer(Mode value) { return value.peer(); }")
            content = content.replace('        System.out.println("enum-contract:matched");', '''        for (int attempt = 0; attempt < 2; attempt++)
            for (Mode receiver : new Mode[]{Mode.BASE, Mode.CUSTOM})
                for (int route = 0; route < 4; route++) {
                    Mode expected = receiver == Mode.BASE ? Mode.CUSTOM : Mode.BASE;
                    Mode actual = route == 0 ? receiver.peer() : route == 1 ? frozenPeer(receiver)
                        : route == 2 ? (Mode)Mode.class.getMethod("peer").invoke(receiver)
                        : (Mode)java.lang.invoke.MethodHandles.publicLookup().findVirtual(Mode.class, "peer",
                            java.lang.invoke.MethodType.methodType(Mode.class)).invokeExact(receiver);
                    if (actual != expected) throw new AssertionError("enum inherited static field changed");
                }
        System.out.println("enum-contract:matched");''')
            modified.write_text(content)
            harness = modified
        originals, baseline_dir, rebuilt = (work / name for name in ("originals", "baseline", "rebuilt"))
        verify.compile_local(inputs, originals)
        classes, methods = compiler_inventory(verify, originals)
        verify.compile_local([*inputs, harness], baseline_dir)
        if args.constant_annotations:
            # The JVM permits stored annotations outside their source target.
            # Recompile only the owned definition to create that bytecode
            # shape; leave the annotated enum and its method bodies intact.
            stamp = source / "Stamp.java"
            stamp.write_text(stamp.read_text().replace('ElementType.FIELD', 'ElementType.TYPE'))
            retargeted = work / "retargeted"
            verify.compile_local([stamp], retargeted)
            for target in (originals, baseline_dir):
                shutil.copy2(retargeted / "fixture/Stamp.class", target / "fixture/Stamp.class")
        def frozen_inputs():
            return {str(p.relative_to(work)): hashlib.sha256(p.read_bytes()).hexdigest()
                    for tree in (originals, baseline_dir) for p in sorted(tree.rglob("*")) if p.is_file()}
        if args.inherited_static_field:
            frozen = frozen_inputs()
            (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        baseline = verify.run([verify.java, "-cp", baseline_dir, "EnumConstantsHarness",
                               *(["true"] if args.constant_annotations else [])], "baseline")
        if args.inherited_static_field:
            for label, replacement in (("lost-field", "null"), ("wrong-field", "CUSTOM"), ("receiver-instead", "this")):
                wrong_source = work / (label + "-source") / "Mode.java"
                wrong_source.parent.mkdir()
                text = inputs[0].read_text()
                needle = "public Mode peer() { return BASE; }"
                if text.count(needle) != 1: raise RuntimeError("Owned enum field counterexample anchor changed")
                wrong_source.open("x").write(text.replace(needle, "public Mode peer() { return " + replacement + "; }"))
                wrong = work / (label + "-classes")
                verify.compile_local([wrong_source], wrong)
                result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, baseline_dir))),
                    "EnumConstantsHarness"], capture_output=True, text=True, timeout=300, env=verify.environment)
                (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
                if result.returncode == 0 or "enum inherited static field changed" not in result.stderr:
                    raise RuntimeError("Immutable enum field observer missed wrong behavior: " + label)
        dex = (FIXTURES / "enums/named" if args.named_constant else
               verify.dex(sorted(originals.rglob("*.class")), work / "dex", originals))
        output = work / "recovered"
        classpath = ["--classpath", args.android_jar.resolve()] if args.recursive_bounds else []
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if (coverage["status"] != "partial" or coverage["unrecovered_method_count"] != 0
                or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods)
                or {row["identity"] for row in coverage["methods"]} !=
                   {ref.replace("Mode$1;", "Mode$Special;") if args.named_constant else ref
                    for ref in methods}):
            raise RuntimeError("Enum projection lost original classes, methods or conservative coverage")
        kinds = {row.get("projection_kind") for row in coverage["methods"]}
        required = {"enum-constructor", "enum-initializer", "enum-values", "enum-valueOf",
                    "enum-constant-constructor", "enum-constant-body", "enum-access-constructor"}
        if not required.issubset(kinds):
            raise RuntimeError("Compiler-managed enum methods have no explicit projection evidence")
        bindings = coverage["class_source_bindings"]
        enum = next(row for row in bindings if row["binding_kind"] == "enum-declaration")
        constant = next(row for row in bindings if row["binding_kind"] == "enum-constant-body")
        if ([(row["field"], row["ordinal"]) for row in enum["constants"]]
                != [("BASE", 0), ("CUSTOM", 1)]
                or enum["compiler_generated_members_status"] != "unverified"
                or constant["constant"] != "CUSTOM"
                or constant["constructor_descriptor_status"] != "unverified"
                or constant["binary_name_status"] != ("changed" if args.named_constant else "unverified")):
            raise RuntimeError("Enum constant identity or compiler boundary is not recorded")
        if args.constant_annotations:
            first = enum["constants"][0]
            annotations = first.get("original_annotations", [])
            if (first.get("annotation_status") != "report-only" or len(annotations) != 1
                    or annotations[0]["type"] != "Lfixture/Stamp;" or annotations[0]["visibility"] != 1
                    or 'value = "owned-note"' not in annotations[0]["source_declaration"]
                    or 'codes = {0x00000003, 0x00000007}' not in annotations[0]["source_declaration"]
                    or not first.get("metadata_differences")):
                raise RuntimeError("Unrepresentable enum annotation lost its original payload or projection reason")
            if "@fixture.Stamp" in (output / "sources/fixture/Mode.java").read_text():
                raise RuntimeError("Invalid-target annotation was emitted as Java")
        verify.compile_local([*sorted((output / "sources").rglob("*.java")),
                              *([] if args.inherited_static_field else [harness])], rebuilt)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        if not set(classes).issubset(rebuilt_classes) or not set(methods).issubset(rebuilt_methods):
            raise RuntimeError("Owned enum fixture did not retain every original descriptor")
        runtime = os.pathsep.join(map(str, (rebuilt, baseline_dir))) if args.inherited_static_field else rebuilt
        actual = verify.run([verify.java, "-cp", runtime, "EnumConstantsHarness",
                             *(["false"] if args.constant_annotations else [])], "recovered-execution")
        if args.constant_annotations:
            before, after = baseline.splitlines(), actual.splitlines()
            if before[-1] != "annotation=true" or after[-1] != "annotation=false":
                raise RuntimeError("Enum annotation reflection differs from the report")
            baseline, actual = "\n".join(before[:-1]), "\n".join(after[:-1])
        if baseline != actual:
            raise RuntimeError("Recovered enum changed observable JVM behavior")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "coverage": "partial", "jvm_output": "matched", "integer_probes": 5,
                    "enum_identity_order_dispatch": "matched", "values_array_isolation": "matched",
                    "valueOf_exceptions": "matched", "EnumSet": "matched",
                    "constant_subclass_reflection": "matched", "original_descriptors": "retained"}
        if args.named_constant:
            evidence.update(constant_subclass_reflection="named-to-anonymous-difference-recorded",
                            original_descriptors="all-accounted-for-with-explicit-name-mapping")
        if args.constant_annotations:
            evidence["enum_field_annotation_payload"] = "retained-in-report"
            evidence["declared_annotation_targets"] = "retained"
            evidence["reflection_difference"] = "confirmed"
        if args.recursive_bounds:
            evidence["recursive_enum_generic_bound"] = "matched"
        if args.inherited_static_field:
            if frozen_inputs() != frozen: raise RuntimeError("Owned enum originals or immutable callers changed")
            evidence.update(inherited_static_enum_field_identity="matched", immutable_caller_routes=4,
                            independent_wrong_programs=3, original_inputs_and_callers="unchanged")
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
