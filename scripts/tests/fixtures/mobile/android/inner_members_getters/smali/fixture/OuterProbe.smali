.class public Lfixture/OuterProbe;
.super Ljava/lang/Object;
.field public final observed:I
.field public final passed:I
.field public final passedWide:J
.field public final passedToken:Lfixture/InnerHost;

.method public constructor <init>(IJLfixture/InnerHost;)V
    .registers 6
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    sget v0, Lfixture/InnerHost;->trace:I
    mul-int/lit8 v0, v0, 10
    add-int/lit8 v0, v0, 4
    sput v0, Lfixture/InnerHost;->trace:I
    invoke-virtual {p0}, Lfixture/OuterProbe;->readOuter()I
    move-result v0
    iput v0, p0, Lfixture/OuterProbe;->observed:I
    iput p1, p0, Lfixture/OuterProbe;->passed:I
    iput-wide p2, p0, Lfixture/OuterProbe;->passedWide:J
    iput-object p4, p0, Lfixture/OuterProbe;->passedToken:Lfixture/InnerHost;
    return-void
.end method

.method public readOuter()I
    .registers 2
    const/4 v0, 0
    return v0
.end method
