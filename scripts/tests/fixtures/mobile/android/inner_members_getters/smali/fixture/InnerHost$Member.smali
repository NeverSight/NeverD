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
.field public final retainedWide:J
.field public final retainedToken:Lfixture/InnerHost;

.method public constructor <init>(Lfixture/InnerHost;I)V
    .registers 7
    .annotation system Ldalvik/annotation/Signature;
        value = { "(I)V" }
    .end annotation
    iput-object p1, p0, Lfixture/InnerHost$Member;->this$0:Lfixture/InnerHost;
    invoke-virtual {p1}, Lfixture/InnerHost;->readValue()I
    move-result v0
    invoke-static {}, Lfixture/InnerHost;->readWide()J
    move-result-wide v1
    invoke-virtual {p1}, Lfixture/InnerHost;->readToken()Lfixture/InnerHost;
    move-result-object v3
    invoke-direct {p0, v0, v1, v2, v3}, Lfixture/OuterProbe;-><init>(IJLfixture/InnerHost;)V
    add-int v0, v0, p2
    iput v0, p0, Lfixture/InnerHost$Member;->offset:I
    iput-wide v1, p0, Lfixture/InnerHost$Member;->retainedWide:J
    iput-object v3, p0, Lfixture/InnerHost$Member;->retainedToken:Lfixture/InnerHost;
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
