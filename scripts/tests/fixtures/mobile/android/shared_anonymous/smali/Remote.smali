.class public Lconsumer/Remote;
.super Ljava/lang/Object;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static make(I)Ljava/lang/Object;
    .registers 2
    new-instance v0, Lfixture/SharedHost$Worker;
    invoke-direct {v0, p0}, Lfixture/SharedHost$Worker;-><init>(I)V
    return-object v0
.end method

.method public static read(Ljava/lang/Object;)I
    .registers 2
    check-cast p0, Lfixture/SharedHost$Worker;
    iget v0, p0, Lfixture/SharedHost$Worker;->value:I
    return v0
.end method
