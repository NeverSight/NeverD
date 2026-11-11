.class public Lfixture/Adapter;
.super Ljava/lang/Object;

.field private final child:Lfixture/SharedHost$Worker$Child;

.method constructor <init>(Lfixture/SharedHost$Worker$Child;)V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput-object p1, p0, Lfixture/Adapter;->child:Lfixture/SharedHost$Worker$Child;
    return-void
.end method

.method public value()I
    .registers 2
    iget-object v0, p0, Lfixture/Adapter;->child:Lfixture/SharedHost$Worker$Child;
    invoke-virtual {v0}, Lfixture/SharedHost$Worker$Child;->value()I
    move-result v0
    return v0
.end method
