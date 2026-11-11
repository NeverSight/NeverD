.class public Lfixture/InnerHost;
.super Ljava/lang/Object;
.annotation system Ldalvik/annotation/Signature;
    value = { "<T:Ljava/lang/Object;>Ljava/lang/Object;" }
.end annotation
.annotation system Ldalvik/annotation/MemberClasses;
    value = { Lfixture/InnerHost$Member; }
.end annotation
.field public base:I

.method public constructor <init>(I)V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput p1, p0, Lfixture/InnerHost;->base:I
    return-void
.end method

.method public make(I)Lfixture/InnerHost$Member;
    .registers 3
    .annotation system Ldalvik/annotation/Signature;
        value = { "(I)Lfixture/InnerHost<TT;>.Member;" }
    .end annotation
    new-instance v0, Lfixture/InnerHost$Member;
    invoke-direct {v0, p0, p1}, Lfixture/InnerHost$Member;-><init>(Lfixture/InnerHost;I)V
    return-object v0
.end method
