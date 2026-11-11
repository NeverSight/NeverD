#!/usr/bin/env python3
"""Execute enum initialization continuations and reject escaped backing arrays."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import json
from pathlib import Path
import shutil
import subprocess
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
    variant = parser.add_mutually_exclusive_group()
    variant.add_argument("--computed-arguments", action="store_true")
    variant.add_argument("--initializer-loop", action="store_true")
    variant.add_argument("--wide-arguments", action="store_true")
    variant.add_argument("--literal-factories", action="store_true")
    variant.add_argument("--encoded-constants", action="store_true")
    variant.add_argument("--delegating-constructors", action="store_true")
    variant.add_argument("--object-arguments", action="store_true")
    variant.add_argument("--object-constructor-arguments", action="store_true")
    variant.add_argument("--conditional-constructor", action="store_true")
    variant.add_argument("--constructor-handlers", action="store_true")
    variant.add_argument("--default-constructors", action="store_true")
    args = parser.parse_args()
    computed_arguments = args.computed_arguments or args.literal_factories
    object_arguments = args.object_arguments or args.object_constructor_arguments
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-enum-arrays-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), work / "unused-d8", 300)
        fixture = FIXTURES / "enum_arrays"
        default_constructor = "Lfixture/Mode;-><init>(Ljava/lang/String;IIIILjava/lang/Object;)V"
        if args.default_constructors:
            owned = work / "owned-input"
            shutil.copytree(fixture, owned)
            mode = owned / "java/fixture/Mode.java"
            text = mode.read_text().replace("FIRST(3), SECOND(7)",
                "FIRST(3, 0, 2, null), SECOND(11, 7, 0, null)")
            text = text.replace("private Mode(int bias) { this.bias = bias; }", """private Mode(int value, int bias) { this.bias = bias; }
    private Mode(int value, int supplied, int mask, Object marker) {
        this(value, (mask & 2) != 0 ? value : supplied);
    }""")
            mode.write_text(text)
            mode = owned / "smali/Mode.smali"
            text = mode.read_text().replace("<init>(Ljava/lang/String;II)V", "<init>(Ljava/lang/String;III)V")
            text = text.replace('"(I)V"', '"(II)V"').replace(".registers 4", ".registers 5")
            text = text.replace("iput p3, p0, Lfixture/Mode;->bias:I", "iput p4, p0, Lfixture/Mode;->bias:I")
            text = text.replace(".method static constructor <clinit>()V\n    .registers 5",
                ".method static constructor <clinit>()V\n    .registers 7")
            for old, value, supplied, mask in ((3, 3, 0, 2), (7, 11, 7, 0)):
                text = text.replace("    const/4 v3, " + str(old),
                    "    const/16 v3, " + str(value) + "\n    const/4 v4, " + str(supplied) +
                    "\n    const/4 v5, " + str(mask) + "\n    const/4 v6, 0")
            text = text.replace("invoke-direct {v0, v1, v2, v3}, Lfixture/Mode;-><init>(Ljava/lang/String;III)V",
                "invoke-direct/range {v0 .. v6}, " + default_constructor)
            text += """
.method synthetic constructor <init>(Ljava/lang/String;IIIILjava/lang/Object;)V
    .registers 7
    and-int/lit8 p5, p5, 2
    if-eqz p5, :selected
    move p4, p3
    :selected
    invoke-direct {p0, p1, p2, p3, p4}, Lfixture/Mode;-><init>(Ljava/lang/String;III)V
    :finished
    return-void
.end method
"""
            mode.write_text(text)
            fixture = owned
        if args.constructor_handlers:
            owned = work / "owned-input"
            shutil.copytree(fixture, owned)
            mode = owned / "java/fixture/Mode.java"
            text = mode.read_text().replace("private final int bias;", "private int bias;")
            text = text.replace("private Mode(int bias) { this.bias = bias; }", """private Mode(int bias) {
        try { this.bias = Failing.read(bias); }
        catch (Throwable failure) { this.bias = -bias; }
    }""")
            mode.write_text(text)
            (owned / "java/fixture/Failing.java").write_text("""package fixture;
public final class Failing {
    private static int count;
    private Failing() {}
    public static int read(int value) throws Throwable {
        ++count;
        if (value > 5) throw new Throwable();
        return value + 1;
    }
    public static int calls() { return count; }
}
""")
            mode = owned / "smali/Mode.smali"
            text = mode.read_text().replace(".field private final bias:I", ".field private bias:I")
            text = text.replace(".method private constructor <init>(Ljava/lang/String;II)V\n    .registers 4",
                                ".method private constructor <init>(Ljava/lang/String;II)V\n    .registers 5")
            text = text.replace("    iput p3, p0, Lfixture/Mode;->bias:I", """    :start
    invoke-static {p3}, Lfixture/Failing;->read(I)I
    move-result v0
    iput v0, p0, Lfixture/Mode;->bias:I
    :end
    goto :done
    .catch Ljava/lang/Throwable; {:start .. :end} :handler
    :handler
    move-exception v0
    neg-int v0, p3
    iput v0, p0, Lfixture/Mode;->bias:I
    :done""")
            mode.write_text(text)
            (owned / "smali/Failing.smali").write_text(""".class public final Lfixture/Failing;
.super Ljava/lang/Object;
.field private static count:I
.method private constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public static read(I)I
    .registers 2
    sget v0, Lfixture/Failing;->count:I
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/Failing;->count:I
    const/4 v0, 5
    if-le p0, v0, :ok
    new-instance v0, Ljava/lang/Throwable;
    invoke-direct {v0}, Ljava/lang/Throwable;-><init>()V
    throw v0
    :ok
    add-int/lit8 v0, p0, 1
    return v0
.end method
.method public static calls()I
    .registers 1
    sget v0, Lfixture/Failing;->count:I
    return v0
.end method
""")
            fixture = owned
        if args.conditional_constructor:
            owned = work / "owned-input"
            shutil.copytree(fixture, owned)
            mode = owned / "java/fixture/Mode.java"
            text = mode.read_text().replace("private final int bias;", "private final int bias;\n    private final int ceiling, floor;")
            text = text.replace("private Mode(int bias) { this.bias = bias; }", """private Mode(int bias) {
        this.ceiling = bias * 10;
        if (bias < 5) { this.bias = bias + 1; this.floor = bias - 1; }
        else { this.bias = bias - 1; this.floor = bias + 1; }
    }
    public int ceiling() { return ceiling; }
    public int floor() { return floor; }""")
            mode.write_text(text)
            mode = owned / "smali/Mode.smali"
            text = mode.read_text().replace(".field private final bias:I",
                ".field private final bias:I\n.field private final ceiling:I\n.field private final floor:I")
            text = text.replace(".method private constructor <init>(Ljava/lang/String;II)V\n    .registers 4",
                                ".method private constructor <init>(Ljava/lang/String;II)V\n    .registers 6")
            text = text.replace("    iput p3, p0, Lfixture/Mode;->bias:I", """    mul-int/lit8 v0, p3, 10
    iput v0, p0, Lfixture/Mode;->ceiling:I
    const/4 v0, 5
    if-lt p3, v0, :small
    add-int/lit8 v0, p3, -1
    iput v0, p0, Lfixture/Mode;->bias:I
    add-int/lit8 v0, p3, 1
    iput v0, p0, Lfixture/Mode;->floor:I
    goto :done
    :small
    add-int/lit8 v0, p3, 1
    iput v0, p0, Lfixture/Mode;->bias:I
    add-int/lit8 v0, p3, -1
    iput v0, p0, Lfixture/Mode;->floor:I
    :done""")
            for field in ("ceiling", "floor"):
                text += f"""
