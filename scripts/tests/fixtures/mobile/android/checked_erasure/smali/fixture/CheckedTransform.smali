.class public final Lfixture/CheckedTransform;
.super Ljava/lang/Object;
.implements Lfixture/Transform;
.annotation system Ldalvik/annotation/Signature;
    value = { "Ljava/lang/Object;Lfixture/Transform<Ljava/lang/String;Ljava/lang/String;>;" }
.end annotation
.field public static calls:I

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public final apply(Ljava/lang/Object;)Ljava/lang/String;
    .registers 4
    .annotation system Ldalvik/annotation/Signature;
        value = { "(Ljava/lang/String;)Ljava/lang/String;" }
    .end annotation
    move-object v0, p1
    check-cast v0, Ljava/lang/String;
    sget v1, Lfixture/CheckedTransform;->calls:I
    add-int/lit8 v1, v1, 1
    sput v1, Lfixture/CheckedTransform;->calls:I
    return-object v0
.end method

.method public bridge synthetic apply(Ljava/lang/Object;)Ljava/lang/Object;
    .registers 3
    invoke-virtual {p0, p1}, Lfixture/CheckedTransform;->apply(Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method
