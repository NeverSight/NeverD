.class public Lfixture/Calculator$Nested;
.super Ljava/lang/Object;

.annotation system Ldalvik/annotation/EnclosingClass;
    value = Lfixture/Calculator;
.end annotation

.annotation system Ldalvik/annotation/InnerClass;
    accessFlags = 0x9
    name = "Nested"
.end annotation

.method public static bump(I)I
    .registers 2
    add-int/lit8 v0, p0, 0x3
    return v0
.end method