.method public {field}()I
    .registers 2
    iget v0, p0, Lfixture/Mode;->{field}:I
    return v0
.end method
"""
            mode.write_text(text)
            fixture = owned
        if object_arguments:
            owned = work / "owned-input"
            shutil.copytree(fixture, owned)
            mode = owned / "java/fixture/Mode.java"
            text = mode.read_text().replace("FIRST(3), SECOND(7)", "FIRST(3, new Item()), SECOND(7, new Item())")
            text = text.replace("private final int bias;", "private final int bias;\n    private final Token item;")
            text = text.replace("private Mode(int bias) { this.bias = bias; }",
                "private Mode(int bias, Token item) { this.bias = bias; this.item = item; }\n"
                "    public Token item() { return item; }")
            mode.write_text(text)
            (owned / "java/fixture/Token.java").write_text("package fixture;\npublic interface Token { int value(); }\n")
            (owned / "java/fixture/SubToken.java").write_text("package fixture;\npublic interface SubToken extends Token {}\n")
            (owned / "java/fixture/Item.java").write_text("""package fixture;
public final class Item implements SubToken {
    private static int count;
    private final int id;
    public Item() { id = ++count; }
    public int value() { return id; }
    public static int calls() { return count; }
}
""")
            mode = owned / "smali/Mode.smali"
            text = mode.read_text().replace("<init>(Ljava/lang/String;II)V", "<init>(Ljava/lang/String;IILfixture/Token;)V")
            text = text.replace('"(I)V"', '"(ILfixture/Token;)V"').replace(".registers 4", ".registers 5")
            text = text.replace(".field private final bias:I", ".field private final bias:I\n.field private final item:Lfixture/Token;")
            text = text.replace("    iput p3, p0, Lfixture/Mode;->bias:I",
                                "    iput p3, p0, Lfixture/Mode;->bias:I\n    iput-object p4, p0, Lfixture/Mode;->item:Lfixture/Token;")
            for value in (3, 7):
                text = text.replace("    const/4 v3, " + str(value), "    const/4 v3, " + str(value) +
                    "\n    new-instance v4, Lfixture/Item;\n    invoke-direct {v4}, Lfixture/Item;-><init>()V")
            text = text.replace("invoke-direct {v0, v1, v2, v3}", "invoke-direct {v0, v1, v2, v3, v4}")
            text += """
.method public item()Lfixture/Token;
    .registers 2
    iget-object v0, p0, Lfixture/Mode;->item:Lfixture/Token;
    return-object v0
.end method
"""
            mode.write_text(text)
            (owned / "smali/Token.smali").write_text(""".class public interface abstract Lfixture/Token;
.super Ljava/lang/Object;
.method public abstract value()I
.end method
""")
            (owned / "smali/SubToken.smali").write_text(""".class public interface abstract Lfixture/SubToken;
.super Ljava/lang/Object;
.implements Lfixture/Token;
""")
            (owned / "smali/Item.smali").write_text(""".class public final Lfixture/Item;
.super Ljava/lang/Object;
.implements Lfixture/SubToken;
.field private static count:I
.field private final id:I
.method public constructor <init>()V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    sget v0, Lfixture/Item;->count:I
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/Item;->count:I
    iput v0, p0, Lfixture/Item;->id:I
    return-void
.end method
.method public value()I
    .registers 2
    iget v0, p0, Lfixture/Item;->id:I
    return v0
.end method
.method public static calls()I
    .registers 1
    sget v0, Lfixture/Item;->count:I
    return v0
