.class public Lfixture/Base;
.super Ljava/lang/Object;

.field public final seen:I

.method public constructor <init>(I)V
    .registers 3
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    invoke-virtual {p0}, Lfixture/Base;->value()I
    move-result v0
    add-int v0, v0, p1
    iput v0, p0, Lfixture/Base;->seen:I
    return-void
.end method

.method public value()I
    .registers 2
    const/4 v0, -1
    return v0
.end method
