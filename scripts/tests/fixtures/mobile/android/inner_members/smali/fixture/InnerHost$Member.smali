.class public final Lfixture/InnerHost$Member;
.super Lfixture/OuterProbe;
.annotation system Ldalvik/annotation/EnclosingClass;
    value = Lfixture/InnerHost;
.end annotation
.annotation system Ldalvik/annotation/InnerClass;
    accessFlags = 0x11
    name = "Member"
.end annotation
.field final synthetic this$0:Lfixture/InnerHost;
.field public final offset:I

.method public constructor <init>(Lfixture/InnerHost;I)V
    .registers 3
    .annotation system Ldalvik/annotation/Signature;
        value = { "(I)V" }
    .end annotation
    iput-object p1, p0, Lfixture/InnerHost$Member;->this$0:Lfixture/InnerHost;
    invoke-direct {p0}, Lfixture/OuterProbe;-><init>()V
    iput p2, p0, Lfixture/InnerHost$Member;->offset:I
    return-void
.end method

.method public readOuter()I
    .registers 2
    iget-object v0, p0, Lfixture/InnerHost$Member;->this$0:Lfixture/InnerHost;
    iget v0, v0, Lfixture/InnerHost;->base:I
    return v0
.end method

.method public result()I
    .registers 3
    iget v0, p0, Lfixture/OuterProbe;->observed:I
    iget v1, p0, Lfixture/InnerHost$Member;->offset:I
    add-int v0, v0, v1
    return v0
.end method
