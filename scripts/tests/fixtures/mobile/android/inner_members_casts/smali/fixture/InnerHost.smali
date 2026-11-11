.class public Lfixture/InnerHost;
.super Ljava/lang/Object;
.annotation system Ldalvik/annotation/MemberClasses;
    value = { Lfixture/InnerHost$Member; }
.end annotation
.field public base:I
.field public static trace:I

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static before()V
    .registers 1
    sget v0, Lfixture/InnerHost;->trace:I
    mul-int/lit8 v0, v0, 10
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/InnerHost;->trace:I
    return-void
.end method

.method public static after()V
    .registers 1
    sget v0, Lfixture/InnerHost;->trace:I
    mul-int/lit8 v0, v0, 10
    add-int/lit8 v0, v0, 2
    sput v0, Lfixture/InnerHost;->trace:I
    return-void
.end method

.method public make(Ljava/lang/Object;)Lfixture/InnerHost$Member;
    .registers 3
    new-instance v0, Lfixture/InnerHost$Member;
    invoke-direct {v0, p0, p1}, Lfixture/InnerHost$Member;-><init>(Lfixture/InnerHost;Ljava/lang/Object;)V
    return-object v0
.end method
