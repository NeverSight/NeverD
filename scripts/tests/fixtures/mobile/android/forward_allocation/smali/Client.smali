.class public final Lfixture/Client;
.super Ljava/lang/Object;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static make(I)Lfixture/Target;
    .registers 3
    :start
    const/4 v1, 5
    invoke-static {v1}, Lfixture/Trace;->hit(I)V
    packed-switch p0, :selection
    new-instance v0, Lfixture/Target;
    goto :initialize
    :zero
    new-instance v0, Ljava/lang/IllegalStateException;
    invoke-static {}, Lfixture/Trace;->message()Ljava/lang/String;
    move-result-object v1
    invoke-direct {v0, v1}, Ljava/lang/IllegalStateException;-><init>(Ljava/lang/String;)V
    throw v0
    :one
    const/4 v0, 0
    return-object v0
    :initialize
    invoke-direct {v0}, Lfixture/Target;-><init>()V
    return-object v0
    :end
    :failure
    move-exception v0
    sput-object v0, Lfixture/Trace;->caught:Ljava/lang/RuntimeException;
    const/4 v1, 3
    invoke-static {v1}, Lfixture/Trace;->hit(I)V
    goto :one
    .catch Ljava/lang/RuntimeException; {:start .. :end} :failure
    :selection
    .packed-switch 0
        :zero
        :one
    .end packed-switch
.end method
