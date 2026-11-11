.class public Lfixture/BoundBox;
.super Ljava/lang/Object;
.annotation system Ldalvik/annotation/Signature;
    value = { "<V::TW;W::Ljava/lang/CharSequence;>Ljava/lang/Object;" }
.end annotation

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public select(ZLjava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/CharSequence;
    .registers 4
    .annotation system Ldalvik/annotation/Signature;
        value = { "<T::TV;>(ZTT;TT;)TT;" }
    .end annotation
    if-eqz p1, :right
    return-object p2
    :right
    return-object p3
.end method
