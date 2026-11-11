.class public Lfixture/OuterProbe;
.super Ljava/lang/Object;
.field public final observed:I

.method public constructor <init>()V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    invoke-virtual {p0}, Lfixture/OuterProbe;->readOuter()I
    move-result v0
    iput v0, p0, Lfixture/OuterProbe;->observed:I
    return-void
.end method

.method public readOuter()I
    .registers 2
    const/4 v0, 0
    return v0
.end method
