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
.field public final retained:Ljava/lang/String;

.method public constructor <init>(Lfixture/InnerHost;Ljava/lang/Object;)V
    .registers 5
    .annotation system Ldalvik/annotation/Signature;
        value = { "(Ljava/lang/Object;)V" }
    .end annotation
    iput-object p1, p0, Lfixture/InnerHost$Member;->this$0:Lfixture/InnerHost;
    invoke-static {}, Lfixture/InnerHost;->before()V
    move-object v0, p2
    check-cast v0, Ljava/lang/String;
    invoke-static {}, Lfixture/InnerHost;->after()V
    move-object v1, p1
    check-cast v1, Ljava/lang/Object;
    invoke-direct {p0, v0, v1}, Lfixture/OuterProbe;-><init>(Ljava/lang/String;Ljava/lang/Object;)V
    iput-object v0, p0, Lfixture/InnerHost$Member;->retained:Ljava/lang/String;
    return-void
.end method

.method public readOuter()I
    .registers 2
    iget-object v0, p0, Lfixture/InnerHost$Member;->this$0:Lfixture/InnerHost;
    iget v0, v0, Lfixture/InnerHost;->base:I
    return v0
.end method
