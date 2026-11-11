.class final Lfixture/Mode$Special;
.super Lfixture/Mode;
.annotation system Ldalvik/annotation/EnclosingClass;
    value = Lfixture/Mode;
.end annotation
.annotation system Ldalvik/annotation/InnerClass;
    accessFlags = 0x18
    name = "Special"
.end annotation

.method constructor <init>(Ljava/lang/String;II)V
    .registers 5
    .annotation system Ldalvik/annotation/Signature;
        value = { "(I)V" }
    .end annotation
    const/4 v0, 0
    invoke-direct {p0, p1, p2, p3, v0}, Lfixture/Mode;-><init>(Ljava/lang/String;IILfixture/Mode$Special;)V
    return-void
.end method

.method public score(I)I
    .registers 3
    invoke-direct {p0, p1}, Lfixture/Mode$Special;->adjust(I)I
    move-result v0
    return v0
.end method

.method private adjust(I)I
    .registers 3
    invoke-virtual {p0}, Lfixture/Mode;->bias()I
    move-result v0
    sub-int v0, p1, v0
    return v0
.end method
