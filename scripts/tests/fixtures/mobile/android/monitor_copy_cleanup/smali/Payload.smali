.class public final Lfixture/Payload;
.super Ljava/lang/Object;
.field public anchor:Ljava/lang/Object;
.field public static partial:Lfixture/Payload;
.field public static observer:Ljava/lang/Runnable;
.method public constructor <init>(Ljava/lang/Object;)V
    .registers 3
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput-object p1, p0, Lfixture/Payload;->anchor:Ljava/lang/Object;
    sput-object p0, Lfixture/Payload;->partial:Lfixture/Payload;
    sget-object v0, Lfixture/Payload;->observer:Ljava/lang/Runnable;
    if-eqz v0, :done
    invoke-interface {v0}, Ljava/lang/Runnable;->run()V
    :done
    return-void
.end method
