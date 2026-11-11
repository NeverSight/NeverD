#!/usr/bin/env python3
"""Compare constructor field order and register state against a Java 25 oracle."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import json
from pathlib import Path
import tempfile

try:
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    variants = parser.add_mutually_exclusive_group()
    variants.add_argument("--inner-members", action="store_true",
                          help="Preserve named inner captures with an omitted generic outer parameter")
    variants.add_argument("--guarded-inner-members", action="store_true",
                          help="Preserve parameter checks before capture and superclass initialization")
    variants.add_argument("--generic-inner-members", action="store_true",
                          help="Report parameterized owner changes while preserving explicit outer values")
    variants.add_argument("--getter-inner-members", action="store_true",
                          help="Preserve one getter evaluation before super and retain its result afterward")
    variants.add_argument("--delegating-inner-members", action="store_true",
                          help="Follow captured outer parameters through aliased and wide constructor chains")
    variants.add_argument("--cast-inner-members", action="store_true",
                          help="Preserve ordered casts before super, including failed casts and null values")
    variants.add_argument("--allocating-inner-members", action="store_true",
                          help="Preserve immediate no-arg object construction before super and retained identity")
    variants.add_argument("--static-field-inner-members", action="store_true",
                          help="Preserve static field snapshots, class initialization and failure before super")
    args = parser.parse_args()
    args.inner_members |= (args.guarded_inner_members or args.generic_inner_members
                           or args.getter_inner_members or args.delegating_inner_members
                           or args.cast_inner_members or args.allocating_inner_members
                           or args.static_field_inner_members)
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-constructor-prologue-")
    with context as directory:
        work = Path(directory).resolve()
        # This smali oracle does not invoke D8 or need an Android SDK.
        verify = Verify(work, choose_jdk(args.java_home), work / "unused-d8", 300)
        fixture = FIXTURES / ("inner_members" if args.inner_members else "constructor_prologue")
        inputs = sorted((fixture / "java").rglob("*.java"))
        harness_name = "InnerMembersHarness" if args.inner_members else "ConstructorPrologueHarness"
        harness = FIXTURES / ("harness/" + harness_name + ".java")
        smali = fixture / "smali"
        if args.getter_inner_members:
            fixture = FIXTURES / "inner_members_getters"
            inputs = sorted((fixture / "java").rglob("*.java"))
            smali = fixture / "smali"
            harness_name = "GetterInnerMembersHarness"
            harness = FIXTURES / ("harness/" + harness_name + ".java")
        if args.cast_inner_members:
            fixture = FIXTURES / "inner_members_casts"
            inputs = sorted((fixture / "java").rglob("*.java"))
            smali = fixture / "smali"
            harness_name = "CastInnerMembersHarness"
            harness = FIXTURES / ("harness/" + harness_name + ".java")
        if args.allocating_inner_members:
            fixture = FIXTURES / "inner_members_allocations"
            inputs = sorted((fixture / "java").rglob("*.java"))
            smali = fixture / "smali"
            harness_name = "AllocatingInnerMembersHarness"
            harness = FIXTURES / ("harness/" + harness_name + ".java")
        if args.static_field_inner_members:
            fixture = FIXTURES / "inner_members_static_fields"
            inputs = sorted((fixture / "java").rglob("*.java"))
            smali = fixture / "smali"
            harness_name = "StaticFieldInnerMembersHarness"
            harness = FIXTURES / ("harness/" + harness_name + ".java")
        if args.generic_inner_members:
            transformed = work / "owned-input"
            inputs = []
            for source in sorted((fixture / "java").rglob("*.java")):
                content = source.read_text()
                if source.name == "InnerHost.java":
                    content = content.replace("public class InnerHost {", "public class InnerHost<T> {\n    public T data;", 1)
                    content = content.replace("public int result()", "public T payload() { return data; }\n        public int result()", 1)
                target = transformed / "java" / source.relative_to(fixture / "java")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content)
                inputs.append(target)
            smali = transformed / "smali"
            smali.mkdir()
            for source in sorted((fixture / "smali").rglob("*.smali")):
                content = source.read_text()
                if source.name == "InnerHost.smali":
                    content = content.replace(".super Ljava/lang/Object;", """.super Ljava/lang/Object;
.annotation system Ldalvik/annotation/Signature;
    value = { "<T:Ljava/lang/Object;>Ljava/lang/Object;" }
