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
.field public final retained:Lfixture/Token;

.method public constructor <init>(Lfixture/InnerHost;I)V
    .registers 4
    .annotation system Ldalvik/annotation/Signature;
        value = { "(I)V" }
    .end annotation
    iput-object p1, p0, Lfixture/InnerHost$Member;->this$0:Lfixture/InnerHost;
    invoke-static {}, Lfixture/OuterProbe;->before()V
    new-instance v0, Lfixture/Token;
    invoke-direct {v0}, Lfixture/Token;-><init>()V
    invoke-direct {p0, v0}, Lfixture/OuterProbe;-><init>(Lfixture/Token;)V
    iput p2, p0, Lfixture/InnerHost$Member;->offset:I
    iput-object v0, p0, Lfixture/InnerHost$Member;->retained:Lfixture/Token;
    return-void
.end method

.method public readOuter()I
    .registers 2
    iget-object v0, p0, Lfixture/InnerHost$Member;->this$0:Lfixture/InnerHost;
    iget v0, v0, Lfixture/InnerHost;->base:I
    return v0
.end method
