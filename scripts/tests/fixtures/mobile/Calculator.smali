.class public Lfixture/Calculator;
.super Ljava/lang/Object;

.annotation system Ldalvik/annotation/MemberClasses;
    value = { Lfixture/Calculator$Nested; }
.end annotation

.method public static compute(I)I
    .registers 2
    invoke-static {p0}, Lfixture/Peer;->twice(I)I
    move-result v0
    invoke-static {v0}, Lfixture/Calculator$Nested;->bump(I)I
    move-result v0
    return v0
.end method

.method public static sumAbs([I)I
    .registers 5
    const/4 v0, 0x0
    const/4 v1, 0x0
    array-length v2, p0
    :loop
    if-ge v1, v2, :done
    aget v3, p0, v1
    if-gez v3, :positive
    neg-int v3, v3
    :positive
    add-int/2addr v0, v3
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    return v0
.end method

.method public static safeDivide(II)I
    .registers 3
    :try_start
    div-int v0, p0, p1
    :try_end
    .catch Ljava/lang/ArithmeticException; {:try_start .. :try_end} :handler
    return v0
    :handler
    move-exception v0
    const/4 v0, -0x1
    return v0
.end method