.end annotation
.field public data:Ljava/lang/Object;
    .annotation system Ldalvik/annotation/Signature;
        value = { "TT;" }
    .end annotation
.end field""", 1)
                    content = content.replace(".method public make(I)Lfixture/InnerHost$Member;", """.method public make(I)Lfixture/InnerHost$Member;
    .annotation system Ldalvik/annotation/Signature;
        value = { "(I)Lfixture/InnerHost<TT;>.Member;" }
    .end annotation""", 1)
                elif source.name == "InnerHost$Member.smali":
                    content = content.replace(".field final synthetic this$0:Lfixture/InnerHost;", """.field final synthetic this$0:Lfixture/InnerHost;
    .annotation system Ldalvik/annotation/Signature;
        value = { "Lfixture/InnerHost<TT;>;" }
    .end annotation
.end field""", 1)
                    content += """
.method public payload()Ljava/lang/Object;
    .registers 2
    .annotation system Ldalvik/annotation/Signature;
        value = { "()TT;" }
    .end annotation
    iget-object v0, p0, Lfixture/InnerHost$Member;->this$0:Lfixture/InnerHost;
    iget-object v0, v0, Lfixture/InnerHost;->data:Ljava/lang/Object;
    return-object v0
.end method
"""
                (smali / source.name).write_text(content)
            changed_harness = transformed / harness.name
            content = harness.read_text().replace("host.base = base;", """host.base = base;
                Object token = new Object();
                host.data = token;""")
            content = content.replace("Field capture =", """if (member.payload() != token)
                    throw new AssertionError("Generic owner capture changed object identity");
                Field capture =""", 1)
            content = content.replace('        System.out.println("static="', '''        System.out.println("payload=" + InnerHost.Member.class.getMethod("payload").getGenericReturnType().getTypeName());
        System.out.println("static="''', 1)
            changed_harness.write_text(content)
            harness = changed_harness
        if args.guarded_inner_members:
            transformed = work / "owned-input"
            inputs = []
            for source in sorted((fixture / "java").rglob("*.java")):
                content = source.read_text()
                if source.name == "InnerHost.java":
                    content = content.replace("public Member(int value) { offset = value; }", """public Member(int value) {
            Guard.check(value, "first");
            Guard.check(value, "second");
            super();
            offset = value;
        }""")
                elif source.name == "OuterProbe.java":
                    content = content.replace("observed = readOuter();",
                                              "Guard.trace = Guard.trace * 10 + 3; observed = readOuter();")
                target = transformed / "java" / source.relative_to(fixture / "java")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content)
                inputs.append(target)
            guarded = FIXTURES / "inner_members_guarded"
            inputs += sorted((guarded / "java").rglob("*.java"))
            smali = transformed / "smali"
            smali.mkdir()
            for source in [*sorted((fixture / "smali").rglob("*.smali")),
                           *sorted((guarded / "smali").rglob("*.smali"))]:
                content = source.read_text()
                if source.name == "InnerHost$Member.smali":
                    content = content.replace(".registers 3", ".registers 4", 1)
                    content = content.replace("    iput-object p1, p0,", """    const-string v0, "first"
    invoke-static {p2, v0}, Lfixture/Guard;->check(ILjava/lang/String;)V
    const-string/jumbo v0, "second"
    invoke-static {p2, v0}, Lfixture/Guard;->check(ILjava/lang/String;)V
    iput-object p1, p0,""", 1)
                elif source.name == "OuterProbe.smali":
                    content = content.replace("    invoke-virtual {p0},", """    sget v0, Lfixture/Guard;->trace:I
    mul-int/lit8 v0, v0, 10
    add-int/lit8 v0, v0, 3
    sput v0, Lfixture/Guard;->trace:I
    invoke-virtual {p0},""", 1)
                (smali / source.name).write_text(content)
            harness_name = "GuardedInnerMembersHarness"
            harness = FIXTURES / ("harness/" + harness_name + ".java")
        if args.delegating_inner_members:
            transformed = work / "owned-input"
            inputs = []
            for source in sorted((fixture / "java").rglob("*.java")):
                content = source.read_text()
                if source.name == "InnerHost.java":
                    content = content.replace("public final int offset;", """public final int offset;
        public long marker;
        public Member(int value, long marker) { this(value); this.marker = marker; }
        public Member(int value, long marker, Object unused) { this(value, marker); }""", 1)
                    content = content.replace("public Member make(int value)", """public Member make(int value, long marker) { return new Member(value, marker); }
    public Member make(int value)""", 1)
                elif source.name == "OuterProbe.java":
                    content = content.replace("public final int observed;", "public final int observed;\n    public static int constructions;", 1)
                    content = content.replace("observed = readOuter();", "constructions++; observed = readOuter();", 1)
                target = transformed / "java" / source.relative_to(fixture / "java")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content)
                inputs.append(target)
            smali = transformed / "smali"
            smali.mkdir()
            for source in sorted((fixture / "smali").rglob("*.smali")):
                content = source.read_text()
                if source.name == "InnerHost$Member.smali":
                    content = content.replace(".field public final offset:I", ".field public final offset:I\n.field public marker:J", 1)
                    content += """