.end method
""")
            fixture = owned
            if args.object_constructor_arguments:
                mode = owned / "java/fixture/Mode.java"
                mode.write_text(mode.read_text().replace('new Item()',
                    'Factory.wrap("constructed", new Item("constructed", TokenSource.VALUE, 10L))'))
                item = owned / "java/fixture/Item.java"
                item.write_text(item.read_text().replace('public Item() { id = ++count; }',
                    'static { Trace.mark(1); }\n'
                    '    public Item(String label, Token token, long extra) {\n'
                    '        id = ++count + token.value() + (int) extra; Trace.mark(3);\n'
                    '    }'))
                (owned / "java/fixture/TokenSource.java").write_text('''package fixture;
public final class TokenSource implements Token {
    public static final Token VALUE = new TokenSource();
    static { Trace.mark(2); }
    private TokenSource() {}
    public int value() { return 10; }
}
''')
                (owned / "java/fixture/Factory.java").write_text('''package fixture;
public final class Factory {
    private Factory() {}
    public static Token wrap(String label, Token item) { Trace.mark(4); return item; }
}
''')
                (owned / "java/fixture/Trace.java").write_text('''package fixture;
public final class Trace {
    private static int sequence;
    private Trace() {}
    public static void mark(int digit) { sequence = sequence * 10 + digit; }
    public static int events() { return sequence; }
}
''')
                mode = owned / "smali/Mode.smali"
                text = mode.read_text().replace('.method static constructor <clinit>()V\n    .registers 5',
                    '.method static constructor <clinit>()V\n    .registers 9')
                text = text.replace('    invoke-direct {v4}, Lfixture/Item;-><init>()V', '''    sget-object v6, Lfixture/TokenSource;->VALUE:Lfixture/Token;
    const-string v5, "constructed"
    const-wide/16 v7, 10
    invoke-direct/range {v4 .. v8}, Lfixture/Item;-><init>(Ljava/lang/String;Lfixture/Token;J)V
    invoke-static {v5, v4}, Lfixture/Factory;->wrap(Ljava/lang/String;Lfixture/Token;)Lfixture/Token;
    move-result-object v4''')
                mode.write_text(text)
                item = owned / "smali/Item.smali"
                text = item.read_text().replace('.method public constructor <init>()V\n    .registers 2',
                    '.method public constructor <init>(Ljava/lang/String;Lfixture/Token;J)V\n    .registers 7')
                text = text.replace('    iput v0, p0, Lfixture/Item;->id:I', '''    invoke-interface {p2}, Lfixture/Token;->value()I
    move-result v1
    add-int v0, v0, v1
    long-to-int v1, p3
    add-int v0, v0, v1
    iput v0, p0, Lfixture/Item;->id:I
    const/4 v0, 3
    invoke-static {v0}, Lfixture/Trace;->mark(I)V''')
                text += '''
.method static constructor <clinit>()V
    .registers 1
    const/4 v0, 1
    invoke-static {v0}, Lfixture/Trace;->mark(I)V
    return-void
.end method
'''
                item.write_text(text)
                (owned / "smali/TokenSource.smali").write_text('''.class public final Lfixture/TokenSource;
.super Ljava/lang/Object;
.implements Lfixture/Token;
.field public static final VALUE:Lfixture/Token;
.method private constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public value()I
    .registers 2
    const/16 v0, 10
    return v0
.end method
.method static constructor <clinit>()V
    .registers 1
    new-instance v0, Lfixture/TokenSource;
    invoke-direct {v0}, Lfixture/TokenSource;-><init>()V
    sput-object v0, Lfixture/TokenSource;->VALUE:Lfixture/Token;
    const/4 v0, 2
    invoke-static {v0}, Lfixture/Trace;->mark(I)V
    return-void
.end method
''')
                (owned / "smali/Factory.smali").write_text('''.class public final Lfixture/Factory;
.super Ljava/lang/Object;
.method private constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public static wrap(Ljava/lang/String;Lfixture/Token;)Lfixture/Token;
    .registers 3
    const/4 v0, 4
    invoke-static {v0}, Lfixture/Trace;->mark(I)V
    return-object p1
.end method
''')
                (owned / "smali/Trace.smali").write_text('''.class public final Lfixture/Trace;
.super Ljava/lang/Object;
.field private static sequence:I
.method private constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public static mark(I)V
    .registers 2
    sget v0, Lfixture/Trace;->sequence:I
    mul-int/lit8 v0, v0, 10
    add-int v0, v0, p0
    sput v0, Lfixture/Trace;->sequence:I
    return-void
.end method
.method public static events()I
    .registers 1
    sget v0, Lfixture/Trace;->sequence:I
    return v0
.end method
''')
        if args.delegating_constructors:
            owned = work / "owned-input"
            shutil.copytree(fixture, owned)
            mode = owned / "java/fixture/Mode.java"
            mode.write_text(mode.read_text().replace("private Mode(int bias) { this.bias = bias; }", """private Mode(int bias) { this(bias, 13L); }
    private Mode(int bias, long tag) { this(bias, tag, ArgumentSource.next()); }
    private Mode(int bias, long tag, int counted) { this.bias = bias + counted; }"""))
            (owned / "java/fixture/ArgumentSource.java").write_text("""package fixture;
public final class ArgumentSource {
    private static int step;
    private ArgumentSource() {}
    public static int next() { return ++step; }
    public static int calls() { return step; }
}
""")
            mode = owned / "smali/Mode.smali"
            text = mode.read_text()
            start = text.index(".method private constructor <init>")
            end = text.index(".end method", start) + len(".end method")
            text = text[:start] + """.method private constructor <init>(Ljava/lang/String;II)V
    .registers 10
    .annotation system Ldalvik/annotation/Signature;
        value = { "(I)V" }
    .end annotation
    const-wide/16 v4, 13
    move-object v0, p0
    move-object v1, p1
    move v2, p2
    move v3, p3
    invoke-direct/range {v0 .. v5}, Lfixture/Mode;-><init>(Ljava/lang/String;IIJ)V
    return-void
.end method
.method private constructor <init>(Ljava/lang/String;IIJ)V
    .registers 13
    .annotation system Ldalvik/annotation/Signature;
        value = { "(IJ)V" }
    .end annotation
    invoke-static {}, Lfixture/ArgumentSource;->next()I
    move-result v6
    move-object v0, p0
    move-object v1, p1
    move v2, p2
    move v3, p3
    move-wide v4, p4
    invoke-direct/range {v0 .. v6}, Lfixture/Mode;-><init>(Ljava/lang/String;IIJI)V
    return-void
.end method
.method private constructor <init>(Ljava/lang/String;IIJI)V
    .registers 7
    .annotation system Ldalvik/annotation/Signature;
        value = { "(IJI)V" }
    .end annotation
    invoke-direct {p0, p1, p2}, Ljava/lang/Enum;-><init>(Ljava/lang/String;I)V
    add-int p3, p3, p6
    iput p3, p0, Lfixture/Mode;->bias:I
    return-void
.end method""" + text[end:]
            mode.write_text(text)
            (owned / "smali/ArgumentSource.smali").write_text(""".class public final Lfixture/ArgumentSource;
.super Ljava/lang/Object;
.field private static step:I
.method private constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public static next()I
    .registers 1
    sget v0, Lfixture/ArgumentSource;->step:I
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/ArgumentSource;->step:I
    return v0
.end method
.method public static calls()I
    .registers 1
    sget v0, Lfixture/ArgumentSource;->step:I
    return v0
.end method
""")
            fixture = owned
        if args.encoded_constants:
            owned = work / "owned-input"
            shutil.copytree(fixture, owned)
            mode = owned / "java/fixture/Mode.java"
            text = mode.read_text().replace("FIRST(3)", "FIRST(Mode.SEED)")
            text = text.replace("    private final int bias;", """    public static final int SEED = 3;
    public static final long DELAY = 0x123456789L;
    public static final String LABEL = "fixture-key";
    public static int readSeed() { return SEED; }
    public static long readDelay() { return DELAY; }
    public static String readLabel() { return LABEL; }
    private final String label;
    public String label() { return label; }
    private final int bias;""")
            text = text.replace("this.bias = bias;", "this.bias = bias; this.label = readLabel();")
            text = text.replace("    private static final EntryTable TABLE = EntryTable.create(values());",
                                "    private static final EntryTable TABLE = EntryTable.create(values());\n"
                                "    static { InitCounter.mark(); }")
            mode.write_text(text)
            (owned / "java/fixture/InitCounter.java").write_text("""package fixture;
public final class InitCounter {
    private static int count;
    private InitCounter() {}
    public static void mark() { ++count; }
    public static int value() { return count; }
}
""")
            (owned / "java/fixture/ConstantReader.java").write_text("""package fixture;
