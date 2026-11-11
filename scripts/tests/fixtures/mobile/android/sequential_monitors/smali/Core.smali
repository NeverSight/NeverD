.class public final Lfixture/Core;
.super Ljava/lang/Object;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static run(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    .registers 10
    :start
    const/4 v3, 1
    const/4 v4, 0
    invoke-static {p0, p1, p2, v3, v4}, Lfixture/Trace;->check(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;II)V
    move-object v1, p0
    monitor-enter v1
    :first_body
    const/4 v3, 2
    const/4 v4, 1
    invoke-static {p0, p1, p2, v3, v4}, Lfixture/Trace;->check(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;II)V
    sget-boolean v5, Lfixture/Trace;->early:Z
    if-eqz v5, :first_tail
    monitor-exit v1
    goto :normal_finally
    :first_tail
    monitor-exit v1
    :first_released
    const/4 v3, 3
    const/4 v4, 0
    invoke-static {p0, p1, p2, v3, v4}, Lfixture/Trace;->check(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;II)V
    sget-boolean v5, Lfixture/Trace;->skip:Z
    if-nez v5, :normal_finally
    move-object v2, p1
    monitor-enter v2
    :second_body
    const/4 v3, 4
    const/4 v4, 2
    invoke-static {p0, p1, p2, v3, v4}, Lfixture/Trace;->check(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;II)V
    monitor-exit v2
    :second_released
    const/4 v3, 5
    const/4 v4, 0
    invoke-static {p0, p1, p2, v3, v4}, Lfixture/Trace;->check(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;II)V
    goto :normal_finally
    :second_failure
    move-exception v0
    :second_exit
    monitor-exit v2
    :second_rethrow
    throw v0
    :first_failure
    move-exception v0
    :first_exit
    monitor-exit v1
    :first_rethrow
    throw v0
    :normal_finally
    const/4 v3, 6
    const/4 v4, 0
    invoke-static {p0, p1, p2, v3, v4}, Lfixture/Trace;->check(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;II)V
    return-object p2
    :outer_failure
    move-exception v0
    const/4 v3, 6
    const/4 v4, 0
    invoke-static {p0, p1, p2, v3, v4}, Lfixture/Trace;->check(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;II)V
    throw v0
    .catchall {:start .. :first_body} :outer_failure
    .catchall {:first_body .. :first_released} :first_failure
    .catchall {:first_released .. :second_body} :outer_failure
    .catchall {:second_body .. :second_released} :second_failure
    .catchall {:second_released .. :second_exit} :outer_failure
    .catchall {:second_exit .. :second_rethrow} :second_failure
    .catchall {:second_rethrow .. :first_failure} :outer_failure
    .catchall {:first_exit .. :first_rethrow} :first_failure
    .catchall {:first_rethrow .. :normal_finally} :outer_failure
.end method