.method public constructor <init>(Lfixture/InnerHost;IJ)V
    .registers 8
    .annotation system Ldalvik/annotation/Signature;
        value = { "(IJ)V" }
    .end annotation
    move-object v0, p0
    move-object v1, p1
    move v2, p2
    invoke-direct/range {v0 .. v2}, Lfixture/InnerHost$Member;-><init>(Lfixture/InnerHost;I)V
    iput-wide p3, v0, Lfixture/InnerHost$Member;->marker:J
    return-void
.end method

.method public synthetic constructor <init>(Lfixture/InnerHost;IJLjava/lang/Object;)V
    .registers 6
    invoke-direct/range {p0 .. p4}, Lfixture/InnerHost$Member;-><init>(Lfixture/InnerHost;IJ)V
    return-void
.end method
"""
                elif source.name == "InnerHost.smali":
                    content += """
.method public make(IJ)Lfixture/InnerHost$Member;
    .registers 5
    new-instance v0, Lfixture/InnerHost$Member;
    invoke-direct {v0, p0, p1, p2, p3}, Lfixture/InnerHost$Member;-><init>(Lfixture/InnerHost;IJ)V
    return-object v0
.end method
"""
                elif source.name == "OuterProbe.smali":
                    content = content.replace(".field public final observed:I", ".field public final observed:I\n.field public static constructions:I", 1)
                    content = content.replace("    invoke-virtual {p0},", """    sget v0, Lfixture/OuterProbe;->constructions:I
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/OuterProbe;->constructions:I
    invoke-virtual {p0},""", 1)
                (smali / source.name).write_text(content)
            harness_name = "DelegatingInnerMembersHarness"
            harness = FIXTURES / ("harness/" + harness_name + ".java")
        originals = work / "originals"
        verify.compile_local(inputs, originals, release=25)
        classes, methods = compiler_inventory(verify, originals)
        baseline_dir = work / "baseline"
        verify.compile_local([*inputs, harness], baseline_dir, release=25)
        baseline = verify.run([verify.java, "-cp", baseline_dir, harness_name], "baseline")
        if args.static_field_inner_members:
            baseline_failure = verify.run([verify.java, "-cp", baseline_dir, harness_name, "failure"],
                                          "baseline-initialization-failure")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", smali, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] != 0
                or coverage.get("minimum_java_release") != 25):
            raise RuntimeError("Constructor inventory or Java release requirement changed")
        prologues = [row["identity"] for row in coverage["methods"]
                     if row.get("minimum_java_release") == 25]
        expected_prologue = ("Lfixture/InnerHost$Member;-><init>(Lfixture/InnerHost;I)V"
                             if args.inner_members else
                             "Lfixture/Captured;-><init>(ILjava/lang/Object;)V")
        signature = "(I)V"
        if args.cast_inner_members:
            signature = "(Ljava/lang/Object;)V"
            expected_prologue = "Lfixture/InnerHost$Member;-><init>(Lfixture/InnerHost;Ljava/lang/Object;)V"
        if prologues != [expected_prologue]:
            raise RuntimeError("Java 25 requirement is not tied to the constructor")
        if args.inner_members:
            bindings = coverage["class_source_bindings"]
            constructor = next(row for row in coverage["methods"]
                               if row["identity"] == expected_prologue)
            if (coverage["status"] != "partial" or len(bindings) != 1
                    or bindings[0]["binding_kind"] != "inner-to-static-member"
                    or not bindings[0]["metadata_differences"]
                    or constructor.get("generic_signature") != signature
                    or constructor.get("generic_signature_omitted_parameters") != 1):
                raise RuntimeError("Inner capture mapping or metadata differences are missing")
        sources = sorted((output / "sources").rglob("*.java"))
        rebuilt = work / "rebuilt"
        verify.compile_local([*sources, harness], rebuilt, release=25)
        _, rebuilt_methods = compiler_inventory(verify, rebuilt)
        if not set(methods).issubset(rebuilt_methods):
            raise RuntimeError("Recompiled source lost original method descriptors")
        actual = verify.run([verify.java, "-cp", rebuilt, harness_name], "recovered-execution")
        if args.static_field_inner_members:
            failure = verify.run([verify.java, "-cp", rebuilt, harness_name, "failure"],
                                 "recovered-initialization-failure")
            if failure != baseline_failure:
                raise RuntimeError("Constructor static read changed class initialization failures")
        if args.inner_members:
            if (baseline.splitlines()[-1] != "static=false"
                    or actual.splitlines()[-1] != "static=true"):
                raise RuntimeError("Expected member modifier projection is missing")
            actual, baseline = actual.splitlines()[:-1], baseline.splitlines()[:-1]
            if args.generic_inner_members:
                if baseline[-1] != "payload=T" or actual[-1] != "payload=java.lang.Object":
                    raise RuntimeError("Captured variable reflection differs from its source projection")
                actual, baseline = actual[:-1], baseline[:-1]
        if actual != baseline:
            raise RuntimeError("Constructor changed JVM behavior")
        evidence = {"status": "success", "class_count": len(classes),
                    "method_count": len(methods), "integer_probes": 5,
                    "java_release": 25, "jvm_output": "matched",
                    "pre_super_field_order": "preserved",
                    "final_fields_and_reference_identity": "preserved"}
        if args.inner_members:
            evidence.update(outer_capture_before_super="preserved",
                            explicit_outer_descriptor="preserved",
                            member_modifiers="explicitly-projected")
        if args.guarded_inner_members:
            evidence.update(integer_probes=3, pre_super_check_order="matched",
                            early_exception_effects="matched")
        if args.getter_inner_members:
            evidence.update(integer_probes=6, pre_super_getter_order="matched",
                            getter_evaluation_count="once", retained_getter_result="matched",
                            getter_exception_and_null_receiver_effects="matched")
        if args.static_field_inner_members:
            evidence.update(integer_probes=5, static_reads_per_constructor=3,
                            retained_static_snapshots="matched", class_initialization_order="matched",
                            initialization_failure_identity="preserved", failed_class_retry="matched")
        if args.cast_inner_members:
            evidence.pop("integer_probes")
            evidence.update(reference_probes=6, pre_super_cast_order="matched",
                            failed_cast_effect_order="matched", null_cast_result="matched",
                            retained_cast_result="matched")
        if args.allocating_inner_members:
            evidence.update(integer_probes=4, pre_super_allocations=5,
                            class_initialization_order="matched",
                            failed_allocation_constructor_order="matched",
                            retained_allocation_identity="matched")
            changes = [change["kind"] for row in coverage.get("generic_signature_projections", [])
                       for change in row["changes"]]
            if changes.count("static-inner-owner-arguments") != 2:
                raise RuntimeError("Parameterized inner owner changes were not recorded")
        if args.delegating_inner_members:
            secondary = next(row for row in coverage["methods"]
                             if row["identity"] == "Lfixture/InnerHost$Member;-><init>(Lfixture/InnerHost;IJ)V")
            if (secondary.get("generic_signature") != "(IJ)V"
                    or secondary.get("generic_signature_omitted_parameters") != 1
                    or secondary.get("generic_signature_parameter_indices") != [1, 2]):
                raise RuntimeError("Delegated constructor capture mapping is missing")
            evidence.update(long_probes=5, constructor_chain_depth=2,
                            superclass_initialization_count="once-per-instance",
                            retained_wide_parameter="matched",
                            marker_constructor_descriptor="preserved")
        if args.generic_inner_members:
            projections = coverage.get("generic_signature_projections", [])
            kinds = [change["kind"] for row in projections for change in row["changes"]]
            if (len(projections) != 3 or kinds.count("static-inner-owner-arguments") != 1
                    or kinds.count("static-inner-type-variable") != 2
                    or any(not row.get("metadata_differences") for row in projections)):
                raise RuntimeError("Parameterized owner changes are not explicit")
            evidence.update(generic_owner_value_identity="matched",
                            generic_reflection_difference="confirmed")
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
