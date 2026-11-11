.class public Lfixture/Captured;
.super Lfixture/Base;

.field public final captured:I
.field public final token:Ljava/lang/Object;
.field public final extra:I

.method public constructor <init>(ILjava/lang/Object;)V
    .registers 4
    iput p1, p0, Lfixture/Captured;->captured:I
    iput-object p2, p0, Lfixture/Captured;->token:Ljava/lang/Object;
    const/4 v0, 3
    invoke-direct {p0, v0}, Lfixture/Base;-><init>(I)V
    add-int v0, p1, v0
    iput v0, p0, Lfixture/Captured;->extra:I
    return-void
.end method

.method public value()I
    .registers 2
    iget v0, p0, Lfixture/Captured;->captured:I
    return v0
.end method
