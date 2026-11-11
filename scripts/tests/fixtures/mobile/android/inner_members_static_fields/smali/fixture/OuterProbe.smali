.class public Lfixture/OuterProbe;
.super Ljava/lang/Object;
.field public final observed:I
.field public final passed:I
.field public final passedWide:J
.field public final passedToken:Lfixture/InnerHost;

.method public constructor <init>(IJLfixture/InnerHost;)V
    .registers 9
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    sget v0, Lfixture/InnerHost;->trace:I
    mul-int/lit8 v0, v0, 10
    add-int/lit8 v0, v0, 2
    sput v0, Lfixture/InnerHost;->trace:I
    sget v0, Lfixture/StaticValues;->initial:I
    add-int/lit8 v0, v0, 7
    sput v0, Lfixture/StaticValues;->initial:I
    sget-wide v0, Lfixture/StaticValues;->wide:J
    const-wide v2, 13
    add-long v0, v0, v2
    sput-wide v0, Lfixture/StaticValues;->wide:J
    const/4 v0, 0
    sput-object v0, Lfixture/StaticValues;->token:Lfixture/InnerHost;
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
