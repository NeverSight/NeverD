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
    monitor-enter p1
    :body_start
    iget-object v0, p0, Lfixture/Gate;->payload:Lfixture/Payload;
    if-nez v0, :allocated
    new-instance v0, Lfixture/Payload;
    invoke-direct {v0, p1}, Lfixture/Payload;-><init>(Ljava/lang/Object;)V
    iput-object v0, p0, Lfixture/Gate;->payload:Lfixture/Payload;
    :allocated
    if-eqz p2, :called
    invoke-interface {p2}, Ljava/lang/Runnable;->run()V
    :called
    iget v0, p0, Lfixture/Gate;->count:I
    add-int/lit8 v0, v0, 1
    iput v0, p0, Lfixture/Gate;->count:I
    iget-object v0, p0, Lfixture/Gate;->failure:Ljava/lang/RuntimeException;
    if-eqz v0, :body_end
    throw v0
    :body_end
    monitor-exit p1
    return-void

    :body_failure
    move-exception v2
    :prepare_start
    const/4 v0, 1
    invoke-static {v0}, Lfixture/Cleanup;->hit(I)V
    :prepare_end
    :work_start
    const/4 v0, 2
    invoke-static {v0}, Lfixture/Cleanup;->hit(I)V
    :work_end
    :clear_start
    const/4 v0, 3
    invoke-static {v0}, Lfixture/Cleanup;->hit(I)V
    :clear_end
    goto :after_clear

    :work_failure
    move-exception v1
    const/4 v0, 3
    invoke-static {v0}, Lfixture/Cleanup;->hit(I)V
    throw v1
    :after_clear
    :abandon_start
    const/4 v0, 4
    invoke-static {v0}, Lfixture/Cleanup;->hit(I)V
    :abandon_end
    goto :rethrow_original

    :abandon_failure
    move-exception v1
    :abandon_failure_start
    const/4 v0, 4
    invoke-static {v0}, Lfixture/Cleanup;->hit(I)V
    throw v1
    :abandon_failure_end
    :rethrow_original
    throw v2
    :rethrow_end

    :release_failure
    move-exception v2
    monitor-exit p1
    throw v2
    .catchall {:body_start .. :body_end} :body_failure
    .catchall {:prepare_start .. :prepare_end} :abandon_failure
    .catchall {:work_start .. :work_end} :work_failure
    .catchall {:clear_start .. :after_clear} :abandon_failure
    .catchall {:abandon_start .. :abandon_end} :release_failure
    .catchall {:abandon_failure_start .. :abandon_failure_end} :release_failure
    .catchall {:rethrow_original .. :rethrow_end} :release_failure
.end method
