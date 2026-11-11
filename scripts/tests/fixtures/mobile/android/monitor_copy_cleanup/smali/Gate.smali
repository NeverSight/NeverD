.class public Lfixture/Gate;
.super Ljava/lang/Object;
.annotation system Ldalvik/annotation/Signature;
    value = {"<T:Ljava/lang/Object;>Ljava/lang/Object;"}
.end annotation
.field public payload:Lfixture/Payload;
.field public count:I
.field public failure:Ljava/lang/RuntimeException;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public update(Ljava/lang/Object;Ljava/lang/Runnable;)V
    .registers 16
    const/4 v0, 0
    const-wide v7, 0x123456789abcdef
    monitor-enter p1
    move-object v3, p0
    move-object v4, p1
    :first_start
    iget-object v1, v3, Lfixture/Gate;->payload:Lfixture/Payload;
    if-nez v1, :first_end
    new-instance v1, Lfixture/Payload;
    invoke-direct {v1, v4}, Lfixture/Payload;-><init>(Ljava/lang/Object;)V
    iput-object v1, v3, Lfixture/Gate;->payload:Lfixture/Payload;
    :first_end
    move-object v5, p2
    :second_start
    if-eqz v5, :second_end
    invoke-interface {v5}, Ljava/lang/Runnable;->run()V
    :second_end
    move-object v6, v3
    :third_start
    iget v0, v6, Lfixture/Gate;->count:I
    add-int/lit8 v0, v0, 1
    iput v0, v6, Lfixture/Gate;->count:I
    iget-object v1, v6, Lfixture/Gate;->failure:Ljava/lang/RuntimeException;
    if-eqz v1, :third_end
    throw v1
    :third_end
    monitor-exit p1
    return-void

    :first_failure
    move-exception v2
    move-object v11, p0
    move v12, v0
    goto :cleanup
    :second_failure
    move-exception v2
    move-object v11, v3
    move v12, v0
    goto :cleanup
    :third_failure
    move-exception v2
    move-object v11, v6
    move v12, v0
    :cleanup
    monitor-exit p1
    throw v2
    .catchall {:first_start .. :first_end} :first_failure
    .catchall {:second_start .. :second_end} :second_failure
    .catchall {:third_start .. :third_end} :third_failure
.end method