public final class ConstantReader {
    private ConstantReader() {}
    public static int value() { return Mode.readSeed(); }
    public static long delay() { return Mode.readDelay(); }
    public static String label() { return Mode.readLabel(); }
}
""")
            mode = owned / "smali/Mode.smali"
            text = mode.read_text().replace(".field private final bias:I",
                ".field private final bias:I\n.field public static final SEED:I = 3\n"
                '.field public static final DELAY:J = 0x123456789L\n'
                '.field public static final LABEL:Ljava/lang/String; = "fixture-key"\n'
                '.field private final label:Ljava/lang/String;')
            text = text.replace("    iput p3, p0, Lfixture/Mode;->bias:I",
                "    iput p3, p0, Lfixture/Mode;->bias:I\n"
                "    sget-object p1, Lfixture/Mode;->LABEL:Ljava/lang/String;\n"
                "    iput-object p1, p0, Lfixture/Mode;->label:Ljava/lang/String;")
            text = text.replace("    const/4 v3, 3", "    sget v3, Lfixture/Mode;->SEED:I")
            text = text.replace("    sput-object v0, Lfixture/Mode;->TABLE:Lfixture/EntryTable;",
                "    sput-object v0, Lfixture/Mode;->TABLE:Lfixture/EntryTable;\n"
                "    invoke-static {}, Lfixture/InitCounter;->mark()V")
            text += """
.method public static readSeed()I
    .registers 1
    sget v0, Lfixture/Mode;->SEED:I
    return v0
.end method
.method public static readDelay()J
    .registers 2
    sget-wide v0, Lfixture/Mode;->DELAY:J
    return-wide v0
.end method
.method public static readLabel()Ljava/lang/String;
    .registers 1
    sget-object v0, Lfixture/Mode;->LABEL:Ljava/lang/String;
    return-object v0
.end method
.method public label()Ljava/lang/String;
    .registers 1
    iget-object p0, p0, Lfixture/Mode;->label:Ljava/lang/String;
    return-object p0
.end method
"""
            mode.write_text(text)
            (owned / "smali/InitCounter.smali").write_text(""".class public final Lfixture/InitCounter;
.super Ljava/lang/Object;
.field private static count:I
.method private constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public static mark()V
    .registers 1
    sget v0, Lfixture/InitCounter;->count:I
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/InitCounter;->count:I
    return-void
.end method
.method public static value()I
    .registers 1
    sget v0, Lfixture/InitCounter;->count:I
    return v0
.end method
""")
            (owned / "smali/ConstantReader.smali").write_text(""".class public final Lfixture/ConstantReader;
.super Ljava/lang/Object;
.method private constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public static value()I
    .registers 1
    sget v0, Lfixture/Mode;->SEED:I
    return v0
.end method
.method public static delay()J
    .registers 2
    sget-wide v0, Lfixture/Mode;->DELAY:J
    return-wide v0
.end method
.method public static label()Ljava/lang/String;
    .registers 1
    sget-object v0, Lfixture/Mode;->LABEL:Ljava/lang/String;
    return-object v0
.end method
""")
            fixture = owned
        if args.wide_arguments:
            owned = work / "owned-input"
            shutil.copytree(fixture, owned)
            mode = owned / "java/fixture/Mode.java"
            text = mode.read_text().replace("FIRST(3), SECOND(7)",
                                            "FIRST(3, Long.MAX_VALUE), SECOND(7, FIRST.scale() * 2L)")
            text = text.replace("private final int bias;", "private final int bias;\n    private final long scale;")
            text = text.replace("private Mode(int bias) { this.bias = bias; }",
                                "private Mode(int bias, long scale) { this.bias = bias; this.scale = scale; }\n"
                                "    public long scale() { return scale; }")
            mode.write_text(text)
            mode = owned / "smali/Mode.smali"
            text = mode.read_text().replace("<init>(Ljava/lang/String;II)V", "<init>(Ljava/lang/String;IIJ)V")
            text = text.replace('"(I)V"', '"(IJ)V"')
            text = text.replace(".field private final bias:I", ".field private final bias:I\n.field private final scale:J")
            text = text.replace(".method private constructor <init>(Ljava/lang/String;IIJ)V\n    .registers 4",
                                ".method private constructor <init>(Ljava/lang/String;IIJ)V\n    .registers 6")
            text = text.replace("    iput p3, p0, Lfixture/Mode;->bias:I",
                                "    iput p3, p0, Lfixture/Mode;->bias:I\n    iput-wide p4, p0, Lfixture/Mode;->scale:J")
            text = text.replace(".method static constructor <clinit>()V\n    .registers 4",
                                ".method static constructor <clinit>()V\n    .registers 8")
            text = text.replace("    const/4 v3, 3", "    const/4 v3, 3\n    const-wide v4, 0x7fffffffffffffffL")
            text = text.replace("    const/4 v3, 7", """    const/4 v3, 7
    sget-object v4, Lfixture/Mode;->FIRST:Lfixture/Mode;
    invoke-virtual {v4}, Lfixture/Mode;->scale()J
    move-result-wide v4
    const-wide/16 v6, 2
    mul-long/2addr v4, v6""")
            text = text.replace("invoke-direct {v0, v1, v2, v3}", "invoke-direct/range {v0 .. v5}")
            text += """
.method public scale()J
    .registers 3
    iget-wide v0, p0, Lfixture/Mode;->scale:J
    return-wide v0
