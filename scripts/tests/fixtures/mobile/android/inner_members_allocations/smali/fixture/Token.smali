.class public Lfixture/Token;
.super Ljava/lang/Object;

.method static constructor <clinit>()V
    .registers 1
    sget v0, Lfixture/OuterProbe;->trace:I
    mul-int/lit8 v0, v0, 10
    add-int/lit8 v0, v0, 2
    sput v0, Lfixture/OuterProbe;->trace:I
    return-void
.end method

.method public constructor <init>()V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    sget v0, Lfixture/OuterProbe;->trace:I
    mul-int/lit8 v0, v0, 10
    add-int/lit8 v0, v0, 3
    sput v0, Lfixture/OuterProbe;->trace:I
    sget v0, Lfixture/OuterProbe;->allocations:I
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/OuterProbe;->allocations:I
    sget-boolean v0, Lfixture/OuterProbe;->fail:Z
    if-eqz v0, :done
    new-instance v0, Ljava/lang/IllegalStateException;
    invoke-direct {v0}, Ljava/lang/IllegalStateException;-><init>()V
    throw v0
    :done
    return-void
.end method
