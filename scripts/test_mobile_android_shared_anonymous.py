#!/usr/bin/env python3
"""Compare a shared anonymous smali body with an independently compiled JVM oracle.

The Java oracle intentionally uses one named member class: it supplies executable
behavior, not evidence that original anonymous reflection metadata is unchanged.
"""
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
    variants.add_argument("--member-host", action="store_true",
                        help="Place the ordinary host inside a public interface")
    variants.add_argument("--public-facade", action="store_true",
                          help="Reach a package-private host through an existing public subclass")
    variants.add_argument("--static-only-member", action="store_true",
                          help="Retain a constructorless inner member containing only static methods")
    variants.add_argument("--static-utility", action="store_true",
                          help="Retain a synthetic anonymous utility without an instance constructor")
    variants.add_argument("--self-types", action="store_true",
                          help="Retain generic self references in anonymous constructors and fields")
    variants.add_argument("--captured-signatures", action="store_true",
                          help="Report undeclared anonymous arguments and relocated method type variables")
    variants.add_argument("--class-captured-signatures", action="store_true",
                          help="Trace captured class formals through nested anonymous method scopes")
    variants.add_argument("--omitted-captures", action="store_true",
                          help="Map a generic middle parameter around omitted int, wide and object captures")
    variants.add_argument("--default-scopes", action="store_true",
                          help="Project missing synthetic-default scopes using an exact shared-body witness")
    args = parser.parse_args()
    args.captured_signatures |= args.class_captured_signatures
    args.self_types |= args.captured_signatures
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-shared-anonymous-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), work / "unused-d8", 300)
        fixture = FIXTURES / "shared_anonymous"
        inputs = sorted((fixture / "java").rglob("*.java"))
        harness_name = "SharedAnonymousHarness"
        harness = FIXTURES / ("harness/" + harness_name + ".java")
        smali = fixture / "smali"
        host = "Lfixture/SharedHost;"
        if args.omitted_captures:
            transformed = work / "owned-input"
            transformed.mkdir()
            inputs = []
            for source in sorted((fixture / "java").rglob("*.java")):
                content = source.read_text()
                if source.name == "SharedHost.java":
                    content = content.replace("public final int value;", """public final int value;
        public final long wide;
        public final Object token;
        public final java.util.List<String> inputs;
        public Worker(int value, java.util.List<String> inputs, long wide, Object token) {
            this.value = value;
            this.wide = wide;
            this.token = token;
            super();
            this.inputs = inputs;
            count++;
        }""", 1)
                    content = content.replace("value = input;", "value = input; wide = 0L; token = null; inputs = null;", 1)
                target = transformed / "java" / source.relative_to(fixture / "java")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content)
                inputs.append(target)
            smali = transformed / "smali"
            smali.mkdir()
            for source in sorted((fixture / "smali").glob("*.smali")):
                content = source.read_text()
                if source.name == "Worker.smali":
                    content = content.replace(".field public final value:I", """.field public final synthetic value:I
.field public final synthetic wide:J
.field public final synthetic token:Ljava/lang/Object;
.field public final inputs:Ljava/util/List;
    .annotation system Ldalvik/annotation/Signature;
        value = { "Ljava/util/List<Ljava/lang/String;>;" }
    .end annotation
.end field""", 1)
                    content = content.replace(".registers 3", ".registers 4", 1)
                    content = content.replace("    return-void", """    const-wide/16 v0, 0
    iput-wide v0, p0, Lfixture/SharedHost$Worker;->wide:J
    const/4 v0, 0
    iput-object v0, p0, Lfixture/SharedHost$Worker;->token:Ljava/lang/Object;
    iput-object v0, p0, Lfixture/SharedHost$Worker;->inputs:Ljava/util/List;
    return-void""", 1)
                    content += """
.method public constructor <init>(ILjava/util/List;JLjava/lang/Object;)V
    .registers 7
    .annotation system Ldalvik/annotation/Signature;
        value = { "(Ljava/util/List<Ljava/lang/String;>;)V" }
    .end annotation
    iput p1, p0, Lfixture/SharedHost$Worker;->value:I
    iput-wide p3, p0, Lfixture/SharedHost$Worker;->wide:J
    iput-object p5, p0, Lfixture/SharedHost$Worker;->token:Ljava/lang/Object;
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput-object p2, p0, Lfixture/SharedHost$Worker;->inputs:Ljava/util/List;
    sget v0, Lfixture/SharedHost$Worker;->count:I
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/SharedHost$Worker;->count:I
    return-void
.end method
"""
                (smali / source.name).write_text(content)
            changed_harness = transformed / harness.name
            changed_harness.write_text(harness.read_text().replace(
                "System.out.println(input", """try {
                java.util.List<String> inputs = new java.util.ArrayList<>();
                Object token = new Object();
                long wide = ((long) input << 32) + 17;
                java.lang.reflect.Constructor<?> ctor =
                        type.getConstructor(int.class, java.util.List.class, long.class, Object.class);
                Object extra = ctor.newInstance(input, inputs, wide, token);
                created++;
                java.lang.reflect.ParameterizedType generic =
                        (java.lang.reflect.ParameterizedType) ctor.getGenericParameterTypes()[1];
                if (generic.getActualTypeArguments()[0] != String.class
                        || type.getField("wide").getLong(extra) != wide
                        || type.getField("token").get(extra) != token
                        || type.getField("inputs").get(extra) != inputs
                        || SharedHost.read(extra) != input || SharedHost.count() != created) {
                    throw new AssertionError("Captured parameter mapping changed values or generic slot");
                }
            } catch (ReflectiveOperationException failure) {
                throw new AssertionError(failure);
            }
            System.out.println(input""", 1))
            harness = changed_harness
        if args.self_types:
            transformed = work / "owned-input"
            transformed.mkdir()
            inputs = []
            for source in sorted((fixture / "java").rglob("*.java")):
                content = source.read_text()
                if source.name == "SharedHost.java":
                    if args.captured_signatures:
                        if args.class_captured_signatures:
                            content = content.replace("public class SharedHost {", "public class SharedHost<T> {", 1)
                            content = content.replace("public static Object make(int input)", "public Object make(int input)", 1)
                        else:
                            content = content.replace("public static Object make(int input)",
                                                      "public static <T> Object make(int input)", 1)
                        content = content.replace("static int count;", "static int count;\n        public Object token;", 1)
                    content = content.replace("public final int value;", """public final int value;
        public java.util.List<? super Worker> peers;
        public Worker(int input, java.util.List<? super Worker> peers) {
            value = input;
            this.peers = peers;
            count++;
        }""", 1)
                    content = content.replace("final Worker parent;", """final Worker parent;
            public java.util.List<? super Worker> ownerPeers;""", 1)
                target = transformed / "java" / source.relative_to(fixture / "java")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content)
                inputs.append(target)
            smali = transformed / "smali"
            smali.mkdir()
            for source in sorted((fixture / "smali").glob("*.smali")):
                content = source.read_text()
                if args.captured_signatures and source.name == "SharedHost.smali":
                    if args.class_captured_signatures:
                        content = content.replace(".super Ljava/lang/Object;", """.super Ljava/lang/Object;
.annotation system Ldalvik/annotation/Signature;
    value = { "<T:Ljava/lang/Object;>Ljava/lang/Object;" }
.end annotation""", 1)
                        content = content.replace(""".method public static make(I)Ljava/lang/Object;
    .registers 2""", """.method public make(I)Ljava/lang/Object;
    .registers 3""", 1)
                        content = content.replace("invoke-direct {v0, p0}", "invoke-direct {v0, p1}", 1)
                    else:
                        content = content.replace(".method public static make(I)Ljava/lang/Object;", """.method public static make(I)Ljava/lang/Object;
    .annotation system Ldalvik/annotation/Signature;
        value = { "<T:Ljava/lang/Object;>(I)Ljava/lang/Object;" }
    .end annotation""", 1)
                if source.name == "Child.smali":
                    content = content.replace(".field final parent:Lfixture/SharedHost$Worker;", """.field final parent:Lfixture/SharedHost$Worker;
.field public ownerPeers:Ljava/util/List;
    .annotation system Ldalvik/annotation/Signature;
        value = { "Ljava/util/List<-Lfixture/SharedHost$Worker;>;" }
    .end annotation
.end field""", 1)
                    if args.captured_signatures:
                        phantom = "Lfixture/SharedHost$Worker<TT;>;"
                        if not args.class_captured_signatures:
                            content = content.replace("Lfixture/SharedHost$Worker;->nested()Ljava/lang/Object;",
                                                      "Lfixture/SharedHost;->make(I)Ljava/lang/Object;", 1)
                        content = content.replace(".field final parent:Lfixture/SharedHost$Worker;", """.field final parent:Lfixture/SharedHost$Worker;
    .annotation system Ldalvik/annotation/Signature;
        value = { """ + '"' + phantom + '"' + """ }
    .end annotation
.end field""", 1)
                        content = content.replace("<init>(Lfixture/SharedHost$Worker;)V", """<init>(Lfixture/SharedHost$Worker;)V
    .annotation system Ldalvik/annotation/Signature;
        value = { """ + '"(' + phantom + ')V"' + """ }
    .end annotation""", 1)
                        content = content.replace("Ljava/util/List<-Lfixture/SharedHost$Worker;>;",
                                                  "Ljava/util/List<-" + phantom + ">;")
                if source.name == "Worker.smali":
                    if args.captured_signatures:
                        content = content.replace(".field static count:I", """.field static count:I
.field public token:Ljava/lang/Object;
    .annotation system Ldalvik/annotation/Signature;
        value = { "TT;" }
    .end annotation
.end field""", 1)
                    content = content.replace(".field public final value:I", """.field public final value:I
.field public peers:Ljava/util/List;
    .annotation system Ldalvik/annotation/Signature;
        value = { "Ljava/util/List<-Lfixture/SharedHost$Worker;>;" }
    .end annotation
.end field""", 1)
                    content += """
.method public constructor <init>(ILjava/util/List;)V
    .registers 4
    .annotation system Ldalvik/annotation/Signature;
        value = { "(ILjava/util/List<-Lfixture/SharedHost$Worker;>;)V" }
    .end annotation
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput p1, p0, Lfixture/SharedHost$Worker;->value:I
    iput-object p2, p0, Lfixture/SharedHost$Worker;->peers:Ljava/util/List;
    sget v0, Lfixture/SharedHost$Worker;->count:I
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/SharedHost$Worker;->count:I
    return-void
.end method
"""
                (smali / source.name).write_text(content)
            changed_harness = transformed / harness.name
            changed_harness.write_text(harness.read_text().replace(
                "System.out.println(input", """try {
                java.util.List<Object> peers = new java.util.ArrayList<>();
                java.lang.reflect.Constructor<?> constructor =
                        type.getConstructor(int.class, java.util.List.class);
                Object extra = constructor.newInstance(input, peers);
                created++;
                java.lang.reflect.Field field = type.getField("peers");
                java.lang.reflect.ParameterizedType parameter =
                        (java.lang.reflect.ParameterizedType) constructor.getGenericParameterTypes()[1];
                java.lang.reflect.WildcardType bound =
                        (java.lang.reflect.WildcardType) parameter.getActualTypeArguments()[0];
                java.lang.reflect.Method nested = type.getDeclaredMethod("nested");
                nested.setAccessible(true);
                Object child = nested.invoke(first);
                if (bound.getLowerBounds()[0] != type || !field.getGenericType().equals(parameter)
                        || !child.getClass().getField("ownerPeers").getGenericType().equals(parameter)
                        || field.get(extra) != peers || SharedHost.read(extra) != input
                        || SharedHost.count() != created) {
                    throw new AssertionError("Generic self binding or capture identity changed");
                }
            } catch (ReflectiveOperationException failure) {
                throw new AssertionError(failure);
            }
            System.out.println(input""", 1))
            harness = changed_harness
            if args.class_captured_signatures:
                harness.write_text(harness.read_text().replace("SharedHost.make(", "new SharedHost<Object>().make("))
        if args.default_scopes:
            transformed = work / "owned-default-input"
            inputs = []
            for source in sorted((fixture / "java").rglob("*.java")):
                content = source.read_text()
                if source.name == "SharedHost.java":
                    content = content.replace("public static Object make(int input)",
                                              "public static <T> Object make(int input)", 1)
                    content = content.replace("    public static Object another", """    public static Object make$default(int input, int mask, Object marker) {
        if ((mask & 1) != 0) input = 17;
        return new Worker(input);
    }
    public static Object another""", 1)
                    content = content.replace("final Worker parent;", """final Worker parent;
            public Object token;
            public Object identity(Object input) { return input; }""", 1)
                target = transformed / "java" / source.relative_to(fixture / "java")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content)
                inputs.append(target)
            smali = transformed / "smali"
            smali.mkdir()
            for source in sorted((fixture / "smali").glob("*.smali")):
                content = source.read_text()
                if source.name == "SharedHost.smali":
                    content = content.replace(".method public static make(I)Ljava/lang/Object;", """.method public static make(I)Ljava/lang/Object;
    .annotation system Ldalvik/annotation/Signature;
        value = { "<T:Ljava/lang/Object;>(I)Ljava/lang/Object;" }
    .end annotation""", 1)
                    content += """
.method public static synthetic make$default(IILjava/lang/Object;)Ljava/lang/Object;
    .registers 4
    and-int/lit8 v0, p1, 1
    if-eqz v0, :supplied
    const/16 p0, 17
    :supplied
    new-instance v0, Lfixture/SharedHost$Worker;
    invoke-direct {v0, p0}, Lfixture/SharedHost$Worker;-><init>(I)V
    return-object v0
.end method
"""
                elif source.name == "Child.smali":
                    content = content.replace("Lfixture/SharedHost$Worker;->nested()Ljava/lang/Object;",
                                              "Lfixture/SharedHost;->make$default(IILjava/lang/Object;)Ljava/lang/Object;", 1)
                    content = content.replace(".field final parent:", """.field public token:Ljava/lang/Object;
    .annotation system Ldalvik/annotation/Signature;
        value = { "TT;" }
    .end annotation
.end field
.field final parent:""", 1)
                    content += """
.method public identity(Ljava/lang/Object;)Ljava/lang/Object;
    .registers 2
    .annotation system Ldalvik/annotation/Signature;
        value = { "(TT;)TT;" }
    .end annotation
    return-object p1
.end method
"""
                (smali / source.name).write_text(content)
            changed_harness = transformed / harness.name
            changed_harness.write_text(harness.read_text().replace(
                "System.out.println(input", """Object supplied = SharedHost.make$default(input + 3, 0, null);
            Object fallback = SharedHost.make$default(input, 1, null);
            created += 2;
            if (supplied.getClass() != type || fallback.getClass() != type
                    || SharedHost.read(supplied) != input + 3
                    || SharedHost.read(fallback) != 17 || SharedHost.count() != created)
                throw new AssertionError("Default mask or shared-body identity changed");
            try {
                java.lang.reflect.Method nested = type.getDeclaredMethod("nested");
                nested.setAccessible(true);
                Object child = nested.invoke(supplied);
                java.lang.reflect.Field token = child.getClass().getDeclaredField("token");
                java.lang.reflect.Method identity = child.getClass().getDeclaredMethod("identity", Object.class);
                token.setAccessible(true);
                identity.setAccessible(true);
                Object value = new Object();
                token.set(child, value);
                if (token.get(child) != value || identity.invoke(child, value) != value
                        || token.getGenericType() != Object.class
                        || identity.getGenericParameterTypes()[0] != Object.class
                        || identity.getGenericReturnType() != Object.class)
                    throw new AssertionError("Default-scope object erasure or identity changed");
            } catch (ReflectiveOperationException failure) {
                throw new AssertionError(failure);
            }
            System.out.println(input""", 1))
            harness = changed_harness
        if args.member_host:
            # Transform only this owned fixture's declaration placement. The
            # independent javac oracle and smali keep the same method bodies.
            host = "Lfixture/FixtureScope$SharedHost;"
            transformed = work / "owned-input"
            transformed.mkdir()
            inputs = []
            for source in sorted((fixture / "java").rglob("*.java")):
                content = source.read_text().replace(
                    "import fixture.SharedHost;", "import fixture.FixtureScope.SharedHost;")
                relative = source.relative_to(fixture / "java")
                if source.name == "SharedHost.java":
                    content = content.replace(
                        "public class SharedHost {",
                        "public interface FixtureScope {\npublic static class SharedHost {", 1) + "\n}\n"
                    relative = relative.with_name("FixtureScope.java")
                elif source.name == "Adapter.java":
                    content = content.replace("package fixture;", "package fixture;\nimport fixture.FixtureScope.SharedHost;", 1)
                target = transformed / "java" / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content)
                inputs.append(target)
            smali = transformed / "smali"
            smali.mkdir()
            for source in sorted((fixture / "smali").glob("*.smali")):
                content = source.read_text().replace("Lfixture/SharedHost", "Lfixture/FixtureScope$SharedHost")
                if source.name == "SharedHost.smali":
                    content = content.replace(".super Ljava/lang/Object;", """.super Ljava/lang/Object;
.annotation system Ldalvik/annotation/EnclosingClass;
    value = Lfixture/FixtureScope;
.end annotation
.annotation system Ldalvik/annotation/InnerClass;
    accessFlags = 0x9
    name = "SharedHost"
.end annotation""", 1)
                (smali / source.name).write_text(content)
            (smali / "FixtureScope.smali").write_text(
                ".class public interface abstract Lfixture/FixtureScope;\n.super Ljava/lang/Object;\n")
            changed_harness = transformed / harness.name
            changed_harness.write_text(harness.read_text().replace(
                "import fixture.SharedHost;", "import fixture.FixtureScope.SharedHost;"))
            harness = changed_harness
        if args.public_facade:
            transformed = work / "owned-input"
            transformed.mkdir()
            inputs = []
            for source in sorted((fixture / "java").rglob("*.java")):
                content = source.read_text()
                if source.name == "SharedHost.java":
                    content = content.replace("public class SharedHost {",
                                              "class SharedHost {\n    public SharedHost() {}", 1)
                elif source.name == "Remote.java":
                    content = content.replace("SharedHost", "PublicFacade")
                target = transformed / "java" / source.relative_to(fixture / "java")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content)
                inputs.append(target)
            smali = transformed / "smali"
            smali.mkdir()
            for source in sorted((fixture / "smali").glob("*.smali")):
                content = source.read_text()
                if source.name == "SharedHost.smali":
                    content = content.replace(".class public ", ".class ", 1)
                (smali / source.name).write_text(content)
            for name, parent, access in [("InheritedHost", "SharedHost", ""),
                                         ("PublicFacade", "InheritedHost", "public ")]:
                target = transformed / "java/fixture" / (name + ".java")
                target.write_text("package fixture;\n" + access + "class " + name
                                  + " extends " + parent + " { public " + name + "() {} }\n")
                inputs.append(target)
                (smali / (name + ".smali")).write_text(
                    ".class " + access + "Lfixture/" + name + ";\n.super Lfixture/" + parent + ";\n"
                    ".method public constructor <init>()V\n    .registers 1\n"
                    "    invoke-direct {p0}, Lfixture/" + parent + ";-><init>()V\n"
                    "    return-void\n.end method\n")
            changed_harness = transformed / harness.name
            changed_harness.write_text(harness.read_text().replace("SharedHost", "PublicFacade").replace(
                "type.getEnclosingClass() != PublicFacade.class",
                "type.getEnclosingClass() != PublicFacade.class.getSuperclass().getSuperclass()"))
            harness = changed_harness
        if args.static_only_member:
            transformed = work / "owned-input"
            transformed.mkdir()
            inputs = []
            for source in sorted((fixture / "java").rglob("*.java")):
                content = source.read_text()
                if source.name == "SharedHost.java":
                    content = content.replace("value = input;", "value = Offsets.offset(input - 2);")
                    content = content.replace("static final class Child {", """public final class Offsets {
            public static int offset(int value) { return value + 2; }
        }
        static final class Child {""", 1)
                target = transformed / "java" / source.relative_to(fixture / "java")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content)
                inputs.append(target)
            smali = transformed / "smali"
            smali.mkdir()
            for source in sorted((fixture / "smali").glob("*.smali")):
                content = source.read_text()
                if source.name == "Worker.smali":
                    content = content.replace("    iput p1, p0,", """    add-int/lit8 p1, p1, -2
    invoke-static {p1}, Lfixture/SharedHost$Worker$Offsets;->offset(I)I
    move-result p1
    iput p1, p0,""", 1)
                (smali / source.name).write_text(content)
            (smali / "Offsets.smali").write_text(""".class public final Lfixture/SharedHost$Worker$Offsets;
.super Ljava/lang/Object;
.annotation system Ldalvik/annotation/EnclosingClass;
    value = Lfixture/SharedHost$Worker;
.end annotation
.annotation system Ldalvik/annotation/InnerClass;
    accessFlags = 0x11
    name = "Offsets"
.end annotation
.method public static offset(I)I
    .registers 1
    add-int/lit8 p0, p0, 2
    return p0
.end method
""")
            changed_harness = transformed / harness.name
            changed_harness.write_text(harness.read_text().replace(
                "System.out.println(input", """Class<?> helper = null;
            for (Class<?> member : type.getDeclaredClasses()) {
                if (member.getSimpleName().equals("Offsets")) helper = member;
            }
            if (helper == null || helper.getEnclosingClass() != type
                    || Modifier.isStatic(helper.getModifiers())) {
                throw new AssertionError("Non-static member metadata changed");
            }
            System.out.println(input""", 1))
            harness = changed_harness
        if args.static_utility:
            transformed = work / "owned-input"
            transformed.mkdir()
            inputs = []
            for source in sorted((fixture / "java").rglob("*.java")):
                content = source.read_text()
                if source.name == "SharedHost.java":
                    content = content.replace("value = input;", "value = input - 2 + InitValues.offset;", 1)
                    content = content.replace("public static final class Worker {",
                                              "static class InitValues { static int offset = 2; }\n"
                                              "    public static final class Worker {", 1)
                target = transformed / "java" / source.relative_to(fixture / "java")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content)
                inputs.append(target)
            smali = transformed / "smali"
            smali.mkdir()
            for source in sorted((fixture / "smali").glob("*.smali")):
                content = source.read_text()
                if source.name == "Worker.smali":
                    content = content.replace("    iput p1, p0,", """    add-int/lit8 p1, p1, -2
    sget v0, Lfixture/SharedHost$InitValues;->offset:I
    add-int p1, p1, v0
    iput p1, p0,""", 1)
                (smali / source.name).write_text(content)
            (smali / "InitValues.smali").write_text(""".class synthetic Lfixture/SharedHost$InitValues;
.super Ljava/lang/Object;
.annotation system Ldalvik/annotation/EnclosingClass;
    value = Lfixture/SharedHost;
.end annotation
.annotation system Ldalvik/annotation/InnerClass;
    accessFlags = 0x1008
    name = null
.end annotation
.field static offset:I
.method static constructor <clinit>()V
    .registers 1
    const/4 v0, 2
    sput v0, Lfixture/SharedHost$InitValues;->offset:I
    return-void
.end method
""")
        originals = work / "originals"
        release = 25 if args.omitted_captures else 17 if args.static_only_member else 8
        verify.compile_local(inputs, originals, release=release)
        classes, methods = compiler_inventory(verify, originals)
        if args.static_only_member or args.static_utility:
            # Java supplies this extra constructor; the owned smali deliberately
            # contains no instance constructor or instance fields for Offsets.
            extra = ("Lfixture/SharedHost$InitValues;-><init>()V" if args.static_utility else
                     "Lfixture/SharedHost$Worker$Offsets;-><init>(Lfixture/SharedHost$Worker;)V")
            if extra not in methods:
                raise RuntimeError("Expected javac inner constructor is missing")
            methods = {identity: detail for identity, detail in methods.items() if identity != extra}
        baseline_dir = work / "baseline"
        verify.compile_local([*inputs, harness], baseline_dir, release=release)
        baseline = verify.run([verify.java, "-cp", baseline_dir, harness_name], "baseline")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", smali, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes)
                or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial"):
            raise RuntimeError("Shared body inventory or conservative recovery status changed")
        bindings = coverage["class_source_bindings"]
        shared_bindings = [binding for binding in bindings
                           if binding["binding_kind"] == "anonymous-to-shared-member"]
        if (len(bindings) != (3 if args.static_only_member or args.static_utility else 2)
                or len(shared_bindings) != 2
                or {binding["enclosing_method"]["name"] for binding in shared_bindings}
                != ({"make", "make$default"} if args.default_scopes
                    else {"make"} if args.captured_signatures and not args.class_captured_signatures
                    else {"make", "nested"}) or any(
                    binding["binding_kind"] != "anonymous-to-shared-member"
                    or binding["binary_name_status"] != "changed"
                    or binding["source_enclosing_class"] != host
                    or not binding["metadata_differences"] for binding in shared_bindings)):
            raise RuntimeError("Changed anonymous ownership is not explicit")
        for row in coverage["methods"]:
            ordinary = {host + "-><init>()V", "Lconsumer/Remote;-><init>()V"}
            if args.public_facade:
                ordinary.update({"Lfixture/InheritedHost;-><init>()V", "Lfixture/PublicFacade;-><init>()V"})
            if row["identity"] not in ordinary:
                kind = ("member-of-projected-class" if args.static_only_member
                        and row["identity"].startswith("Lfixture/SharedHost$Worker$Offsets;")
                        else "anonymous-to-shared-member")
                if args.static_utility and (row["identity"].startswith("Lfixture/SharedHost$InitValues;")
                                            or row["identity"] == "Lfixture/SharedHost$Worker;-><init>(I)V"):
                    kind = "anonymous-to-named-member"
                if row["projection_kind"] != kind:
                    raise RuntimeError("A shared caller or body lost its projection status")
        sources = sorted((output / "sources").rglob("*.java"))
        rebuilt = work / "rebuilt"
        verify.compile_local([*sources, harness], rebuilt, release=release)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        renamed = set(methods)
        renamed_classes = {}
        for binding in bindings:
            owner = renamed_classes.get(binding["source_enclosing_class"], binding["source_enclosing_class"])
            expected_class = owner[:-1] + "$" + binding["source_name"] + ";"
            if expected_class not in rebuilt_classes:
                raise RuntimeError("Shared source class has the wrong declaration owner")
            renamed = {method.replace(binding["class"], expected_class) for method in renamed}
            renamed_classes[binding["class"]] = expected_class
            if args.public_facade and binding.get("source_reference_class") != "Lfixture/PublicFacade;":
                raise RuntimeError("Inherited source qualifier is not recorded")
        if not renamed.issubset(rebuilt_methods):
            raise RuntimeError("Shared source projection lost an original method")
        actual = verify.run([verify.java, "-cp", rebuilt, harness_name], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Shared anonymous body, state or identity changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "integer_probes": 5, "shared_class_identity": "matched",
                    "shared_static_state": "matched", "external_capture_storage": "matched",
                    "nested_capture_dependencies": "matched",
                    "public_cross_package_access": "matched",
                    "jvm_behavior": "matched",
                    "source_ownership": "changed to one static member", "coverage": "partial"}
        if args.member_host:
            evidence["interface_member_host"] = "retained"
        if args.self_types:
            evidence["generic_self_constructor_and_field"] = "matched"
            evidence["generic_shared_capture_field"] = "matched"
        if args.captured_signatures:
            projections = coverage.get("generic_signature_projections", [])
            kinds = [change["kind"] for row in projections for change in row["changes"]]
            if (len(projections) != 4 or kinds.count("anonymous-captured-type-arguments") != 3
                    or kinds.count("shared-class-type-variable" if args.class_captured_signatures
                                   else "shared-method-type-variable") != 1
                    or any(not row.get("original_signature") or not row.get("metadata_differences")
                           for row in projections)):
                raise RuntimeError("Original captured signatures or projection differences are missing")
            evidence["captured_signature_differences"] = "explicitly-reported"
        if args.omitted_captures:
            constructor = next(row for row in coverage["methods"]
                               if row["identity"] == "Lfixture/SharedHost$Worker;-><init>(ILjava/util/List;JLjava/lang/Object;)V")
            if (constructor.get("generic_signature_parameter_indices") != [1]
                    or constructor.get("generic_signature_omitted_parameters") != 3
                    or constructor.get("generic_signature") != "(Ljava/util/List<Ljava/lang/String;>;)V"
                    or coverage.get("minimum_java_release") != 25):
                raise RuntimeError("Original constructor signature or exact capture mapping is missing")
            evidence.update(captured_parameter_values="matched", generic_middle_parameter="matched",
                            java_release=25)
        if args.default_scopes:
            projections = coverage.get("generic_signature_projections", [])
            changes = [change for row in projections for change in row["changes"]]
            if (len(projections) != 2 or len(changes) != 3
                    or any(change["kind"] != "synthetic-default-method-type-variable"
                           or change.get("scope_origin") != host + "->make(I)Ljava/lang/Object;"
                           or change["source_type"] != "Ljava/lang/Object;" for change in changes)
                    or any(not row.get("original_signature") or not row.get("metadata_differences")
                           for row in projections)):
                raise RuntimeError("Synthetic default scope origin or metadata differences are missing")
            evidence.update(default_mask_behavior="matched", default_scope_value_identity="matched",
                            default_scope_projection="explicitly-reported-with-origin")
        if args.public_facade:
            evidence["inherited_public_member_qualifier"] = "matched"
            evidence["package_private_host_access"] = "retained"
        if args.static_only_member:
            helpers = coverage["generated_source_helpers"]
            if not any(row["class"] == "Lfixture/SharedHost$Worker$Offsets;"
                       and row["kind"] == "default-constructor" for row in helpers):
                raise RuntimeError("Additional source constructor is not recorded")
            if coverage.get("minimum_java_release") != 16:
                raise RuntimeError("Static inner members require Java 16")
            evidence["static_methods_in_non_static_member"] = "matched"
            evidence["additional_constructor_helper"] = "recorded"
        if args.static_utility:
            utilities = [binding for binding in bindings
                         if binding["binding_kind"] == "anonymous-to-named-member"]
            if (len(utilities) != 1 or utilities[0]["class"] != "Lfixture/SharedHost$InitValues;"
                    or len(utilities[0]["metadata_differences"]) < 5
                    or not any(row["class"] == utilities[0]["class"] and row["kind"] == "default-constructor"
                               for row in coverage["generated_source_helpers"])):
                raise RuntimeError("Constructorless utility source differences are not recorded")
            evidence["constructorless_static_utility"] = "matched"
            evidence["additional_constructor_helper"] = "recorded"
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
