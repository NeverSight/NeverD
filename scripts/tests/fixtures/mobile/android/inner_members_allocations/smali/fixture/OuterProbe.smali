.class public Lfixture/OuterProbe;
.super Ljava/lang/Object;
.field public static trace:I
.field public static constructions:I
.field public static allocations:I
.field public static fail:Z
.field public final observed:I
.field public final token:Lfixture/Token;

.method public constructor <init>(Lfixture/Token;)V
    .registers 3
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    sget v0, Lfixture/OuterProbe;->trace:I
    mul-int/lit8 v0, v0, 10
    add-int/lit8 v0, v0, 4
    sput v0, Lfixture/OuterProbe;->trace:I
    sget v0, Lfixture/OuterProbe;->constructions:I
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/OuterProbe;->constructions:I
    invoke-virtual {p0}, Lfixture/OuterProbe;->readOuter()I
    move-result v0
    iput v0, p0, Lfixture/OuterProbe;->observed:I
    iput-object p1, p0, Lfixture/OuterProbe;->token:Lfixture/Token;
    return-void
.end method

.method public static before()V
    .registers 1
    sget v0, Lfixture/OuterProbe;->trace:I
    mul-int/lit8 v0, v0, 10
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/OuterProbe;->trace:I
    return-void
.end method

.method public readOuter()I
    .registers 2
    const/4 v0, 0
    return v0
.end method
