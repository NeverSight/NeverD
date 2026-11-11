.class public Lfixture/Outer;
.super Ljava/lang/Object;
.field public static getters:I
.field public static constructions:I
.field public limit:I
.field public fail:Z
.field public final sentinel:Ljava/lang/IllegalStateException;
.method public constructor <init>()V
    .registers 4
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    new-instance v0, Ljava/lang/IllegalStateException;
    const-string v1, "owned getter failure"
    invoke-direct {v0, v1}, Ljava/lang/IllegalStateException;-><init>(Ljava/lang/String;)V
    iput-object v0, p0, Lfixture/Outer;->sentinel:Ljava/lang/IllegalStateException;
    return-void
.end method
.method public limit()I
    .registers 2
    sget v0, Lfixture/Outer;->getters:I
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/Outer;->getters:I
    iget-boolean v0, p0, Lfixture/Outer;->fail:Z
    if-eqz v0, :read
    iget-object v0, p0, Lfixture/Outer;->sentinel:Ljava/lang/IllegalStateException;
    throw v0
    :read
    iget v0, p0, Lfixture/Outer;->limit:I
    return v0
.end method
