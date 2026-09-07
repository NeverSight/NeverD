.class public Lfixture/Peer;
.super Ljava/lang/Object;

.method public static twice(I)I
    .registers 2
    mul-int/lit8 v0, p0, 0x2
    return v0
.end method

.method public static greeting()Ljava/lang/String;
    .registers 1
    const-string v0, "neverd"
    return-object v0
.end method
