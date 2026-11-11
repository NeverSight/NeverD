.class final Lfixture/SharedHost$Worker$Child;
.super Ljava/lang/Object;

.annotation system Ldalvik/annotation/EnclosingMethod;
    value = Lfixture/SharedHost$Worker;->nested()Ljava/lang/Object;
.end annotation
.annotation system Ldalvik/annotation/InnerClass;
    accessFlags = 0x10
    name = null
.end annotation

.field final parent:Lfixture/SharedHost$Worker;

.method constructor <init>(Lfixture/SharedHost$Worker;)V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput-object p1, p0, Lfixture/SharedHost$Worker$Child;->parent:Lfixture/SharedHost$Worker;
    return-void
.end method

.method value()I
    .registers 2
    iget-object v0, p0, Lfixture/SharedHost$Worker$Child;->parent:Lfixture/SharedHost$Worker;
    iget v0, v0, Lfixture/SharedHost$Worker;->value:I
    return v0
.end method