.end method
"""
            mode.write_text(text)
            fixture = owned
        if args.initializer_loop:
            owned = work / "owned-input"
            shutil.copytree(fixture, owned)
            mode = owned / "java/fixture/Mode.java"
            text = mode.read_text().replace("    private static final EntryTable TABLE = EntryTable.create(values());", """    private static final EntryTable TABLE = EntryTable.create(values());
    private static final int TOTAL;
    static {
        int total = 0;
        for (Mode item : values()) total += item.ordinal() + 1;
        TOTAL = total;
    }
    public static int total() { return TOTAL; }""")
            mode.write_text(text)
            mode = owned / "smali/Mode.smali"
            text = mode.read_text().replace(".field private final bias:I",
                                           ".field private static final TOTAL:I\n.field private final bias:I")
            text = text.replace(".method static constructor <clinit>()V\n    .registers 4",
                                ".method static constructor <clinit>()V\n    .registers 5\n    :prefix")
            text = text.replace("    sput-object v0, Lfixture/Mode;->TABLE:Lfixture/EntryTable;", """    sput-object v0, Lfixture/Mode;->TABLE:Lfixture/EntryTable;
    sget-object v0, Lfixture/Mode;->$VALUES:[Lfixture/Mode;
    array-length v1, v0
    const/4 v2, 0
    const/4 v3, 0
    :loop
    if-ge v2, v1, :done
    aget-object v4, v0, v2
    invoke-virtual {v4}, Ljava/lang/Enum;->ordinal()I
    move-result v4
    add-int/lit8 v4, v4, 1
    add-int v3, v3, v4
    add-int/lit8 v2, v2, 1
    goto :loop
    :done
    sput v3, Lfixture/Mode;->TOTAL:I""")
            text += """
.method public static total()I
    .registers 1
    sget v0, Lfixture/Mode;->TOTAL:I
    return v0
.end method
"""
            mode.write_text(text)
            fixture = owned
        if computed_arguments:
            owned = work / "owned-input"
            shutil.copytree(fixture, owned)
            mode = owned / "java/fixture/Mode.java"
            text = mode.read_text().replace("FIRST(3), SECOND(7)",
                "FIRST(ArgumentSource.INSTANCE.next(), ArgumentSource.INSTANCE.next()), "
                "SECOND(ArgumentSource.INSTANCE.next(), ArgumentSource.INSTANCE.next())")
            text = text.replace("private Mode(int bias) { this.bias = bias; }",
                                "private Mode(int left, int right) { this.bias = left * 10 + right; }")
            mode.write_text(text)
            (owned / "java/fixture/ArgumentSource.java").write_text("""package fixture;
public final class ArgumentSource {
    public static final ArgumentSource INSTANCE = new ArgumentSource();
    private static int step;
    private ArgumentSource() {}
    public int next() { return ++step; }
    public static int calls() { return step; }
}
""")
            mode = owned / "smali/Mode.smali"
            text = mode.read_text().replace("<init>(Ljava/lang/String;II)V", "<init>(Ljava/lang/String;III)V")
            text = text.replace(".registers 4", ".registers 5").replace('"(I)V"', '"(II)V"')
            text = text.replace("    iput p3, p0, Lfixture/Mode;->bias:I", """    mul-int/lit8 p3, p3, 10
    add-int p3, p3, p4
    iput p3, p0, Lfixture/Mode;->bias:I""")
            for value in (3, 7):
                text = text.replace("    const/4 v3, " + str(value), """    sget-object v3, Lfixture/ArgumentSource;->INSTANCE:Lfixture/ArgumentSource;
    invoke-virtual {v3}, Lfixture/ArgumentSource;->next()I
    move-result v3
    sget-object v4, Lfixture/ArgumentSource;->INSTANCE:Lfixture/ArgumentSource;
    invoke-virtual {v4}, Lfixture/ArgumentSource;->next()I
    move-result v4""")
            text = text.replace("{v0, v1, v2, v3}", "{v0, v1, v2, v3, v4}")
            mode.write_text(text)
            (owned / "smali/ArgumentSource.smali").write_text(""".class public final Lfixture/ArgumentSource;
.super Ljava/lang/Object;
.field public static final INSTANCE:Lfixture/ArgumentSource;
.field private static step:I
.method private constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method static constructor <clinit>()V
    .registers 1
    new-instance v0, Lfixture/ArgumentSource;
    invoke-direct {v0}, Lfixture/ArgumentSource;-><init>()V
    sput-object v0, Lfixture/ArgumentSource;->INSTANCE:Lfixture/ArgumentSource;
    return-void
.end method
.method public next()I
    .registers 2
    sget v0, Lfixture/ArgumentSource;->step:I
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/ArgumentSource;->step:I
    return v0
.end method
.method public static calls()I
    .registers 1
    sget v0, Lfixture/ArgumentSource;->step:I
    return v0
.end method
""")
            if args.literal_factories:
                mode = owned / "java/fixture/Mode.java"
                mode.write_text(mode.read_text().replace("ArgumentSource.INSTANCE.next()", "ArgumentSource.next(5L)"))
                source = owned / "java/fixture/ArgumentSource.java"
                source.write_text(source.read_text().replace("public int next() { return ++step; }",
                    "public static int next(long factor) { return (int) (++step * factor); }"))
                mode = owned / "smali/Mode.smali"
                text = mode.read_text().replace(".method static constructor <clinit>()V\n    .registers 5",
                                                ".method static constructor <clinit>()V\n    .registers 7")
                for reg in (3, 4):
                    text = text.replace(
                        f"    sget-object v{reg}, Lfixture/ArgumentSource;->INSTANCE:Lfixture/ArgumentSource;\n"
                        f"    invoke-virtual {{v{reg}}}, Lfixture/ArgumentSource;->next()I",
                        "    const-wide/16 v5, 5\n"
                        "    invoke-static {v5, v6}, Lfixture/ArgumentSource;->next(J)I")
                mode.write_text(text)
                source = owned / "smali/ArgumentSource.smali"
                text = source.read_text().replace(".method public next()I\n    .registers 2",
                                                  ".method public static next(J)I\n    .registers 5")
                text = text.replace("    sput v0, Lfixture/ArgumentSource;->step:I\n    return v0",
                                    "    sput v0, Lfixture/ArgumentSource;->step:I\n"
                                    "    int-to-long v0, v0\n    mul-long/2addr v0, p0\n"
                                    "    long-to-int v0, v0\n    return v0")
                source.write_text(text)
            fixture = owned
        inputs = sorted((fixture / "java").rglob("*.java"))
        harness = FIXTURES / "harness/EnumArrayHarness.java"
        if args.constructor_handlers:
            modified = work / harness.name
            text = harness.read_text().replace("value + 3", "value + 4").replace("value + 7", "value - 7")
            text = text.replace('        System.out.println("enum-array-contract:matched");',
                                '        if (fixture.Failing.calls() != 2) throw new AssertionError("Protected constructor call count changed");\n'
                                '        System.out.println("enum-array-contract:matched");')
            modified.write_text(text)
            harness = modified
        if args.conditional_constructor:
            modified = work / harness.name
            text = harness.read_text().replace("value + 3", "value + 4").replace("value + 7", "value + 6")
            text = text.replace('        System.out.println("enum-array-contract:matched");',
                '        if (Mode.FIRST.ceiling() != 30 || Mode.SECOND.ceiling() != 70\n'
                '                || Mode.FIRST.floor() != 2 || Mode.SECOND.floor() != 8)\n'
                '            throw new AssertionError("Conditional final assignments changed");\n'
                '        System.out.println("enum-array-contract:matched");')
            modified.write_text(text)
            harness = modified
        if object_arguments:
            modified = work / harness.name
            modified.write_text(harness.read_text().replace(
                '        System.out.println("enum-array-contract:matched");',
                '        if (fixture.Item.calls() != 2 || Mode.FIRST.item() == Mode.SECOND.item()\n'
                '                || Mode.FIRST.item().value() != 1 || Mode.SECOND.item().value() != 2)\n'
                '            throw new AssertionError("Object argument allocation or dispatch changed");\n'
                '        System.out.println("enum-array-contract:matched");'))
            harness = modified
            if args.object_constructor_arguments:
                text = harness.read_text().replace('item().value() != 1', 'item().value() != 21')
                text = text.replace('item().value() != 2)', 'item().value() != 22)')
                text = text.replace('        System.out.println("enum-array-contract:matched");',
                    '        if (fixture.Trace.events() != 123434) throw new AssertionError("Allocation and argument initialization order changed");\n'
                    '        System.out.println("enum-array-contract:matched");')
                harness.write_text(text)
        if args.delegating_constructors:
            modified = work / harness.name
            text = harness.read_text().replace("value + 3", "value + 4").replace("value + 7", "value + 9")
            text = text.replace('        System.out.println("enum-array-contract:matched");',
                                '        if (fixture.ArgumentSource.calls() != 2) throw new AssertionError("Constructor call count changed");\n'
                                '        System.out.println("enum-array-contract:matched");')
            modified.write_text(text)
            harness = modified
        if args.encoded_constants:
            modified = work / harness.name
            modified.write_text(harness.read_text().replace(
                "public static void main(String[] args) {", """public static void main(String[] args) {
        if (fixture.InitCounter.value() != 0) throw new AssertionError("Premature enum initialization");
        if (fixture.ConstantReader.label() != "fixture-key" || fixture.InitCounter.value() != 1
                || Mode.FIRST.label() != "fixture-key" || Mode.SECOND.label() != "fixture-key"
                || fixture.ConstantReader.delay() != 0x123456789L || fixture.InitCounter.value() != 1
                || fixture.ConstantReader.value() != 3 || fixture.InitCounter.value() != 1)
            throw new AssertionError("Encoded field reads lost active initialization");"""))
            harness = modified
        if args.wide_arguments:
            modified = work / harness.name
            modified.write_text(harness.read_text().replace(
                '        System.out.println("enum-array-contract:matched");',
                '        if (Mode.FIRST.scale() != Long.MAX_VALUE || Mode.SECOND.scale() != -2L) '
                'throw new AssertionError("Wide argument overflow changed");\n'
                '        System.out.println("enum-array-contract:matched");'))
            harness = modified
        if args.initializer_loop:
            modified = work / harness.name
            modified.write_text(harness.read_text().replace(
                '        System.out.println("enum-array-contract:matched");',
                '        if (Mode.total() != 3) throw new AssertionError("Initializer loop changed");\n'
                '        System.out.println("enum-array-contract:matched");'))
            harness = modified
        if computed_arguments:
            modified = work / harness.name
            text = harness.read_text().replace("value + 3", "value + 12").replace("value + 7", "value + 34")
            if args.literal_factories:
                text = text.replace("value + 12", "value + 60").replace("value + 34", "value + 170")
            text = text.replace('        System.out.println("enum-array-contract:matched");',
                                '        if (fixture.ArgumentSource.calls() != 4) throw new AssertionError("Argument calls repeated or omitted");\n'
                                '        System.out.println("enum-array-contract:matched");')
            modified.write_text(text)
            harness = modified
        originals, baseline_dir, rebuilt = (work / name for name in ("originals", "baseline", "rebuilt"))
        verify.compile_local(inputs, originals)
        classes, methods = compiler_inventory(verify, originals)
        verify.compile_local([*inputs, harness], baseline_dir)
        baseline = verify.run([verify.java, "-cp", baseline_dir, "EnumArrayHarness"], "baseline")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", fixture / "smali", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if (coverage["status"] != "partial" or coverage["unrecovered_method_count"] != 0
                or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods)
                or {row["identity"] for row in coverage["methods"]} != set(methods)):
            raise RuntimeError("Enum array recovery lost original identities or conservative coverage")
        enum = next(row for row in coverage["class_source_bindings"]
                    if row["binding_kind"] == "enum-declaration")
        if args.default_constructors:
            projected = {row["identity"] for row in coverage["methods"]
                         if row.get("projection_kind") == "enum-access-constructor"}
            if projected != {default_constructor}:
                raise RuntimeError("Default constructor forwarding lost its original method inventory")
        if enum.get("backing_array_projection") != "verified-read-only-copy":
            raise RuntimeError("The backing-array ownership proof is not recorded")
        if args.encoded_constants:
            if ({row["field"] for row in enum.get("encoded_constants", [])} != {"SEED", "DELAY", "LABEL"}
                    or sum(row["kind"] == "enum-encoded-value-read"
                           for row in coverage["generated_source_helpers"]) != 3):
                raise RuntimeError("Encoded values and active-read helpers have no projection evidence")
        if computed_arguments and sum(row["kind"] == "enum-argument-evaluation"
                                           for row in coverage["generated_source_helpers"]) != 4:
            raise RuntimeError("Computed argument helpers are not separately recorded")
        if args.wide_arguments and sum(row["kind"] == "enum-argument-evaluation"
                                      for row in coverage["generated_source_helpers"]) != 1:
            raise RuntimeError("Wide argument helper is not separately recorded")
        verify.compile_local([*sorted((output / "sources").rglob("*.java")), harness], rebuilt)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        if (not set(classes).issubset(rebuilt_classes)
                or not set(methods).issubset(rebuilt_methods)):
            raise RuntimeError("Enum array fixture lost original descriptors")
        actual = verify.run([verify.java, "-cp", rebuilt, "EnumArrayHarness"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Enum initializer continuation or stored-array behavior changed")
        mutations = {
            "write": """
.method public mutate()V
    .registers 3
    iget-object v0, p0, Lfixture/EntryTable;->items:[Lfixture/Mode;
    const/4 v1, 0
    aput-object v1, v0, v1
    return-void
.end method
""",
            "return": """
.method public expose()[Lfixture/Mode;
    .registers 2
    iget-object v0, p0, Lfixture/EntryTable;->items:[Lfixture/Mode;
    return-object v0
.end method
""",
            "external-call": """
.method public publish()V
    .registers 2
    iget-object v0, p0, Lfixture/EntryTable;->items:[Lfixture/Mode;
    invoke-static {v0}, Lfixture/Missing;->consume(Ljava/lang/Object;)V
    return-void
.end method
""",
            "identity": """
.method public identity()I
    .registers 2
    iget-object v0, p0, Lfixture/EntryTable;->items:[Lfixture/Mode;
    invoke-static {v0}, Ljava/lang/System;->identityHashCode(Ljava/lang/Object;)I
    move-result v0
    return v0
.end method
""",
        }
        for name, addition in mutations.items():
            mutated = work / ("rejected-" + name)
            shutil.copytree(fixture / "smali", mutated)
            source = mutated / "EntryTable.smali"
            source.write_text(source.read_text() + addition)
            destination = work / ("unexpected-" + name)
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(mutated), "-o", str(destination)],
                                    capture_output=True, text=True, timeout=300, check=False)
            (work / ("rejection-" + name + ".log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "Android enum array" not in result.stderr or destination.exists():
                raise RuntimeError("Unproven array use was accepted or published: " + name)
        if computed_arguments:
            mutated = work / "rejected-argument-reuse"
            shutil.copytree(fixture / "smali", mutated)
            source = mutated / "Mode.smali"
            source.write_text(source.read_text().replace("{v0, v1, v2, v3, v4}", "{v0, v1, v2, v3, v3}", 1))
            destination = work / "unexpected-argument-reuse"
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(mutated), "-o", str(destination)],
                                    capture_output=True, text=True, timeout=300, check=False)
            (work / "rejection-argument-reuse.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "computed enum arguments" not in result.stderr or destination.exists():
                raise RuntimeError("Duplicated or reordered argument evaluation was accepted")
        if args.initializer_loop:
            for name, replacement in {
                "branch-into-prefix": "goto :prefix",
                "loop-mutation": "aput-object v4, v0, v2\n    goto :loop",
            }.items():
                mutated = work / ("rejected-" + name)
                shutil.copytree(fixture / "smali", mutated)
                source = mutated / "Mode.smali"
                source.write_text(source.read_text().replace("goto :loop", replacement))
                destination = work / ("unexpected-" + name)
                result = subprocess.run([str(args.neverd.resolve()), "mobile", str(mutated), "-o", str(destination)],
                                        capture_output=True, text=True, timeout=300, check=False)
                (work / ("rejection-" + name + ".log")).write_text(result.stdout + result.stderr)
                if result.returncode == 0 or "Android enum" not in result.stderr or destination.exists():
                    raise RuntimeError("Unproven initializer loop was accepted: " + name)
        if args.wide_arguments:
            for name, opcode, expected in (
                ("addition", "add-long v4, v4, v6", -(1 << 63) + 1),
                ("literal-left-subtraction", "sub-long v4, v6, v4", 3 - (1 << 63)),
            ):
                changed = work / ("arithmetic-" + name)
                shutil.copytree(fixture / "smali", changed)
                source = changed / "Mode.smali"
                source.write_text(source.read_text().replace("mul-long/2addr v4, v6", opcode))
                destination = work / ("recovered-" + name)
                verify.run([args.neverd.resolve(), "mobile", changed, "-o", destination], name + "-recover")
                check = work / ("harness-" + name) / harness.name
                check.parent.mkdir()
                check.write_text(harness.read_text().replace("Mode.SECOND.scale() != -2L",
                                                            "Mode.SECOND.scale() != " + str(expected) + "L"))
                classes_dir = work / ("compiled-" + name)
                verify.compile_local([*sorted((destination / "sources").rglob("*.java")), check], classes_dir)
                verify.run([verify.java, "-cp", classes_dir, "EnumArrayHarness"], name + "-execute")
            mutated = work / "rejected-wide-carrier"
            shutil.copytree(fixture / "smali", mutated)
            source = mutated / "Mode.smali"
            source.write_text(source.read_text().replace("const-wide/16 v6, 2", "const/4 v6, 2"))
            destination = work / "unexpected-wide-carrier"
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(mutated), "-o", str(destination)],
                                    capture_output=True, text=True, timeout=300, check=False)
            (work / "rejection-wide-carrier.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "computed enum arithmetic" not in result.stderr or destination.exists():
                raise RuntimeError("Mismatched arithmetic carrier was accepted")
        if args.encoded_constants:
            for field in ("SEED:I", "LABEL:Ljava/lang/String;"):
                name = field.split(":", 1)[0].lower()
                mutated = work / ("rejected-mutable-encoded-" + name)
                shutil.copytree(fixture / "smali", mutated)
                source = mutated / "Mode.smali"
                source.write_text(source.read_text().replace(".field public static final " + field,
                                                             ".field public static " + field))
                destination = work / ("unexpected-mutable-encoded-" + name)
                result = subprocess.run([str(args.neverd.resolve()), "mobile", str(mutated), "-o", str(destination)],
                                        capture_output=True, text=True, timeout=300, check=False)
                (work / ("rejection-mutable-encoded-" + name + ".log")).write_text(result.stdout + result.stderr)
                if result.returncode == 0 or "encoded static values" not in result.stderr or destination.exists():
                    raise RuntimeError("Mutable encoded value was treated as a constant: " + name)
        if args.delegating_constructors:
            changes = {
                "cycle": ("invoke-direct/range {v0 .. v5}, Lfixture/Mode;-><init>(Ljava/lang/String;IIJ)V",
                          "invoke-direct/range {v0 .. v3}, Lfixture/Mode;-><init>(Ljava/lang/String;II)V"),
                "duplicated-result": ("    move-wide v4, p4", "    move v3, v6\n    move-wide v4, p4"),
                "discarded-result": ("    move-wide v4, p4", "    move v6, v3\n    move-wide v4, p4"),
            }
            for name, (before, after) in changes.items():
                mutated = work / ("rejected-delegation-" + name)
                shutil.copytree(fixture / "smali", mutated)
                source = mutated / "Mode.smali"
                source.write_text(source.read_text().replace(before, after))
                destination = work / ("unexpected-delegation-" + name)
                result = subprocess.run([str(args.neverd.resolve()), "mobile", str(mutated), "-o", str(destination)],
                                        capture_output=True, text=True, timeout=300, check=False)
                (work / ("rejection-delegation-" + name + ".log")).write_text(result.stdout + result.stderr)
                if result.returncode == 0 or "Android enum" not in result.stderr or destination.exists():
                    raise RuntimeError("Unproven enum constructor delegation was accepted: " + name)
        if object_arguments:
            changes = {
                "wrong-constructor": ("Mode.smali", "invoke-direct {v4}, Lfixture/Item;-><init>()V",
                                      "invoke-direct {v4}, Ljava/lang/Object;-><init>()V"),
                "uninitialized": ("Mode.smali", "invoke-direct {v4}, Lfixture/Item;-><init>()V", "nop"),
                "incompatible-interface": ("Item.smali", ".implements Lfixture/SubToken;", ""),
            }
            if args.object_constructor_arguments:
                call = 'invoke-direct/range {v4 .. v8}, Lfixture/Item;-><init>(Ljava/lang/String;Lfixture/Token;J)V'
                changes.update({
                    "wrong-constructor": ("Mode.smali", call,
                        'invoke-direct {v4}, Ljava/lang/Object;-><init>()V'),
                    "uninitialized": ("Mode.smali", call, 'nop'),
                    "broken-wide-pair": ("Mode.smali", call,
                        'invoke-direct {v4, v5, v6, v7, v7}, Lfixture/Item;-><init>(Ljava/lang/String;Lfixture/Token;J)V'),
                    "reused-object": ("Mode.smali",
                        'invoke-static {v5, v4}, Lfixture/Factory;->wrap(Ljava/lang/String;Lfixture/Token;)Lfixture/Token;',
                        'invoke-static {v4, v4}, Lfixture/Factory;->wrap(Lfixture/Token;Lfixture/Token;)Lfixture/Token;'),
                })
            for name, (filename, before, after) in changes.items():
                mutated = work / ("rejected-object-" + name)
                shutil.copytree(fixture / "smali", mutated)
                source = mutated / filename
                source.write_text(source.read_text().replace(before, after))
                destination = work / ("unexpected-object-" + name)
                result = subprocess.run([str(args.neverd.resolve()), "mobile", str(mutated), "-o", str(destination)],
                                        capture_output=True, text=True, timeout=300, check=False)
                (work / ("rejection-object-" + name + ".log")).write_text(result.stdout + result.stderr)
                error = "adjacent register pair" if name == "broken-wide-pair" else "Android enum"
                if result.returncode == 0 or error not in result.stderr or destination.exists():
                    raise RuntimeError("Unproven object argument was accepted: " + name)
        if args.conditional_constructor:
            changes = {
                "missing-write": ("    iput v0, p0, Lfixture/Mode;->floor:I", "    nop"),
                "repeated-write": ("    iput v0, p0, Lfixture/Mode;->floor:I",
                                   "    iput v0, p0, Lfixture/Mode;->floor:I\n    iput v0, p0, Lfixture/Mode;->floor:I"),
                "branch-reentry": ("    goto :done", "    goto :small"),
            }
            for name, (before, after) in changes.items():
                mutated = work / ("rejected-final-" + name)
                shutil.copytree(fixture / "smali", mutated)
                source = mutated / "Mode.smali"
                source.write_text(source.read_text().replace(before, after, 1))
                destination = work / ("unexpected-final-" + name)
                result = subprocess.run([str(args.neverd.resolve()), "mobile", str(mutated), "-o", str(destination)],
                                        capture_output=True, text=True, timeout=300, check=False)
                (work / ("rejection-final-" + name + ".log")).write_text(result.stdout + result.stderr)
                if result.returncode == 0 or "one verified initialization" not in result.stderr or destination.exists():
                    raise RuntimeError("Unproven conditional final write was accepted: " + name)
        if args.constructor_handlers:
            changes = {
                "covers-delegation": (
                    "    invoke-direct {p0, p1, p2}, Ljava/lang/Enum;-><init>(Ljava/lang/String;I)V\n    :start",
                    "    :start\n    invoke-direct {p0, p1, p2}, Ljava/lang/Enum;-><init>(Ljava/lang/String;I)V",
                    "must follow Enum initialization"),
                "normal-handler-entry": ("    goto :done", "    goto :handler", "exception-only"),
            }
            for name, (before, after, error) in changes.items():
                mutated = work / ("rejected-handler-" + name)
                shutil.copytree(fixture / "smali", mutated)
                source = mutated / "Mode.smali"
                source.write_text(source.read_text().replace(before, after, 1))
                destination = work / ("unexpected-handler-" + name)
                result = subprocess.run([str(args.neverd.resolve()), "mobile", str(mutated), "-o", str(destination)],
                                        capture_output=True, text=True, timeout=300, check=False)
                (work / ("rejection-handler-" + name + ".log")).write_text(result.stdout + result.stderr)
                if result.returncode == 0 or error not in result.stderr or destination.exists():
                    raise RuntimeError("Unproven constructor handler was accepted: " + name)
        if args.default_constructors:
            changes = {
                "hidden-mask": ("and-int/lit8 p5, p5, 2", "and-int/lit8 p5, p2, 2"),
                "skipped-delegation": ("if-eqz p5, :selected", "if-eqz p5, :finished"),
                "side-effect": ("move p4, p3", "invoke-static {}, Lfixture/Mode;->entries()Lfixture/EntryTable;"),
                "hidden-parameter": ("move p4, p3", "move-object p1, p6"),
                "unknown-mask": ("const/4 v5, 2", "sget v5, Lfixture/Mode;->MASK:I"),
            }
            for name, (before, after) in changes.items():
                mutated = work / ("rejected-default-" + name)
                shutil.copytree(fixture / "smali", mutated)
                source = mutated / "Mode.smali"
                text = source.read_text().replace(before, after)
                if name == "unknown-mask":
                    text = text.replace(".field private final bias:I", ".field private final bias:I\n.field private static MASK:I")
                source.write_text(text)
                destination = work / ("unexpected-default-" + name)
                result = subprocess.run([str(args.neverd.resolve()), "mobile", str(mutated), "-o", str(destination)],
                                        capture_output=True, text=True, timeout=300, check=False)
                (work / ("rejection-default-" + name + ".log")).write_text(result.stdout + result.stderr)
                if result.returncode == 0 or "default argument" not in result.stderr or destination.exists():
                    raise RuntimeError("Unproven default argument forwarding was accepted: " + name)
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "coverage": "partial", "jvm_output": "matched", "integer_probes": 5,
                    "initializer_continuation": "retained", "private_final_array_reads": "matched",
                    "values_array_isolation": "matched", "null_and_index_behavior": "matched",
                    "original_descriptors": "retained", "rejected_escapes": sorted(mutations)}
        if computed_arguments:
            evidence["argument_evaluation_order_and_count"] = "matched"
            evidence["reused_argument_effects"] = "rejected"
        if args.literal_factories:
            evidence["static_factory_wide_literal_arguments"] = "matched"
        if args.initializer_loop:
            evidence["initializer_loop"] = "matched"
            evidence["loop_mutation_and_prefix_reentry"] = "rejected"
        if args.wide_arguments:
            evidence["computed_long_overflow"] = "matched"
            evidence["addition_and_literal_left_subtraction"] = "matched"
            evidence["mismatched_arithmetic_carrier"] = "rejected"
        if args.encoded_constants:
            evidence["encoded_values_before_constant_allocation"] = "matched"
            evidence["active_class_initialization_on_field_read"] = "matched"
            evidence["interned_string_before_constant_allocation"] = "matched"
            evidence["mutable_encoded_value"] = "rejected"
        if args.delegating_constructors:
            evidence["private_constructor_chain"] = "retained"
            evidence["delegation_argument_call_count"] = "matched"
            evidence["cycles_and_reused_or_discarded_results"] = "rejected"
        if object_arguments:
            evidence["object_argument_initialization_and_identity"] = "matched"
            evidence["transitive_interface_assignment"] = "matched"
            evidence["invalid_initialization_or_type"] = "rejected"
        if args.object_constructor_arguments:
            evidence["allocation_argument_initialization_and_factory_order"] = "matched"
            evidence["wide_constructor_arguments"] = "matched"
            evidence["reused_constructor_effects"] = "rejected"
        if args.conditional_constructor:
            evidence["conditional_final_assignments"] = "matched"
            evidence["missing_repeated_and_reentered_writes"] = "rejected"
        if args.constructor_handlers:
            evidence["constructor_normal_and_exception_paths"] = "matched"
            evidence["delegation_and_handler_entry_boundaries"] = "rejected-invalid"
        if args.default_constructors:
            evidence["default_argument_mask_set_and_clear"] = "matched"
            evidence["invalid_guard_or_side_effect"] = "rejected"
            evidence["original_descriptors"] = "present-in-compiled-classes"
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
