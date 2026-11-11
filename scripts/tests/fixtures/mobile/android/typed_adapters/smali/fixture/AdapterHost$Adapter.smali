.class public final synthetic Lfixture/AdapterHost$Adapter;
.super Ljava/lang/Object;
.implements Lfixture/Task;
.annotation system Ldalvik/annotation/EnclosingClass;
    value = Lfixture/AdapterHost;
.end annotation
.annotation system Ldalvik/annotation/InnerClass;
    accessFlags = 0x19
    name = null
.end annotation
.field private final synthetic function:Lfixture/Callback;
    .annotation system Ldalvik/annotation/Signature;
        value = { "Lfixture/Callback<TP;Lfixture/Signal<-TR;>;Ljava/lang/Object;>;" }
    .end annotation
.end field
.method public constructor <init>(Lfixture/Callback;)V
    .registers 2
    .annotation system Ldalvik/annotation/Signature;
        value = { "(Lfixture/Callback<-TP;-Lfixture/Signal<-TR;>;+Ljava/lang/Object;>;)V" }
    .end annotation
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput-object p1, p0, Lfixture/AdapterHost$Adapter;->function:Lfixture/Callback;
    return-void
.end method
.method public final synthetic run(Lfixture/Input;Lfixture/Signal;)Ljava/lang/Object;
    .registers 4
    iget-object v0, p0, Lfixture/AdapterHost$Adapter;->function:Lfixture/Callback;
    invoke-interface {v0, p1, p2}, Lfixture/Callback;->call(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    move-result-object v0
    return-object v0
.end method
