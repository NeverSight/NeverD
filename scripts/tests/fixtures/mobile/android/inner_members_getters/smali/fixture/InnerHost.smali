.class public Lfixture/InnerHost;
.super Ljava/lang/Object;
.field public static trace:I
.field public base:I

.method public constructor <init>(I)V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput p1, p0, Lfixture/InnerHost;->base:I
    return-void
.end method

.method public readValue()I
    .registers 3
    sget v0, Lfixture/InnerHost;->trace:I
    mul-int/lit8 v0, v0, 10
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/InnerHost;->trace:I
    iget v0, p0, Lfixture/InnerHost;->base:I
    const/16 v1, 42
    if-ne v0, v1, :success
    new-instance v0, Ljava/lang/IllegalStateException;
    invoke-direct {v0}, Ljava/lang/IllegalStateException;-><init>()V
    throw v0
    :success
    return v0
.end method

.method public static readWide()J
    .registers 2
    sget v0, Lfixture/InnerHost;->trace:I
    mul-int/lit8 v0, v0, 10
    add-int/lit8 v0, v0, 2
    sput v0, Lfixture/InnerHost;->trace:I
    const-wide v0, 0x12345678abcdef01
    return-wide v0
.end method

.method public readToken()Lfixture/InnerHost;
    .registers 2
    sget v0, Lfixture/InnerHost;->trace:I
    mul-int/lit8 v0, v0, 10
    add-int/lit8 v0, v0, 3
    sput v0, Lfixture/InnerHost;->trace:I
    return-object p0
.end method

.method public make(I)Lfixture/InnerHost$Member;
    .registers 3
    new-instance v0, Lfixture/InnerHost$Member;
    invoke-direct {v0, p0, p1}, Lfixture/InnerHost$Member;-><init>(Lfixture/InnerHost;I)V
    return-object v0
.end method
